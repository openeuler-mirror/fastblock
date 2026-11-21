#!/bin/bash
# Phase 1 PR 1.2 scenario A: install snapshot in progress, then the leader
# exits and comes back.  The new leader must continue (or restart) the
# install, the lagging follower must catch up, every payload must read back
# and _recovery_by_snapshot must not loop forever.
#
# Brings up its own cluster: the raft log trim trigger has to be set at osd
# boot, so a cluster started without it cannot be forced into snapshot
# recovery here.
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
conf="$script_root/.vstart/etc/fastblock/fastblock.json"
log_dir="$script_root/.vstart/var/log/fastblock"
run_dir="$script_root/.vstart/run"
client_bin="$script_root/monitor/fastblock-client"
stress_bin="$script_root/build/src/test/snapshot_pg_stress"

pool_id="${LEADER_SNAPSHOT_POOL_ID:-1}"
pg_id="${LEADER_SNAPSHOT_PG_ID:-0}"
osd_count="${LEADER_SNAPSHOT_OSD_COUNT:-3}"
pg_count=8
replica=3
snapshot_objects="${LEADER_SNAPSHOT_OBJECTS:-400}"
concurrent_writes=16
append_writes=16
io_size=4096
cpu_mask="${LEADER_SNAPSHOT_CPU_MASK:-0x8}"
max_recovery_rounds="${LEADER_SNAPSHOT_MAX_RECOVERY_ROUNDS:-6}"
leader_restart_delay="${LEADER_SNAPSHOT_RESTART_DELAY:-10}"

# AOS mkfs writes the whole sparse backing file, so the vstart default of
# 100G fills a small root partition and the format fails with ENOSPC.
export FASTBLOCK_VSTART_AIO_FILE_SIZE="${FASTBLOCK_VSTART_AIO_FILE_SIZE:-2G}"
export FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES="${FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES:-128}"
export FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES="${FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES:-64}"
export FASTBLOCK_RAFT_RECOVERY_TEST_LOGS=1
export FASTBLOCK_VSTART_SPDK_ARGS="${FASTBLOCK_VSTART_SPDK_ARGS:---no-huge}"

object_name="${LEADER_SNAPSHOT_OBJECT:-leader_change_snapshot_regression}"
live_object="${object_name}_live"
snapshot_filler="${object_name}_filler"
concurrent_object="${object_name}_concurrent"
append_object="${object_name}_append"

kill_osd() {
    local id="$1"
    local pid
    pid="$(cat "$run_dir/osd-${id}.pid" 2>/dev/null || true)"
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        # pid files go stale whenever an osd was restarted outside vstart
        pid="$(pgrep -f "fastblock-os[d].* --id ${id} " | head -1 || true)"
    fi
    if [ -n "$pid" ]; then
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$run_dir/osd-${id}.pid"
}

require_exec() {
    if [ ! -x "$1" ]; then
        echo "required executable not found: $1" >&2
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
    local deadline=$((SECONDS + 120))
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
}

load_osd_map() {
    declare -gA osd_addr=()
    declare -gA osd_port=()

    # Live query: log scraping fed stale ports to the membership tool before.
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
}

find_pg_leader() {
    local deadline=$((SECONDS + 90))
    local match=""

    while [ "$SECONDS" -lt "$deadline" ]; do
        match="$(grep -E -n "becoming leader of pg ${pool_id}\\.${pg_id}" "$log_dir"/osd*.log 2>/dev/null | tail -n 1 || true)"
        if [[ "$match" =~ osd([0-9]+)\.log ]]; then
            printf "%s" "${BASH_REMATCH[1]}"
            return 0
        fi
        sleep 1
    done

    echo "failed to derive leader for PG $pool_id.$pg_id" >&2
    exit 1
}

pick_other() {
    local leader="$1"
    local member
    for member in "${pg_members[@]}"; do
        if [ "$member" != "$leader" ]; then
            printf "%s" "$member"
            return 0
        fi
    done
    echo "failed to pick a second member for PG $pool_id.$pg_id" >&2
    exit 1
}

wait_for_log() {
    local label="$1"
    local pattern="$2"
    local deadline=$((SECONDS + 240))
    local hit=""

    while [ "$SECONDS" -lt "$deadline" ]; do
        hit="$(grep -E -n "$pattern" "$log_dir"/osd*.log 2>/dev/null | tail -n 1 || true)"
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

wait_for_osd_log_after_line() {
    local osd_id="$1"
    local start_line="$2"
    local label="$3"
    local pattern="$4"
    local deadline=$((SECONDS + 120))
    local hit=""
    local log_file="$log_dir/osd${osd_id}.log"

    while [ "$SECONDS" -lt "$deadline" ]; do
        hit="$(tail -n +"$((start_line + 1))" "$log_file" 2>/dev/null | grep -E -n "$pattern" | tail -n 1 || true)"
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
    if grep -E -n "assert|Assertion|SIGSEGV|segmentation|core dumped" "$log_dir"/*.log >/tmp/leader-change-snapshot-fatal.log 2>/dev/null; then
        cat /tmp/leader-change-snapshot-fatal.log >&2
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
require_exec "$script_root/build/src/osd/fastblock-osd"

core_before="$(latest_core || true)"

stop_cluster
# mkfs claims a DPDK core; an osd that is still dying from the previous run
# makes the new mkfs fail silently (no core_size in the data dir) and that
# osd then spins forever without booting.  Wait the old ones out.
for _ in $(seq 1 30); do
    pgrep -f "build/src/osd/fastblock-osd" >/dev/null || break
    pkill -9 -f "build/src/osd/fastblock-osd" 2>/dev/null || true
    sleep 1
done
rm -rf "$script_root/.vstart"

echo "Starting dev cluster with raft trim trigger=$FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES keep=$FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES"
FASTBLOCK_VSTART_AIO_PREALLOCATE=0 \
    "$script_root/vstart.sh" -m dev -c "$osd_count" -r "$replica" -p "$pg_count" -C 1

"$client_bin" -conf="$conf" -set=noout
check_full_health "baseline"
load_osd_map
load_pg_members
leader="$(find_pg_leader)"
lagging="$(pick_other "$leader")"
original_members="$(awk '{print $4}' <<<"$( "$client_bin" -conf="$conf" -op=getpgmap | awk -v pg="$pool_id.$pg_id" '$1 == pg {print; exit}')" | tr -d '[]')"

echo "Target PG $pool_id.$pg_id members: $original_members"
echo "Leader: osd$leader"
echo "Lagging follower: osd$lagging"

kill_osd "$lagging"
wait_for_status "lagging follower down" "${osd_count} osds: $((osd_count - 1)) up, ${osd_count} in"

load_pg_members
if [ "${pg_members[*]}" != "$(tr ',' ' ' <<<"$original_members")" ]; then
    echo "PG $pool_id.$pg_id remapped despite noout" >&2
    exit 1
fi

run_pg_stress write "$leader" "$live_object" 1 1
run_pg_stress read "$leader" "$live_object" 1 1

echo "Writing $snapshot_objects snapshot filler objects while osd$lagging is down/in"
run_pg_stress write "$leader" "$snapshot_filler" "$snapshot_objects" "$snapshot_objects"

wait_for_log "leader raft log trim" "raft log test trim \\["

echo "Restarting osd$lagging to force install snapshot"
"$script_root/build/src/osd/fastblock-osd" -C "$conf" --id "$lagging" -N 0 \
    $FASTBLOCK_VSTART_SPDK_ARGS \
    >> "$log_dir/osd${lagging}.log" 2>&1 &
echo "$!" > "$run_dir/osd-${lagging}.pid"

wait_for_log "snapshot recovery starts" "pg: ${pool_id}\\.${pg_id}, recovery by snapshot to node ${lagging}"
wait_for_log "non-final install snapshot sent" "send installsnapshot_request to node ${lagging} pg ${pool_id}\\.${pg_id} .* done 0"

# ---- the cross moment: the leader exits mid-install and comes back
echo "Killing the leader osd$leader while install snapshot is in progress"
kill_osd "$leader"
rm -f "$run_dir/osd-${leader}.pid"
sleep "$leader_restart_delay"

echo "Restarting the old leader osd$leader"
"$script_root/build/src/osd/fastblock-osd" -C "$conf" --id "$leader" -N 0 \
    $FASTBLOCK_VSTART_SPDK_ARGS \
    >> "$log_dir/osd${leader}.log" 2>&1 &
echo "$!" > "$run_dir/osd-${leader}.pid"

wait_for_status "after leader restart" "${osd_count} osds: ${osd_count} up, ${osd_count} in"
check_full_health "after leader restart"

echo "Waiting for the install snapshot to finish under the new leader"
wait_for_log "install snapshot completed" "pg ${pool_id}\\.${pg_id}, snapshot is end\\."

recovery_rounds="$(grep -E -c "recovery by snapshot to node ${lagging}" "$log_dir"/osd*.log 2>/dev/null | awk -F: '{s += $NF} END {print s + 0}')"
if [ "$recovery_rounds" -gt "$max_recovery_rounds" ]; then
    echo "_recovery_by_snapshot looped $recovery_rounds times (max $max_recovery_rounds)" >&2
    exit 1
fi
echo "recovery by snapshot ran $recovery_rounds round(s) and finished"

check_full_health "after install snapshot"
load_osd_map
new_leader="$(find_pg_leader)"
echo "Leader after recovery: osd$new_leader"

append_start_line="$(wc -l < "$log_dir/osd${lagging}.log")"
echo "Writing $append_writes objects after recovery to verify append entries"
run_pg_stress write "$new_leader" "$append_object" "$append_writes" 1
wait_for_osd_log_after_line "$lagging" "$append_start_line" "post-recovery append entries" \
    "raft recovery test appendentries received from node [0-9]+ pg: ${pool_id}\\.${pg_id} .* entries [1-9]"

echo "Concurrent write/read while the recovered cluster serves IO"
run_pg_stress write "$new_leader" "$concurrent_object" "$concurrent_writes" 1
run_pg_stress read "$new_leader" "$concurrent_object" 1 1

run_pg_stress read "$new_leader" "$live_object" 1 1
run_pg_stress read "$new_leader" "$snapshot_filler" 1 "$snapshot_objects"
run_pg_stress read "$new_leader" "$append_object" 1 1
check_full_health "after read verify"

scan_fatal_logs
check_no_new_core "$core_before"

echo "leader-change snapshot regression completed"
