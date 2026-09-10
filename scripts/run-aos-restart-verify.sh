#!/bin/bash
# Write, restart every local OSD (no mkfs), then read-verify the same image.
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
state_root="${FASTBLOCK_VSTART_STATE_ROOT:-$script_root/.vstart}"
conf="${FASTBLOCK_CONF:-$state_root/etc/fastblock/fastblock.json}"
bench_conf="${FASTBLOCK_BENCH_CONF:-$state_root/etc/fastblock/block_bench.dev.json}"
data_dir="${FASTBLOCK_DATA_DIR:-$state_root/var/lib/fastblock}"
log_dir="${FASTBLOCK_LOG_DIR:-$state_root/var/log/fastblock}"
run_dir="$state_root/run"
client_bin="${FASTBLOCK_CLIENT_BIN:-$script_root/monitor/fastblock-client}"
osd_bin="${FASTBLOCK_OSD_BIN:-$script_root/build/src/osd/fastblock-osd}"
verify_bin="${FASTBLOCK_IMAGE_VERIFY_BIN:-$script_root/build/src/tools/image_verify/image_verify}"
image_name="${AOS_RESTART_IMAGE:-aos-restart-$(date +%s)}"
seed="${AOS_VERIFY_SEED:-$(date +%s)}"
expected_osds="${RAFT_REGRESSION_OSD_COUNT:-3}"
replica="${RAFT_REGRESSION_REPLICA:-3}"
expected_pgs="${RAFT_REGRESSION_PG_COUNT:-1}"
cpu_mask="${BLOCK_BENCH_CPU_MASK:-0x8}"
boot_wait="${AOS_RESTART_BOOT_WAIT:-30}"
align="${AOS_DUMP_ALIGN:-512}"

require_file() {
    if [ ! -f "$1" ]; then
        echo "required file not found: $1" >&2
        exit 1
    fi
}

require_exec() {
    if [ ! -x "$1" ]; then
        echo "required executable not found: $1" >&2
        exit 1
    fi
}

cluster_status() {
    "$client_bin" -conf="$conf" -op=status
}

check_health() {
    local label="$1"
    local status
    echo "Checking cluster health: $label"
    status="$(cluster_status)"
    printf "%s\n" "$status"
    if ! grep -Eq "${expected_osds} osds: ${expected_osds} up, ${expected_osds} in" <<<"$status"; then
        echo "unexpected OSD health during $label" >&2
        exit 1
    fi
    if ! grep -Eq "[[:space:]]${expected_pgs}[[:space:]]+active" <<<"$status"; then
        echo "unexpected PG health during $label" >&2
        exit 1
    fi
}

wait_osds_down() {
    local i=0
    local status
    echo "Waiting for monitor to mark OSDs down"
    while [ "$i" -lt 40 ]; do
        status="$(cluster_status || true)"
        if grep -Eq "osd: ${expected_osds} osds: 0 up" <<<"$status"; then
            printf "%s\n" "$status"
            return 0
        fi
        sleep 2
        i=$((i + 1))
    done
    echo "monitor did not mark OSDs down in time:" >&2
    cluster_status || true
    exit 1
}

stop_local_osds() {
    local pid_file pid
    for pid_file in "$run_dir"/osd-*.pid; do
        [ -f "$pid_file" ] || continue
        pid="$(cat "$pid_file")"
        if kill -0 "$pid" 2>/dev/null; then
            echo "stopping osd pid $pid ($pid_file)"
            kill -TERM "$pid" || true
            local i=0
            while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 40 ]; do
                sleep 1
                i=$((i + 1))
            done
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" || true
            fi
        fi
        rm -f "$pid_file"
    done
}

start_local_osds() {
    local id pid_file
    for id in $(seq 1 "$expected_osds"); do
        pid_file="$run_dir/osd-$id.pid"
        echo "starting osd $id"
        nohup "$osd_bin" -C "$conf" --id "$id" -N 0 \
            >> "$log_dir/osd${id}.log" 2>&1 &
        echo $! > "$pid_file"
        sleep 3
    done
}

require_file "$conf"
require_file "$bench_conf"
require_exec "$client_bin"
require_exec "$osd_bin"
require_exec "$verify_bin"

check_health "before write"

echo "Writing unique payloads image=$image_name seed=$seed"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    "$verify_bin" -m "$cpu_mask" -C "$bench_conf" -M write -S "$seed" -I "$image_name"

echo "Restarting OSDs (no mkfs)"
for id in $(seq 1 "$expected_osds"); do
    wc -l < "$log_dir/osd${id}.log" > "/tmp/osd${id}.loglines"
done
stop_local_osds
wait_osds_down
start_local_osds
echo "Waiting ${boot_wait}s for OSDs to recover"
sleep "$boot_wait"
check_health "after restart"

echo "Read-verify unique payloads image=$image_name seed=$seed after restart"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    "$verify_bin" -m "$cpu_mask" -C "$bench_conf" -M verify -S "$seed" -I "$image_name"

echo "dump-compare after restart seed=$seed"
dump_files=()
for id in $(seq 1 "$expected_osds"); do
    dump_files+=("$data_dir/osd-$id/aos_file")
done
"$verify_bin" -M dump-compare --align "$align" --seed "$seed" \
    --min-replicas "$replica" "${dump_files[@]}"

echo "Checking recover kv / shutdown after restart"
fail_cfg=0
for id in $(seq 1 "$expected_osds"); do
    log="$log_dir/osd${id}.log"
    prev_lines=0
    if [ -f "/tmp/osd${id}.loglines" ]; then
        prev_lines="$(cat "/tmp/osd${id}.loglines")"
    fi
    tail -n +"$((prev_lines + 1))" "$log" > "/tmp/osd${id}.cycle"
    last_start="$(grep -n 'Starting SPDK' "/tmp/osd${id}.cycle" | tail -1 | cut -d: -f1)"
    echo "----- osd $id this cycle -----"
    grep -E 'aos recover kv table|aos recover scan done|seed self|wrong_thread|booted|Stopping AppendOnlyStore' \
        "/tmp/osd${id}.cycle" || true
    if grep -q 'wrong_thread' "/tmp/osd${id}.cycle"; then
        echo "osd $id: wrong_thread during this restart cycle" >&2
        fail_cfg=1
    fi
    if [ -n "$last_start" ]; then
        tail -n +"$last_start" "/tmp/osd${id}.cycle" > "/tmp/osd${id}.lastboot"
        if grep -q 'seed self cfg' "/tmp/osd${id}.lastboot"; then
            echo "osd $id: still seeding self cfg (node_cfg missing after recover)" >&2
            fail_cfg=1
        fi
        if ! grep -q 'node_cfg' "/tmp/osd${id}.lastboot"; then
            echo "osd $id: recover log has no node_cfg key" >&2
            fail_cfg=1
        fi
    else
        echo "osd $id: no SPDK start after restart" >&2
        fail_cfg=1
    fi
    rm -f "/tmp/osd${id}.cycle" "/tmp/osd${id}.lastboot" "/tmp/osd${id}.loglines"
done
if [ "$fail_cfg" -ne 0 ]; then
    echo "restart IO passed but raft kv recover check failed" >&2
    exit 6
fi

echo "AOS restart verify passed for image $image_name"
