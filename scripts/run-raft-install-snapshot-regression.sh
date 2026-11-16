#!/bin/bash
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

conf="$script_root/.vstart/etc/fastblock/fastblock.json"
log_dir="$script_root/.vstart/var/log/fastblock"
run_dir="$script_root/.vstart/run"
client_bin="$script_root/monitor/fastblock-client"
stress_bin="$script_root/build/src/test/snapshot_pg_stress"

pool_id="${RAFT_SNAPSHOT_POOL_ID:-1}"
pg_id="${RAFT_SNAPSHOT_PG_ID:-4}"
osd_count="${RAFT_SNAPSHOT_OSD_COUNT:-4}"
pg_count="${RAFT_SNAPSHOT_PG_COUNT:-8}"
replica="${RAFT_SNAPSHOT_REPLICA:-3}"
writes="${RAFT_SNAPSHOT_WRITES:-512}"
snapshot_objects="${RAFT_SNAPSHOT_OBJECT_COUNT:-256}"
concurrent_writes="${RAFT_SNAPSHOT_CONCURRENT_WRITES:-16}"
append_writes="${RAFT_SNAPSHOT_APPEND_WRITES:-16}"
io_size="${RAFT_SNAPSHOT_IO_SIZE:-4096}"
cpu_mask="${RAFT_SNAPSHOT_CPU_MASK:-0x10}"
trim_trigger="${FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES:-128}"
trim_keep="${FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES:-64}"
object_name="${RAFT_SNAPSHOT_OBJECT:-raft_install_snapshot_regression}"
live_object="${object_name}_live"

export FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES="$trim_trigger"
export FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES="$trim_keep"
export FASTBLOCK_RAFT_RECOVERY_TEST_LOGS=1

require_exec() {
    local path="$1"
    if [ ! -x "$path" ]; then
        echo "required executable not found: $path" >&2
        exit 1
    fi
}

latest_core() {
    find /var/lib/systemd/coredump -maxdepth 1 -name 'core.*' -printf '%T@ %p\n' 2>/dev/null \
        | sort -n \
        | tail -n 1
}

stop_cluster() {
    if [ -d "$run_dir" ]; then
        local pid_file
        for pid_file in "$run_dir"/osd-*.pid "$run_dir"/monitor.pid; do
            [ -f "$pid_file" ] || continue
            kill "$(cat "$pid_file")" 2>/dev/null || true
        done
    fi
}

cleanup() {
    stop_cluster
}
trap cleanup EXIT

cluster_status() {
    "$client_bin" -conf="$conf" -op=status
}

wait_for_status() {
    local label="$1"
    local pattern="$2"
    local deadline=$((SECONDS + 90))
    local status=""

    while [ "$SECONDS" -lt "$deadline" ]; do
        status="$(cluster_status || true)"
        if grep -Eq "$pattern" <<<"$status"; then
            printf "%s\n" "$status"
            return 0
        fi
        sleep 2
    done

    echo "timed out waiting for status: $label" >&2
    printf "%s\n" "$status" >&2
    exit 1
}

check_full_health() {
    local label="$1"
    local status
    echo "Checking cluster health: $label"
    wait_for_status "$label" "${osd_count} osds: ${osd_count} up, ${osd_count} in" >/dev/null
    wait_for_status "$label" "[[:space:]]${pg_count}[[:space:]]+active" >/dev/null
    status="$(cluster_status)"
    printf "%s\n" "$status"

    if ! grep -Eq "${osd_count} osds: ${osd_count} up, ${osd_count} in" <<<"$status"; then
        echo "unexpected OSD health during $label" >&2
        exit 1
    fi
    if ! grep -Eq "[[:space:]]${pg_count}[[:space:]]+active" <<<"$status"; then
        echo "unexpected PG health during $label" >&2
        exit 1
    fi
}

load_osd_map() {
    declare -gA osd_addr=()
    declare -gA osd_port=()

    while read -r osd_id addr _core _shard port _rest; do
        [[ "$osd_id" =~ ^[0-9]+$ ]] || continue
        osd_addr["$osd_id"]="$addr"
        osd_port["$osd_id"]="$port"
    done < <("$client_bin" -conf="$conf" -op=getosdmap -detail)
}

load_pg_members() {
    local pg_line
    pg_line="$("$client_bin" -conf="$conf" -op=getpgmap | awk -v pg="$pool_id.$pg_id" '$1 == pg {print; exit}')"
    if [ -z "$pg_line" ]; then
        echo "failed to find PG $pool_id.$pg_id in pg map" >&2
        exit 1
    fi

    local members
    members="$(awk '{print $4}' <<<"$pg_line" | tr -d '[]' | tr ',' ' ')"
    read -r -a pg_members <<<"$members"
    if [ "${#pg_members[@]}" -lt 2 ]; then
        echo "PG $pool_id.$pg_id has too few members: $pg_line" >&2
        exit 1
    fi
}

pg_member_csv() {
    local IFS=,
    printf "%s" "${pg_members[*]}"
}

find_pg_leader() {
    local deadline=$((SECONDS + 60))
    local match=""

    while [ "$SECONDS" -lt "$deadline" ]; do
        match="$(rg -n "becoming leader of pg ${pool_id}\\.${pg_id}" "$log_dir"/osd*.log 2>/dev/null | tail -n 1 || true)"
        if [[ "$match" =~ osd([0-9]+)\.log ]]; then
            printf "%s" "${BASH_REMATCH[1]}"
            return 0
        fi
        sleep 1
    done

    echo "failed to derive leader for PG $pool_id.$pg_id" >&2
    exit 1
}

pick_follower() {
    local leader="$1"
    local member
    for member in "${pg_members[@]}"; do
        if [ "$member" != "$leader" ]; then
            printf "%s" "$member"
            return 0
        fi
    done
    echo "failed to pick follower for PG $pool_id.$pg_id" >&2
    exit 1
}

wait_for_log() {
    local label="$1"
    local pattern="$2"
    local deadline=$((SECONDS + 120))
    local hit=""

    while [ "$SECONDS" -lt "$deadline" ]; do
        hit="$(rg -n "$pattern" "$log_dir"/osd*.log 2>/dev/null | tail -n 1 || true)"
        if [ -n "$hit" ]; then
            echo "$hit"
            return 0
        fi
        sleep 2
    done

    echo "timed out waiting for log: $label" >&2
    echo "pattern: $pattern" >&2
    exit 1
}

wait_for_osd_log() {
    local osd_id="$1"
    local label="$2"
    local pattern="$3"
    local deadline=$((SECONDS + 120))
    local hit=""
    local log_file="$log_dir/osd${osd_id}.log"

    while [ "$SECONDS" -lt "$deadline" ]; do
        hit="$(rg -n "$pattern" "$log_file" 2>/dev/null | tail -n 1 || true)"
        if [ -n "$hit" ]; then
            echo "$hit"
            return 0
        fi
        sleep 2
    done

    echo "timed out waiting for osd${osd_id} log: $label" >&2
    echo "pattern: $pattern" >&2
    exit 1
}

wait_for_osd_log_after_line() {
    local osd_id="$1"
    local start_line="$2"
    local label="$3"
    local pattern="$4"
    local deadline=$((SECONDS + 120))
    local hit=""
    local log_file="$log_dir/osd${osd_id}.log"

    while [ "$SECONDS" -lt "$deadline" ]; do
        hit="$(tail -n +"$((start_line + 1))" "$log_file" 2>/dev/null | rg -n "$pattern" | tail -n 1 || true)"
        if [ -n "$hit" ]; then
            echo "$hit"
            return 0
        fi
        sleep 2
    done

    echo "timed out waiting for osd${osd_id} log after line ${start_line}: $label" >&2
    echo "pattern: $pattern" >&2
    exit 1
}

log_timestamp() {
    local file="$1"
    local pattern="$2"
    local hit
    hit="$(rg -n "$pattern" "$file" 2>/dev/null | tail -n 1 || true)"
    if [[ ! "$hit" =~ \[([^]]+)\] ]]; then
        echo "failed to find timestamp for pattern: $pattern in $file" >&2
        exit 1
    fi
    printf "%s" "${BASH_REMATCH[1]}"
}

run_pg_stress() {
    local mode="$1"
    local contact_osd="$2"
    local object="$3"
    local write_count="$4"
    local object_count="$5"
    local port="${osd_port[$contact_osd]}"
    local addr="${osd_addr[$contact_osd]}"

    FB_PG_OBJECT="$object" \
    FB_PG_WRITES="$write_count" \
    FB_PG_OBJECT_COUNT="$object_count" \
    FB_PG_IO_SIZE="$io_size" \
    FB_PG_MODE="$mode" \
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
        "$stress_bin" --no-huge -s 1024 -m "$cpu_mask" \
        -C "$conf" \
        -I "$contact_osd" \
        -o "$addr" \
        -t "$port" \
        -P "$pool_id" \
        -G "$pg_id"
}

scan_fatal_logs() {
    if rg -n "assert|Assertion|SIGSEGV|segmentation|core dumped" "$log_dir"/*.log >/tmp/raft-install-snapshot-fatal.log 2>/dev/null; then
        cat /tmp/raft-install-snapshot-fatal.log >&2
        echo "fatal log pattern found" >&2
        exit 1
    fi
}

check_no_new_core() {
    local before="$1"
    local after
    after="$(latest_core || true)"
    if [ "$after" != "$before" ]; then
        echo "new coredump detected" >&2
        echo "before: ${before:-<none>}" >&2
        echo "after : ${after:-<none>}" >&2
        exit 1
    fi
}

require_exec "$client_bin"
require_exec "$stress_bin"
require_exec "$script_root/vstart.sh"

core_before="$(latest_core || true)"

stop_cluster
rm -rf "$script_root/.vstart"

echo "Starting dev cluster with raft trim trigger=$trim_trigger keep=$trim_keep"
FASTBLOCK_VSTART_AIO_PREALLOCATE=0 \
FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES="$trim_trigger" \
FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES="$trim_keep" \
    "$script_root/vstart.sh" -m dev -c "$osd_count" -r "$replica" -p "$pg_count" -C 1

"$client_bin" -conf="$conf" -set=noout
check_full_health "baseline"

load_osd_map
load_pg_members
leader="$(find_pg_leader)"
follower="$(pick_follower "$leader")"
original_members="$(pg_member_csv)"

echo "Target PG $pool_id.$pg_id members: $original_members"
echo "Leader: osd$leader"
echo "Lagging follower: osd$follower"

kill "$(cat "$run_dir/osd-${follower}.pid")"
wait_for_status "follower down/in" "${osd_count} osds: $((osd_count - 1)) up, ${osd_count} in"

load_pg_members
if [ "$(pg_member_csv)" != "$original_members" ]; then
    echo "PG $pool_id.$pg_id remapped despite noout" >&2
    exit 1
fi

run_pg_stress write "$leader" "$live_object" 1 1
run_pg_stress read "$leader" "$live_object" 1 1

echo "Writing $snapshot_objects snapshot filler objects while osd$follower is down/in"
run_pg_stress write "$leader" "${object_name}_snapshot" "$snapshot_objects" "$snapshot_objects"

wait_for_osd_log "$leader" "leader raft log trim" "raft log test trim \\["

echo "Restarting osd$follower"
"$script_root/build/src/osd/fastblock-osd" -C "$conf" --id "$follower" -N 0 \
    >> "$log_dir/osd${follower}.log" 2>&1 &
echo "$!" > "$run_dir/osd-${follower}.pid"

wait_for_log "snapshot recovery starts" "pg: ${pool_id}\\.${pg_id}, recovery by snapshot to node ${follower}"
wait_for_log "snapshot check sent" "send snapshot_check_request to node ${follower} pg ${pool_id}\\.${pg_id}"
wait_for_log "non-final install snapshot sent" "send installsnapshot_request to node ${follower} pg ${pool_id}\\.${pg_id} .* done 0"

concurrent_log="$script_root/.vstart/raft-concurrent-io.log"
echo "Writing and reading while install snapshot is in progress"
(
    run_pg_stress write "$leader" "${object_name}_concurrent" "$concurrent_writes" 1
    run_pg_stress read "$leader" "${object_name}_concurrent" 1 1
) > "$concurrent_log" 2>&1 &
concurrent_pid="$!"

wait_for_log "install snapshot sent" "send installsnapshot_request to node ${follower} pg ${pool_id}\\.${pg_id}"
wait_for_log "install snapshot received" "recvd installsnapshot_request from node [0-9]+ pg: ${pool_id}\\.${pg_id}"
wait_for_log "install snapshot response" "recvd installsnapshot_response in pg: ${pool_id}\\.${pg_id} from node ${follower} .* success: 1"
wait_for_log "install snapshot final chunk sent" "send installsnapshot_request to node ${follower} pg ${pool_id}\\.${pg_id} .* done 1"
wait_for_log "install snapshot completed" "pg ${pool_id}\\.${pg_id}, snapshot is end\\."

if ! wait "$concurrent_pid"; then
    cat "$concurrent_log" >&2
    echo "concurrent IO failed while install snapshot was in progress" >&2
    exit 1
fi

concurrent_done_ts="$(log_timestamp "$concurrent_log" "snapshot_pg_stress read verify success, object=${object_name}_concurrent")"
snapshot_done_ts="$(log_timestamp "$log_dir/osd${leader}.log" "pg ${pool_id}\\.${pg_id}, snapshot is end\\.")"
if [[ "$concurrent_done_ts" > "$snapshot_done_ts" || "$concurrent_done_ts" == "$snapshot_done_ts" ]]; then
    echo "concurrent read/write did not finish before install snapshot completed" >&2
    echo "concurrent read/write done: $concurrent_done_ts" >&2
    echo "snapshot done             : $snapshot_done_ts" >&2
    exit 1
fi
echo "Concurrent read/write completed during install snapshot at $concurrent_done_ts; snapshot completed at $snapshot_done_ts"

check_full_health "after install snapshot"
load_osd_map
append_start_line="$(wc -l < "$log_dir/osd${follower}.log")"
echo "Writing $append_writes objects after install snapshot to verify append entries"
run_pg_stress write "$leader" "${object_name}_append" "$append_writes" 1
wait_for_osd_log_after_line "$follower" "$append_start_line" "post-snapshot append entries" \
    "raft recovery test appendentries received from node ${leader} pg: ${pool_id}\\.${pg_id} .* entries [1-9]"

run_pg_stress read "$leader" "$live_object" 1 1
run_pg_stress read "$leader" "${object_name}_concurrent" 1 1
run_pg_stress read "$leader" "${object_name}_append" 1 1
check_full_health "after read verify"

scan_fatal_logs
check_no_new_core "$core_before"

echo "raft install snapshot regression completed"
