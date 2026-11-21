#!/bin/bash
# PR 1.3: NVMe-oF TCP failover on a sock dev cluster.
#
# While the kernel initiator is writing an exported image, kill one replica
# OSD and then the raft leader. A 3-replica group has no quorum after both
# are down, so I/O is expected to fail with retryable errors in that window.
# Restart the OSDs and require:
#   - no fio/nvmf/osd process stays in D-state
#   - the nvmf reactor still answers nvmf_get_subsystems
#   - a full pattern write+read of the exported device matches
#   - block_bench write-read-verify passes on a second image
#
# The OSD data path stays on msg_transport from vstart (sock when this box
# has no RNIC). The NVMe-oF listener is TCP. RDMA对照 is PR 1.4.
#
#   ./scripts/run-nvmf-failover-regression.sh
#   NVMF_START_CLUSTER=0 ./scripts/run-nvmf-failover-regression.sh
#
# The pool must have a single PG so one leader owns the whole image.
# bdev_fastblock_create is sent as a raw JSON-RPC: rpc.py adds pool_id=null,
# which this server's decoder rejects.
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
state_root="${FASTBLOCK_VSTART_STATE_ROOT:-$script_root/.vstart}"
conf="${FASTBLOCK_CONF:-$state_root/etc/fastblock/fastblock.json}"
bench_conf="${FASTBLOCK_BENCH_CONF:-$state_root/etc/fastblock/block_bench.dev.json}"
log_dir="${FASTBLOCK_LOG_DIR:-$state_root/var/log/fastblock}"
run_dir="$state_root/run"
data_dir="${FASTBLOCK_DATA_DIR:-$state_root/var/lib/fastblock}"
client_bin="${FASTBLOCK_CLIENT_BIN:-$script_root/monitor/fastblock-client}"
osd_bin="${FASTBLOCK_OSD_BIN:-$script_root/build/src/osd/fastblock-osd}"
nvmf_bin="${FASTBLOCK_NVMF_BIN:-$script_root/build/src/bdev/fastblock-nvmf-tgt}"
spdk_python="${FASTBLOCK_SPDK_PYTHON:-$script_root/build/deps_build/spdk-prefix/src/spdk/python}"

osd_count="${NVMF_OSD_COUNT:-3}"
replica="${NVMF_REPLICA:-3}"
pg_count="${NVMF_PG_COUNT:-1}"
start_cluster="${NVMF_START_CLUSTER:-1}"
ready_timeout="${NVMF_READY_TIMEOUT:-300}"
image_name="${NVMF_IMAGE_NAME:-nvmf-failover}"
image_bytes="${NVMF_IMAGE_BYTES:-33554432}"
object_bytes="${NVMF_OBJECT_BYTES:-4194304}"
block_bytes=4096
pool_name="${NVMF_POOL:-fb}"
nqn="${NVMF_NQN:-nqn.2016-06.io.spdk:cnode1}"
serial="${NVMF_SERIAL:-SPDK00000000000001}"
listen_addr="${NVMF_LISTEN_ADDR:-127.0.0.1}"
listen_port="${NVMF_LISTEN_PORT:-4420}"
bdev_name="${NVMF_BDEV_NAME:-nvmf_fbdev}"
nvmf_mem_mb="${NVMF_MEM_MB:-2048}"
pg_id="${NVMF_PG_ID:-0}"

export FASTBLOCK_VSTART_AIO_FILE_SIZE="${FASTBLOCK_VSTART_AIO_FILE_SIZE:-2G}"
export FASTBLOCK_VSTART_SPDK_ARGS="${FASTBLOCK_VSTART_SPDK_ARGS:---no-huge}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

fio_pid=""
nvmf_pid=""
nvmf_sock=""
dev=""
started_cluster=0
pool_id=""
log_mark="$log_dir/nvmf-failover-logmark"
leader_mark="$log_dir/nvmf-failover-leadermark"

require_exec() {
    if [ ! -x "$1" ]; then
        echo "required executable not found: $1" >&2
        exit 1
    fi
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "required command not found: $1" >&2
        exit 1
    fi
}

step() {
    echo
    echo "== $* =="
}

cluster_status() {
    "$client_bin" -conf="$conf" -op=status 2>/dev/null || true
}

cluster_ready() {
    local st="$1"
    grep -qE "[[:space:]]${osd_count} up" <<<"$st" || return 1
    grep -qE "^[[:space:]]+${pg_count}[[:space:]]+active" <<<"$st" || return 1
    return 0
}

wait_ready() {
    local label="$1"
    local deadline=$((SECONDS + ready_timeout))
    local st=""
    echo "Waiting for cluster health: $label"
    while [ "$SECONDS" -lt "$deadline" ]; do
        st="$(cluster_status)"
        if cluster_ready "$st"; then
            printf "%s\n" "$st"
            echo "cluster ready: ${osd_count} osd up, ${pg_count} pg active"
            return 0
        fi
        sleep 5
    done
    echo "cluster not ready within ${ready_timeout}s ($label)" >&2
    printf "%s\n" "$st" >&2
    exit 1
}

proc_state() {
    local pid="$1"
    local stat rest
    stat="$(cat "/proc/$pid/stat" 2>/dev/null)" || {
        echo gone
        return 0
    }
    rest="${stat#*)}"
    # Field after ") " is the process state.
    printf "%s" "${rest:1:1}"
}

live_osd_pids() {
    local f pid
    for f in "$run_dir"/osd-*.pid; do
        [ -f "$f" ] || continue
        pid="$(cat "$f" 2>/dev/null || true)"
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            printf "%s\n" "$pid"
        fi
    done
}

# Fail only when the same process stays in D across every sample. AIO reactors
# enter D while a write is in the kernel, so different pids can each show D
# on a different sample during recovery. A stuck reactor does not leave D.
assert_not_stuck() {
    local label="$1"
    shift
    local pids=("$@")
    local i pid st any
    declare -A d_hits=()
    for pid in "${pids[@]}"; do
        [ -n "$pid" ] || continue
        d_hits["$pid"]=0
    done
    for i in 1 2 3 4; do
        any=0
        for pid in "${pids[@]}"; do
            [ -n "$pid" ] || continue
            kill -0 "$pid" 2>/dev/null || continue
            st="$(proc_state "$pid")"
            if [ "$st" = "D" ]; then
                d_hits["$pid"]=$(( ${d_hits[$pid]:-0} + 1 ))
                echo "$label: pid $pid state D (sample $i)"
                any=1
            else
                d_hits["$pid"]=0
            fi
        done
        if [ "$any" -eq 0 ]; then
            echo "$label: no D-state"
            return 0
        fi
        sleep 3
    done
    any=0
    for pid in "${!d_hits[@]}"; do
        if [ "${d_hits[$pid]}" -ge 4 ]; then
            echo "$label: pid $pid stayed in D across all samples" >&2
            any=1
        fi
    done
    if [ "$any" -eq 0 ]; then
        echo "$label: D-state did not stick to one process"
        return 0
    fi
    echo "process stuck in D-state during $label" >&2
    ps -eo stat,pid,comm | awk '$1 ~ /^D/ {print}' >&2 || true
    exit 1
}

rpc_call() {
    local method="$1"
    local params="$2"
    local timeout="${3:-60}"
    PYTHONPATH="$spdk_python${PYTHONPATH:+:$PYTHONPATH}" \
        python3 - "$nvmf_sock" "$method" "$params" "$timeout" <<'PY'
import json
import sys
from spdk.rpc.client import JSONRPCClient

sock, method, params, timeout = sys.argv[1:]
with JSONRPCClient(sock, timeout=float(timeout)) as client:
    result = client.call(method, json.loads(params))
json.dump(result, sys.stdout)
sys.stdout.write("\n")
PY
}

assert_reactor() {
    local label="$1"
    local out=""
    if ! out="$(rpc_call nvmf_get_subsystems '{}' 8)"; then
        echo "nvmf reactor did not answer during $label" >&2
        exit 1
    fi
    if ! grep -q "$nqn" <<<"$out"; then
        echo "nvmf reactor answered without $nqn during $label" >&2
        printf "%s\n" "$out" >&2
        exit 1
    fi
    echo "reactor alive during $label"
}

snapshot_logs() {
    local dest="$1"
    mkdir -p "$log_dir"
    : > "$dest"
    local f
    for f in "$log_dir"/osd*.log; do
        [ -f "$f" ] || continue
        wc -c < "$f" | awk -v path="$f" '{print path, $1}'
    done > "$dest"
}

new_leader_since_mark() {
    local f bytes base id
    [ -f "$leader_mark" ] || return 1
    while read -r f bytes; do
        [ -f "$f" ] || continue
        if tail -c +$((bytes + 1)) "$f" \
            | grep -q -E "becoming leader of pg ${pool_id}\\.${pg_id}( |$)"; then
            base="$(basename "$f")"
            id="${base#osd}"
            id="${id%.log}"
            printf "%s" "$id"
            return 0
        fi
    done < "$leader_mark"
    return 1
}

find_pg_leader() {
    local deadline=$((SECONDS + 90))
    local f base id n
    while [ "$SECONDS" -lt "$deadline" ]; do
        n=0
        id=""
        for f in "$log_dir"/osd*.log; do
            [ -f "$f" ] || continue
            if grep -q -E "becoming leader of pg ${pool_id}\\.${pg_id}( |$)" "$f"; then
                n=$((n + 1))
                base="$(basename "$f")"
                id="${base#osd}"
                id="${id%.log}"
            fi
        done
        if [ "$n" -eq 1 ]; then
            printf "%s" "$id"
            return 0
        fi
        if [ "$n" -gt 1 ]; then
            f="$(grep -l -E "becoming leader of pg ${pool_id}\\.${pg_id}( |$)" "$log_dir"/osd*.log | xargs ls -t | head -1)"
            base="$(basename "$f")"
            id="${base#osd}"
            id="${id%.log}"
            printf "%s" "$id"
            return 0
        fi
        sleep 1
    done
    echo "failed to derive leader for PG ${pool_id}.${pg_id}" >&2
    exit 1
}

load_pg_members() {
    local line
    line="$("$client_bin" -conf="$conf" -op=getpgmap | awk -v pg="${pool_id}.${pg_id}" '$1 == pg {print}')"
    if [ -z "$line" ]; then
        echo "PG ${pool_id}.${pg_id} not in pg map" >&2
        "$client_bin" -conf="$conf" -op=getpgmap >&2 || true
        exit 1
    fi
    echo "pg map: $line"
    local members
    members="$(awk '{print $4}' <<<"$line" | tr -d '[]' | tr ',' ' ')"
    # shellcheck disable=SC2206
    pg_members=($members)
    if [ "${#pg_members[@]}" -lt 2 ]; then
        echo "PG ${pool_id}.${pg_id} has no replica to kill" >&2
        exit 1
    fi
}

pick_replica() {
    local leader="$1"
    local member
    for member in "${pg_members[@]}"; do
        if [ "$member" != "$leader" ]; then
            printf "%s" "$member"
            return 0
        fi
    done
    echo "failed to pick a non-leader replica" >&2
    exit 1
}

kill_osd() {
    local id="$1"
    local pid=""
    pid="$(cat "$run_dir/osd-${id}.pid" 2>/dev/null || true)"
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        pid="$(pgrep -f "fastblock-os[d].*--id ${id}( |$)" | head -1 || true)"
    fi
    if [ -n "$pid" ]; then
        echo "killing osd $id pid $pid"
        kill -9 "$pid" 2>/dev/null || true
    else
        echo "osd $id has no live pid" >&2
        exit 1
    fi
    rm -f "$run_dir/osd-${id}.pid"
}

start_osd() {
    local id="$1"
    echo "starting osd $id"
    # shellcheck disable=SC2086
    nohup "$osd_bin" -C "$conf" --id "$id" -N 0 ${FASTBLOCK_VSTART_SPDK_ARGS} \
        >> "$log_dir/osd${id}.log" 2>&1 &
    echo $! > "$run_dir/osd-${id}.pid"
}

stop_cluster() {
    local f pid
    if [ ! -d "$run_dir" ]; then
        return 0
    fi
    for f in "$run_dir"/osd-*.pid "$run_dir"/monitor.pid; do
        [ -f "$f" ] || continue
        pid="$(cat "$f" 2>/dev/null || true)"
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
        rm -f "$f"
    done
}

cleanup() {
    local rc=$?
    set +e
    if [ -n "$fio_pid" ] && kill -0 "$fio_pid" 2>/dev/null; then
        kill "$fio_pid" 2>/dev/null || true
        wait "$fio_pid" 2>/dev/null || true
    fi
    nvme disconnect -n "$nqn" >/dev/null 2>&1 || true
    if [ -n "$nvmf_pid" ] && kill -0 "$nvmf_pid" 2>/dev/null; then
        kill "$nvmf_pid" 2>/dev/null || true
        local i=0
        while kill -0 "$nvmf_pid" 2>/dev/null && [ "$i" -lt 20 ]; do
            sleep 1
            i=$((i + 1))
        done
        kill -9 "$nvmf_pid" 2>/dev/null || true
    fi
    if [ "$started_cluster" = 1 ]; then
        stop_cluster
    fi
    if [ "$rc" -ne 0 ]; then
        echo "nvmf failover regression failed (exit $rc). logs: $log_dir" >&2
    fi
}
trap cleanup EXIT

find_nvme_dev() {
    local json text
    json="$(nvme list -o json 2>/dev/null || true)"
    if [ -n "$json" ]; then
        dev="$(printf "%s" "$json" | jq -r --arg sn "$serial" '
            (.. | objects | select(.SerialNumber? == $sn) | .DevicePath) // empty
        ' 2>/dev/null | head -1 || true)"
    fi
    if [ -z "$dev" ]; then
        text="$(nvme list 2>/dev/null || true)"
        dev="$(awk -v sn="$serial" 'index($0, sn) {print $1; exit}' <<<"$text")"
    fi
    [ -n "$dev" ] && [ -b "$dev" ]
}

wait_nvme_dev() {
    local i
    for i in $(seq 1 30); do
        if find_nvme_dev; then
            echo "initiator device $dev serial $serial"
            return 0
        fi
        sleep 1
    done
    echo "nvme device with serial $serial did not appear" >&2
    nvme list >&2 || true
    exit 1
}

run_fio() {
    local name="$1"
    shift
    timeout 600 fio \
        --name="$name" \
        --filename="$dev" \
        --size="$image_bytes" \
        --bs=4k \
        --iodepth=8 \
        --ioengine=libaio \
        --direct=1 \
        --numjobs=1 \
        --group_reporting=1 \
        "$@"
}

check_no_new_cores() {
    local now
    if [ ! -d /var/lib/systemd/coredump ]; then
        return 0
    fi
    now="$(find /var/lib/systemd/coredump -maxdepth 1 -name 'core.*' -newer "$log_mark" -print 2>/dev/null || true)"
    if [ -n "$now" ]; then
        echo "new core dump during the regression:" >&2
        printf "%s\n" "$now" >&2
        exit 1
    fi
}

check_osd_logs() {
    local f bytes
    local hit=0
    [ -f "$log_mark" ] || return 0
    while read -r f bytes; do
        [ -f "$f" ] || continue
        if tail -c +$((bytes + 1)) "$f" \
            | grep -E -n "SIGSEGV|segmentation|core dumped|Assertion " >&2; then
            echo "fatal message in $f" >&2
            hit=1
        fi
    done < "$log_mark"
    if [ "$hit" -ne 0 ]; then
        exit 1
    fi
}

require_exec "$client_bin"
require_exec "$osd_bin"
require_exec "$nvmf_bin"
require_exec "$script_root/scripts/run-dev-write-read-verify.sh"
require_cmd nvme
require_cmd fio
require_cmd python3
require_cmd jq
require_cmd modprobe
if [ ! -d "$spdk_python/spdk/rpc" ]; then
    echo "SPDK python rpc client not found: $spdk_python" >&2
    exit 1
fi
if [ "$pg_count" -ne 1 ]; then
    echo "this regression needs one PG so a single leader owns the image" >&2
    exit 1
fi
if [ "$(nproc)" -lt $((osd_count + 2)) ]; then
    echo "need at least $((osd_count + 2)) cpus (osds + nvmf + block_bench)" >&2
    exit 1
fi

if [ ! -d /sys/class/infiniband ] || [ -z "$(ls -A /sys/class/infiniband 2>/dev/null || true)" ]; then
    echo "rdma对照: skipped, no RNIC (PR 1.4)"
fi

if [ "$start_cluster" = 1 ]; then
    step "bring up ${osd_count}-osd sock cluster (1 pg, aio ${FASTBLOCK_VSTART_AIO_FILE_SIZE})"
    mkdir -p "$data_dir"
    for leftover in "$data_dir"/osd-*; do
        [ -d "$leftover" ] || continue
        lid="${leftover##*-}"
        if [ "$lid" -gt "$osd_count" ]; then
            echo "removing leftover $leftover"
            rm -rf "$leftover"
        fi
    done
    started_cluster=1
    (
        cd "$script_root"
        timeout 900 ./vstart.sh -m dev -c "$osd_count" -r "$replica" -p "$pg_count"
    )
fi

if [ ! -f "$conf" ]; then
    echo "cluster config not found: $conf" >&2
    exit 1
fi

step "wait for cluster health"
wait_ready "before export"
snapshot_logs "$log_mark"

pool_id="$("$client_bin" -conf="$conf" -op=listpools | sed -n "s/.*poolid: \\([0-9][0-9]*\\), name: ${pool_name},.*/\\1/p" | head -1)"
if [ -z "$pool_id" ]; then
    echo "pool $pool_name not found" >&2
    "$client_bin" -conf="$conf" -op=listpools >&2 || true
    exit 1
fi
echo "pool $pool_name id $pool_id"

msg_transport="$(jq -r '.msg_transport // "?"' "$conf")"
mon_host="$(jq -r '.mon_host[0]' "$conf")"
mon_port="$(jq -r '.mon_rpc_port // 3333' "$conf")"
echo "msg_transport=$msg_transport monitor=${mon_host}:${mon_port}"

step "create image $image_name ($image_bytes bytes)"
create_out="$("$client_bin" -conf="$conf" -op=createimage \
    -poolname="$pool_name" -imagename="$image_name" \
    -imagesize="$image_bytes" -objectsize="$object_bytes")"
printf "%s\n" "$create_out"
if ! grep -q "Image is created" <<<"$create_out"; then
    echo "createimage failed" >&2
    exit 1
fi

step "start fastblock-nvmf-tgt"
mkdir -p "$log_dir"
nohup "$nvmf_bin" --no-huge -s "$nvmf_mem_mb" -C "$conf" -S 1 \
    >> "$log_dir/nvmf.log" 2>&1 &
nvmf_pid=$!
echo "nvmf pid $nvmf_pid"
nvmf_sock="/var/tmp/fastblock_nvmf_tgt${nvmf_pid}.sock"
deadline=$((SECONDS + 90))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -S "$nvmf_sock" ]; then
        break
    fi
    if ! kill -0 "$nvmf_pid" 2>/dev/null; then
        echo "nvmf-tgt exited before the rpc socket appeared" >&2
        tail -40 "$log_dir/nvmf.log" >&2 || true
        exit 1
    fi
    sleep 1
done
if [ ! -S "$nvmf_sock" ]; then
    echo "rpc socket $nvmf_sock did not appear" >&2
    tail -40 "$log_dir/nvmf.log" >&2 || true
    exit 1
fi
echo "nvmf rpc $nvmf_sock"

step "export $pool_name/$image_name over NVMe-oF TCP ${listen_addr}:${listen_port}"
bdev_params="$(jq -n \
    --arg name "$bdev_name" \
    --arg pool "$pool_name" \
    --arg image "$image_name" \
    --arg mon "${mon_host}:${mon_port}" \
    --argjson image_size "$image_bytes" \
    --argjson object_size "$object_bytes" \
    --argjson block_size "$block_bytes" \
    '{name:$name, pool_name:$pool, image_name:$image, image_size:$image_size,
      object_size:$object_size, block_size:$block_size, monitor_address:$mon}')"
bdev_ok=0
for attempt in 1 2 3 4 5 6; do
    if rpc_call bdev_fastblock_create "$bdev_params" 120; then
        bdev_ok=1
        break
    fi
    echo "bdev_fastblock_create attempt $attempt failed; waiting for the monitor map" >&2
    sleep 5
done
if [ "$bdev_ok" -ne 1 ]; then
    echo "bdev_fastblock_create failed" >&2
    exit 1
fi
rpc_call nvmf_create_transport '{"trtype":"TCP","io_unit_size":16384,"max_io_qpairs_per_ctrlr":8,"in_capsule_data_size":8192}'
rpc_call nvmf_create_subsystem "$(jq -n --arg nqn "$nqn" --arg sn "$serial" \
    '{nqn:$nqn, allow_any_host:true, serial_number:$sn, model_number:"SPDK_Controller1"}')"
rpc_call nvmf_subsystem_add_ns "$(jq -n --arg nqn "$nqn" --arg bdev "$bdev_name" \
    '{nqn:$nqn, namespace:{bdev_name:$bdev}}')"
rpc_call nvmf_subsystem_add_listener "$(jq -n --arg nqn "$nqn" --arg addr "$listen_addr" --arg port "$listen_port" \
    '{nqn:$nqn, listen_address:{trtype:"TCP", traddr:$addr, trsvcid:$port, adrfam:"ipv4"}}')"
assert_reactor "after export"

modprobe nvme-tcp
nvme connect -t tcp -n "$nqn" -a "$listen_addr" -s "$listen_port"
wait_nvme_dev
if command -v blockdev >/dev/null 2>&1; then
    dev_bytes="$(blockdev --getsize64 "$dev" || echo 0)"
    if [ "$dev_bytes" -gt 0 ] && [ "$dev_bytes" -lt "$image_bytes" ]; then
        echo "namespace is $dev_bytes bytes; fio will use that"
        image_bytes="$dev_bytes"
    fi
fi

step "baseline pattern write+verify before faults"
run_fio nvmf_base --rw=write --verify=pattern --verify_pattern=0x5a --do_verify=1

step "start background writes"
fio --name=nvmf_storm \
    --filename="$dev" \
    --size="$image_bytes" \
    --bs=4k \
    --iodepth=16 \
    --ioengine=libaio \
    --direct=1 \
    --rw=randwrite \
    --runtime=180 \
    --time_based=1 \
    --continue_on_error=io \
    --status-interval=5 \
    --group_reporting=1 \
    --output="$log_dir/nvmf-storm.fio" \
    >/dev/null 2>&1 &
fio_pid=$!
sleep 5
if ! kill -0 "$fio_pid" 2>/dev/null; then
    echo "background fio exited before the first fault" >&2
    cat "$log_dir/nvmf-storm.fio" >&2 || true
    exit 1
fi

load_pg_members
leader="$(find_pg_leader)"
replica_victim="$(pick_replica "$leader")"
echo "leader osd$leader  replica victim osd$replica_victim"

step "kill replica osd$replica_victim during writes"
kill_osd "$replica_victim"
sleep 5
if ! kill -0 "$fio_pid" 2>/dev/null; then
    echo "fio died after killing one replica; majority should still serve writes" >&2
    cat "$log_dir/nvmf-storm.fio" >&2 || true
    exit 1
fi
assert_not_stuck "after replica kill" "$fio_pid" "$nvmf_pid" $(live_osd_pids)
assert_reactor "after replica kill"

step "kill leader osd$leader during writes"
snapshot_logs "$leader_mark"
kill_osd "$leader"
sleep 8
assert_not_stuck "after leader kill" "$fio_pid" "$nvmf_pid" $(live_osd_pids)
assert_reactor "after leader kill"

step "restart killed osds"
start_osd "$replica_victim"
start_osd "$leader"
echo "waiting for a new raft leader"
deadline=$((SECONDS + 120))
new_leader=""
while [ "$SECONDS" -lt "$deadline" ]; do
    if new_leader="$(new_leader_since_mark)"; then
        break
    fi
    sleep 2
done
if [ -z "$new_leader" ]; then
    echo "no new leader for PG ${pool_id}.${pg_id} after restart" >&2
    exit 1
fi
echo "leader after restart: osd$new_leader"
wait_ready "after osd restart"

assert_not_stuck "after restart" "$fio_pid" "$nvmf_pid" $(live_osd_pids)
assert_reactor "after restart"

step "stop the in-flight writer and confirm I/O resumed"
if kill -0 "$fio_pid" 2>/dev/null; then
    kill "$fio_pid" 2>/dev/null || true
    wait "$fio_pid" 2>/dev/null || true
fi
fio_pid=""
# The storm job is allowed to have seen errors while quorum was gone.
# A fresh short write must succeed once the OSDs are back.
recover_ok=0
for attempt in 1 2 3; do
    if run_fio nvmf_recover --rw=randwrite --runtime=10 --time_based=1; then
        recover_ok=1
        break
    fi
    echo "post-recovery write attempt $attempt failed; retrying" >&2
    sleep 10
done
if [ "$recover_ok" -ne 1 ]; then
    echo "I/O did not resume through NVMe-oF after OSD restart" >&2
    exit 1
fi

step "final pattern write+verify on the exported device"
run_fio nvmf_final --rw=write --verify=pattern --verify_pattern=0xa5 --do_verify=1

step "disconnect initiator and stop nvmf"
nvme disconnect -n "$nqn" || true
dev=""
if [ -n "$nvmf_pid" ] && kill -0 "$nvmf_pid" 2>/dev/null; then
    kill "$nvmf_pid" 2>/dev/null || true
    wait "$nvmf_pid" 2>/dev/null || true
fi
nvmf_pid=""

step "block_bench write-read-verify"
# OSDs hold the low cores. nvmf has exited, so the next core is free.
export BLOCK_BENCH_CPU_MASK="${BLOCK_BENCH_CPU_MASK:-$(printf '0x%x' $((1 << osd_count)))}"
export BLOCK_BENCH_IMAGE_NAME="${BLOCK_BENCH_IMAGE_NAME:-nvmf-failover-bench}"
echo "block_bench mask $BLOCK_BENCH_CPU_MASK image $BLOCK_BENCH_IMAGE_NAME"
"$script_root/scripts/run-dev-write-read-verify.sh" "$bench_conf"

check_no_new_cores
check_osd_logs

step "nvmf failover regression passed"
echo "image $image_name verified through NVMe-oF TCP after replica+leader kill"
echo "block_bench image $BLOCK_BENCH_IMAGE_NAME verified"
echo "logs: $log_dir"
