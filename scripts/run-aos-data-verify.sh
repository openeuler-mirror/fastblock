#!/bin/bash
# Data-correctness path: unique per-offset payloads, replica dump-compare,
# restart recover, and single-OSD failover read.
#
# block_bench is a latency tool (constant 0x55). This script uses image_verify.
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
image_name="${AOS_VERIFY_IMAGE:-aos-verify-$(date +%s)}"
seed="${AOS_VERIFY_SEED:-$(date +%s)}"
expected_osds="${RAFT_REGRESSION_OSD_COUNT:-3}"
expected_pgs="${RAFT_REGRESSION_PG_COUNT:-1}"
cpu_mask="${BLOCK_BENCH_CPU_MASK:-0x8}"
boot_wait="${AOS_RESTART_BOOT_WAIT:-30}"
align="${AOS_DUMP_ALIGN:-512}"
do_restart="${AOS_VERIFY_RESTART:-1}"
do_failover="${AOS_VERIFY_FAILOVER:-1}"

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
    local want_up="${2:-$expected_osds}"
    local status
    echo "Checking cluster health: $label (osds up=$want_up)"
    status="$(cluster_status)"
    printf "%s\n" "$status"
    if ! grep -Eq "${expected_osds} osds: ${want_up} up" <<<"$status"; then
        echo "unexpected OSD health during $label" >&2
        exit 1
    fi
    if ! grep -Eq "[[:space:]]${expected_pgs}[[:space:]]+active" <<<"$status"; then
        echo "unexpected PG health during $label" >&2
        exit 1
    fi
}

aos_files() {
    local id
    for id in $(seq 1 "$expected_osds"); do
        echo "$data_dir/osd-$id/aos_file"
    done
}

dump_compare() {
    local label="$1"
    echo "dump-compare ($label) seed=$seed"
    "$verify_bin" -M dump-compare --align "$align" --seed "$seed" $(aos_files)
}

run_verify() {
    local mode="$1"
    echo "image_verify $mode image=$image_name seed=$seed"
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
        "$verify_bin" -m "$cpu_mask" -C "$bench_conf" -M "$mode" -S "$seed" -I "$image_name"
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

stop_osd() {
    local id="$1"
    local pid_file="$run_dir/osd-$id.pid"
    local pid
    [ -f "$pid_file" ] || return 0
    pid="$(cat "$pid_file")"
    if kill -0 "$pid" 2>/dev/null; then
        echo "stopping osd $id pid $pid"
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
}

stop_local_osds() {
    local id
    for id in $(seq 1 "$expected_osds"); do
        stop_osd "$id"
    done
}

start_osd() {
    local id="$1"
    local pid_file="$run_dir/osd-$id.pid"
    echo "starting osd $id"
    nohup "$osd_bin" -C "$conf" --id "$id" -N 0 \
        >> "$log_dir/osd${id}.log" 2>&1 &
    echo $! > "$pid_file"
}

start_local_osds() {
    local id
    for id in $(seq 1 "$expected_osds"); do
        start_osd "$id"
        sleep 3
    done
}

require_file "$conf"
require_file "$bench_conf"
require_exec "$client_bin"
require_exec "$osd_bin"
require_exec "$verify_bin"

for f in $(aos_files); do
    require_file "$f"
done

echo "AOS data verify image=$image_name seed=$seed"
check_health "before write"
run_verify write
dump_compare "after write"
run_verify verify

if [ "$do_restart" = "1" ]; then
    echo "Restarting OSDs (no mkfs)"
    stop_local_osds
    wait_osds_down
    dump_compare "osds stopped"
    start_local_osds
    echo "Waiting ${boot_wait}s for OSDs to recover"
    sleep "$boot_wait"
    check_health "after restart"
    run_verify verify
    dump_compare "after restart"
fi

if [ "$do_failover" = "1" ]; then
    echo "Failing over: stop osd 1 and re-read"
    stop_osd 1
    sleep 5
    check_health "osd1 down" 2
    run_verify verify
    start_osd 1
    echo "Waiting ${boot_wait}s for osd 1 to rejoin"
    sleep "$boot_wait"
    check_health "osd1 back"
fi

echo "AOS data verify passed for image $image_name seed=$seed"
