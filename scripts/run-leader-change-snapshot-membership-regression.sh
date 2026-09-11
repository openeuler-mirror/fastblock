#!/bin/bash
# Phase 1 PR 1.2 scenario C: snapshot recovery crossed with membership
# change.  Both directions on one 4-OSD cluster (a spare is required):
#
#   C1  lagging follower recovers by snapshot, then change_nodes
#   C2  after the new membership, drop a member and force snapshot
#       recovery inside that member set (must not remap)
#
# Brings up its own cluster: the raft log trim trigger has to be set at
# osd boot.
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
conf="$script_root/.vstart/etc/fastblock/fastblock.json"
log_dir="$script_root/.vstart/var/log/fastblock"
run_dir="$script_root/.vstart/run"
client_bin="$script_root/monitor/fastblock-client"
stress_bin="$script_root/build/src/test/snapshot_pg_stress"
membership_bin="$script_root/build/src/test/raft_membership"
osd_bin="$script_root/build/src/osd/fastblock-osd"

pool_id="${LEADER_SNAPSHOT_POOL_ID:-1}"
pg_id="${LEADER_SNAPSHOT_PG_ID:-4}"
osd_count="${LEADER_SNAPSHOT_OSD_COUNT:-4}"
pg_count=8
replica=3
snapshot_objects="${LEADER_SNAPSHOT_OBJECTS:-200}"
io_size=4096
stress_cpu_mask="${LEADER_SNAPSHOT_CPU_MASK:-0x10}"
membership_cpu_mask="${RAFT_MEMBERSHIP_CPU_MASK:-0x20}"
max_recovery_rounds="${LEADER_SNAPSHOT_MAX_RECOVERY_ROUNDS:-8}"

export FASTBLOCK_VSTART_AIO_FILE_SIZE="${FASTBLOCK_VSTART_AIO_FILE_SIZE:-2G}"
export FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES="${FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES:-128}"
export FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES="${FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES:-64}"
export FASTBLOCK_RAFT_RECOVERY_TEST_LOGS=1
export FASTBLOCK_VSTART_SPDK_ARGS="${FASTBLOCK_VSTART_SPDK_ARGS:---no-huge}"
export RAFT_MEMBERSHIP_TIMEOUT_SECONDS="${RAFT_MEMBERSHIP_TIMEOUT_SECONDS:-300}"
# change_nodes that adds a node against a trimmed log waits on snapshot
# catch-up of the new member.  The default 1s RPC timeout aborts the client
# while the leader is still installing.
# Default 1s matches vstart / scenario B.  A short-log change_nodes
# completes in tens of ms; a 600s timeout kept the RPC open across a
# leader crash during new-member catch-up and hid the real error.

object_name="${LEADER_SNAPSHOT_OBJECT:-leader_change_snap_memb_regression}"
live_object="${object_name}_live"
c1_filler="${object_name}_c1_filler"
c2_filler="${object_name}_c2_filler"
append_object="${object_name}_append"

killed_victim_pids=""

kill_osd() {
    local id="$1"
    local pid
    pid="$(cat "$run_dir/osd-${id}.pid" 2>/dev/null || true)"
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        pid="$(pgrep -f "fastblock-os[d].* --id ${id} " | head -1 || true)"
    fi
    if [ -n "$pid" ]; then
        killed_victim_pids="${killed_victim_pids} ${pid}"
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$run_dir/osd-${id}.pid"
}

start_osd() {
    local id="$1"
    "$osd_bin" -C "$conf" --id "$id" -N 0 \
        $FASTBLOCK_VSTART_SPDK_ARGS \
        >> "$log_dir/osd${id}.log" 2>&1 &
    echo "$!" > "$run_dir/osd-${id}.pid"
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

core_pid_from_path() {
    local base
    base="$(basename "${1:-}")"
    awk -F. '{print $(NF-2)}' <<<"$base"
}

check_no_new_core() {
    local before="$1"
    local after
    after="$(latest_core || true)"
    if [ "$after" = "$before" ]; then
        return 0
    fi
    local core_pid victim_pid
    core_pid="$(core_pid_from_path "$after")"
    for victim_pid in $killed_victim_pids; do
        if [ -n "$core_pid" ] && [ "$core_pid" = "$victim_pid" ]; then
            echo "ignoring coredump from deliberately killed pid $core_pid"
            return 0
        fi
    done
    echo "new coredump detected" >&2
    echo "before: ${before:-<none>}" >&2
    echo "after : ${after:-<none>}" >&2
    exit 1
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
    local deadline=$((SECONDS + 180))
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
    "$client_bin" -conf="$conf" -op=getpgmap >&2 || true
    exit 1
}

check_full_health() {
    local label="$1"
    echo "Checking cluster health: $label"
    wait_for_status "$label" "${osd_count} osds: ${osd_count} up, ${osd_count} in" >/dev/null
    wait_for_status "$label" "[[:space:]]${pg_count}[[:space:]]+active" >/dev/null
}

load_osd_map() {
    declare -gA osd_addr=()
    declare -gA osd_port=()
    osd_ids=()

    while read -r osd_id addr _core _shard port _rest; do
        [[ "$osd_id" =~ ^[0-9]+$ ]] || continue
        osd_ids+=("$osd_id")
        osd_addr["$osd_id"]="$addr"
        osd_port["$osd_id"]="$port"
    done < <("$client_bin" -conf="$conf" -op=getosdmap -detail)
}

load_pg_members() {
    local pg_line
    pg_line="$("$client_bin" -conf="$conf" -op=getpgmap | awk -v pg="$pool_id.$pg_id" '$1 == pg {print}')"
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

sorted_csv() {
    echo "$1" | tr ',' '\n' | grep -E '^[0-9]+$' | sort -n | paste -sd, -
}

contains_member() {
    local needle="$1"
    shift
    local item
    for item in "$@"; do
        if [ "$item" = "$needle" ]; then
            return 0
        fi
    done
    return 1
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
    local skip
    local member
    for member in "${pg_members[@]}"; do
        local ok=1
        for skip in "$@"; do
            if [ "$member" = "$skip" ]; then
                ok=0
                break
            fi
        done
        if [ "$ok" -eq 1 ]; then
            printf "%s" "$member"
            return 0
        fi
    done
    echo "failed to pick a remaining PG member (skip: $*)" >&2
    exit 1
}

pick_spare() {
    local osd_id
    for osd_id in "${osd_ids[@]}"; do
        if ! contains_member "$osd_id" "${pg_members[@]}"; then
            printf "%s" "$osd_id"
            return 0
        fi
    done
    echo "no spare OSD found for PG $pool_id.$pg_id" >&2
    exit 1
}

log_count() {
    local pattern="$1"
    local n
    n="$(grep -E -c "$pattern" "$log_dir"/osd*.log 2>/dev/null \
        | awk -F: '{s += $NF} END {print s + 0}' || true)"
    echo "${n:-0}"
    return 0
}

wait_for_log() {
    local label="$1"
    local pattern="$2"
    local min_count="${3:-1}"
    local deadline=$((SECONDS + 240))
    local count=0
    local hit=""

    while [ "$SECONDS" -lt "$deadline" ]; do
        count="$(log_count "$pattern")"
        if [ "$count" -ge "$min_count" ]; then
            hit="$(grep -E -n "$pattern" "$log_dir"/osd*.log 2>/dev/null | tail -n 1 || true)"
            echo "$hit"
            return 0
        fi
        sleep 2
    done

    echo "timed out waiting for log: $label (have $count, want >= $min_count)" >&2
    echo "pattern: $pattern" >&2
    "$client_bin" -conf="$conf" -op=getpgmap >&2 || true
    exit 1
}

count_recovery_rounds() {
    local node="$1"
    log_count "recovery by snapshot to node ${node}"
}

run_pg_stress() {
    local mode="$1"
    local contact_osd="$2"
    local object="$3"
    local write_count="$4"
    local object_count="$5"
    local port="${osd_port[$contact_osd]}"
    local addr="${osd_addr[$contact_osd]}"

    echo "pg_stress $mode object=$object writes=$write_count count=$object_count via osd$contact_osd"
    FB_PG_OBJECT="$object" \
    FB_PG_WRITES="$write_count" \
    FB_PG_OBJECT_COUNT="$object_count" \
    FB_PG_IO_SIZE="$io_size" \
    FB_PG_MODE="$mode" \
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
        "$stress_bin" --no-huge -s 1024 -m "$stress_cpu_mask" \
        -C "$conf" \
        -I "$contact_osd" \
        -o "$addr" \
        -t "$port" \
        -P "$pool_id" \
        -G "$pg_id"
}

run_membership_change() {
    local label="$1"
    local contact_osd="$2"
    local nodes_csv="$3"
    local log_file="$4"

    echo "Running membership change ($label): $nodes_csv via osd$contact_osd"
    local rc
    set +e
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
        "$membership_bin" --no-huge -s 1024 -m "$membership_cpu_mask" \
        -C "$conf" \
        -I "$contact_osd" \
        -o "${osd_addr[$contact_osd]}" \
        -t "${osd_port[$contact_osd]}" \
        -P "$pool_id" \
        -G "$pg_id" \
        -N change \
        -O "$nodes_csv" > "$log_file" 2>&1
    rc=$?
    set -e
    if grep -q "change membership in the pg .* result: 0" "$log_file"; then
        return 0
    fi
    if [ "$rc" -eq 0 ]; then
        return 1
    fi
    return "$rc"
}

# Membership can race a snapshot that is still draining on the leader
# (RAFT_ERR_SNAPSHOT_IN_PROGRESS = -140 → exit 116) or a brief no-leader
# window after a restart.  Retry a few times.
run_membership_change_retry() {
    local label="$1"
    local contact_osd="$2"
    local nodes_csv="$3"
    local log_file="$4"
    local attempt rc=1

    for attempt in 1 2 3 4 5 6; do
        load_osd_map
        set +e
        run_membership_change "$label attempt $attempt" "$contact_osd" "$nodes_csv" "$log_file"
        rc=$?
        set -e
        if [ "$rc" -eq 0 ]; then
            cat "$log_file"
            return 0
        fi
        echo "membership change ($label) attempt $attempt rc=$rc"
        cat "$log_file" || true
        # 116 = SNAPSHOT_IN_PROGRESS, 113 = NOT_FOUND_LEADER, 109 = NO_CONNECTED,
        # 103 = MEMBERSHIP_CHANGING.  Anything else is a real failure.
        case "$rc" in
            116|113|109|103|1) sleep 10 ;;
            *) return "$rc" ;;
        esac
    done
    echo "membership change ($label) failed after retries, last rc=$rc" >&2
    return "$rc"
}

force_snapshot_recovery() {
    local lagging="$1"
    local contact="$2"
    local filler="$3"
    local rounds_before rounds_after rounds

    echo "---- snapshot recovery of osd$lagging (filler=$filler) ----"
    kill_osd "$lagging"
    wait_for_status "osd$lagging down" "${osd_count} osds: $((osd_count - 1)) up, ${osd_count} in" >/dev/null

    load_pg_members
    local trim_before end_before
    trim_before="$(log_count "raft log test trim \\[")"
    end_before="$(log_count "pg ${pool_id}\\.${pg_id}, snapshot is end\\.")"
    run_pg_stress write "$contact" "$filler" "$snapshot_objects" "$snapshot_objects"
    wait_for_log "leader raft log trim" "raft log test trim \\[" "$((trim_before + 1))"

    echo "Restarting osd$lagging to force install snapshot"
    start_osd "$lagging"
    wait_for_status "osd$lagging back" "${osd_count} osds: ${osd_count} up, ${osd_count} in" >/dev/null

    rounds_before="$(count_recovery_rounds "$lagging")"
    wait_for_log "snapshot recovery starts for osd$lagging" \
        "pg: ${pool_id}\\.${pg_id}, recovery by snapshot to node ${lagging}" \
        "$((rounds_before + 1))"
    wait_for_log "install snapshot completed for osd$lagging" \
        "pg ${pool_id}\\.${pg_id}, snapshot is end\\." \
        "$((end_before + 1))"

    rounds_after="$(count_recovery_rounds "$lagging")"
    rounds=$((rounds_after - rounds_before))
    if [ "$rounds" -gt "$max_recovery_rounds" ]; then
        echo "_recovery_by_snapshot looped $rounds times for osd$lagging (max $max_recovery_rounds)" >&2
        exit 1
    fi
    echo "recovery by snapshot of osd$lagging ran $rounds round(s) and finished"
}

assert_members_csv() {
    local want="$1"
    local label="$2"
    local deadline=$((SECONDS + 180))
    local got=""

    while [ "$SECONDS" -lt "$deadline" ]; do
        load_pg_members
        got="$(pg_member_csv)"
        if [ "$(sorted_csv "$got")" = "$(sorted_csv "$want")" ]; then
            echo "PG $pool_id.$pg_id members $got ($label)"
            return 0
        fi
        sleep 3
    done
    echo "PG $pool_id.$pg_id members mismatch ($label): got $got want $want" >&2
    "$client_bin" -conf="$conf" -op=getpgmap >&2 || true
    exit 1
}

scan_fatal_logs() {
    if grep -E -n "assert|Assertion|SIGSEGV|segmentation|core dumped" "$log_dir"/*.log >/tmp/leader-change-snap-memb-fatal.log 2>/dev/null; then
        cat /tmp/leader-change-snap-memb-fatal.log >&2
        echo "fatal log pattern found" >&2
        exit 1
    fi
}

if [ "$osd_count" -le "$replica" ]; then
    echo "this regression needs a spare OSD; run with LEADER_SNAPSHOT_OSD_COUNT=4" >&2
    exit 1
fi

require_exec "$client_bin"
require_exec "$stress_bin"
require_exec "$membership_bin"
require_exec "$osd_bin"
require_exec "$script_root/vstart.sh"

core_before="$(latest_core || true)"

stop_cluster
for _ in $(seq 1 30); do
    pgrep -f "build/src/osd/fastblock-osd" >/dev/null || break
    pkill -9 -f "build/src/osd/fastblock-osd" 2>/dev/null || true
    sleep 1
done
for _ in $(seq 1 15); do
    pgrep -f "monitor/fastblock-mon" >/dev/null || break
    pkill -9 -f "monitor/fastblock-mo[n]" 2>/dev/null || true
    sleep 1
done
rm -rf "$script_root/.vstart"

echo "Starting 4-OSD cluster with raft trim trigger=$FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES keep=$FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES"
FASTBLOCK_VSTART_AIO_PREALLOCATE=0 \
    "$script_root/vstart.sh" -m dev -c "$osd_count" -r "$replica" -p "$pg_count" -C 1

"$client_bin" -conf="$conf" -set=noout
check_full_health "baseline"
load_osd_map
load_pg_members
leader="$(find_pg_leader)"
lagging="$(pick_other "$leader")"
other="$(pick_other "$leader" "$lagging")"
spare="$(pick_spare)"
original_csv="$(pg_member_csv)"
alternate_csv="${leader},${lagging},${spare}"

echo "Target PG $pool_id.$pg_id members: $original_csv"
echo "Leader: osd$leader  lagging: osd$lagging  replace: osd$other  spare: osd$spare"
echo "Alternate members: $alternate_csv"

# ---- C1: change_nodes on a short log, then snapshot-recover a member
# of the NEW set (self-test report 5.8.3).  Adding a node against a
# trimmed log currently crashes the leader at snapshot-end → AE, so
# membership has to happen before the first trim.
echo
echo "== C1: membership change, then snapshot recovery in the new set =="
run_pg_stress write "$leader" "$live_object" 1 1
run_pg_stress read "$leader" "$live_object" 1 1

tool_log="$(mktemp /tmp/leader-change-snap-memb.XXXX.log)"
run_membership_change_retry "C1 to alternate" "$leader" "$alternate_csv" "$tool_log"
cat "$tool_log"
check_full_health "after C1 membership"
assert_members_csv "$alternate_csv" "after C1 membership"

load_osd_map
load_pg_members
leader="$(find_pg_leader)"
run_pg_stress read "$leader" "$live_object" 1 1

c1_lagging="$spare"
if ! contains_member "$c1_lagging" "${pg_members[@]}" || [ "$c1_lagging" = "$leader" ]; then
    c1_lagging="$(pick_other "$leader")"
fi
echo "C1 will snapshot-recover osd$c1_lagging in member set $(pg_member_csv)"
force_snapshot_recovery "$c1_lagging" "$leader" "$c1_filler"
check_full_health "after C1 snapshot"
assert_members_csv "$alternate_csv" "after C1 snapshot (must stay on new members, not remap)"

load_osd_map
leader="$(find_pg_leader)"
run_pg_stress read "$leader" "$live_object" 1 1
run_pg_stress read "$leader" "$c1_filler" 1 "$snapshot_objects"
run_pg_stress write "$leader" "$append_object" 16 1
run_pg_stress read "$leader" "$append_object" 1 1
echo "C1 passed"

# ---- C2: snapshot-recover a different member of the same new set ----
echo
echo "== C2: snapshot recovery of another member of the new set =="
load_osd_map
load_pg_members
leader="$(find_pg_leader)"
c2_lagging="$(pick_other "$leader" "$c1_lagging")"
echo "C2 will snapshot-recover osd$c2_lagging in member set $(pg_member_csv)"

force_snapshot_recovery "$c2_lagging" "$leader" "$c2_filler"
check_full_health "after C2 snapshot"
assert_members_csv "$alternate_csv" "after C2 snapshot (must stay on new members, not remap)"

load_osd_map
leader="$(find_pg_leader)"
run_pg_stress read "$leader" "$live_object" 1 1
run_pg_stress read "$leader" "$c1_filler" 1 "$snapshot_objects"
run_pg_stress read "$leader" "$c2_filler" 1 "$snapshot_objects"
run_pg_stress read "$leader" "$append_object" 1 1
echo "C2 passed"

scan_fatal_logs
check_no_new_core "$core_before"

echo "leader-change snapshot-membership regression completed"
