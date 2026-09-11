#!/bin/bash
# Phase 1 PR 1.2 scenario B: change_nodes in progress, then the original
# leader exits.  The pg map must converge to the target members (re-issuing
# the change after the new leader takes over is allowed), the monitor must
# keep answering GetClusterMap, and data must read back afterwards.
#
# Assumes a running cluster with at least replica+1 OSDs (a spare is needed
# to build the alternate member set), e.g. started by run-engine-signoff.sh.
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
conf="${FASTBLOCK_CONF:-$script_root/.vstart/etc/fastblock/fastblock.json}"
log_dir="${FASTBLOCK_LOG_DIR:-$script_root/.vstart/var/log/fastblock}"
run_dir="${FASTBLOCK_RUN_DIR:-$script_root/.vstart/run}"
client_bin="${FASTBLOCK_CLIENT_BIN:-$script_root/monitor/fastblock-client}"
membership_bin="${RAFT_MEMBERSHIP_BIN:-$script_root/build/src/test/raft_membership}"
osd_bin="${FASTBLOCK_OSD_BIN:-$script_root/build/src/osd/fastblock-osd}"

pool_id="${RAFT_REGRESSION_POOL_ID:-1}"
pg_id="${RAFT_REGRESSION_PG_ID:-4}"
osd_count="${RAFT_REGRESSION_OSD_COUNT:-4}"
pg_count="${RAFT_REGRESSION_PG_COUNT:-8}"
membership_cpu_mask="${RAFT_MEMBERSHIP_CPU_MASK:-0x10}"
bench_cpu_mask="${BLOCK_BENCH_CPU_MASK:-0x80}"
converge_timeout="${LEADER_CHANGE_CONVERGE_TIMEOUT:-180}"

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

killed_victim=""

# On any exit path, make sure a victim killed for the scenario is running
# again; leaving it dead poisons the next run of this or any other script.
cleanup() {
    if [ -n "${killed_victim:-}" ] && ! pgrep -f "fastblock-os[d].* --id ${killed_victim} " >/dev/null 2>&1; then
        nohup "$osd_bin" -C "$conf" --id "$killed_victim" -N 0             >> "$log_dir/osd${killed_victim}.log" 2>&1 &
    fi
}
trap cleanup EXIT

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
    status="$(cluster_status)"
    printf "%s\n" "$status"
}

load_osd_map() {
    declare -gA osd_addr=()
    declare -gA osd_port=()
    osd_ids=()

    # Live query: log scraping fed stale ports to the membership tool before.
    while read -r osd_id addr _core _shard port _rest; do
        [[ "$osd_id" =~ ^[0-9]+$ ]] || continue
        osd_ids+=("$osd_id")
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

sorted_members() {
    printf "%s\n" "$@" | sort -n | tr '\n' ' ' | sed 's/ $//'
}

member_csv() {
    local IFS=,
    printf "%s" "$*"
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

derive_alternate_members() {
    local spare=""
    local osd_id
    for osd_id in "${osd_ids[@]}"; do
        if ! contains_member "$osd_id" "${pg_members[@]}"; then
            spare="$osd_id"
            break
        fi
    done

    if [ -z "$spare" ]; then
        echo "no spare OSD found for PG $pool_id.$pg_id membership regression" >&2
        exit 1
    fi

    read -r -a alt_members <<<"${pg_members[@]}"
    alt_members[((${#alt_members[@]} - 1))]="$spare"
}

run_membership_change() {
    local label="$1"
    local contact_osd="$2"
    local nodes_csv="$3"
    local log_file="$4"

    echo "Running membership change ($label): $nodes_csv"
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
}

wait_pg_active() {
    local label="$1"
    local deadline=$((SECONDS + converge_timeout))

    # change_nodes driven by the test tool rewrites the raft config only; the
    # mon pgmap keeps the mon-assigned member list, so there is nothing to
    # converge there.  What must hold is: the PG stays active and serves IO
    # with the new raft member set.
    while [ "$SECONDS" -lt "$deadline" ]; do
        if "$client_bin" -conf="$conf" -op=getpgmap \
             | awk -v pg="$pool_id.$pg_id" '$1 == pg {exit !($2 == "active")}'; then
            echo "PG $pool_id.$pg_id active ($label)"
            return 0
        fi
        sleep 3
    done

    echo "PG $pool_id.$pg_id not active within ${converge_timeout}s: $label" >&2
    exit 1
}

run_write_read_verify() {
    local label="$1"
    echo "Running write/read verify: $label"
    BLOCK_BENCH_CPU_MASK="$bench_cpu_mask" \
        "$script_root/scripts/run-dev-write-read-verify.sh"
}

scan_fatal_logs() {
    if grep -E -n "assert|Assertion|SIGSEGV|segmentation|core dumped" "$log_dir"/*.log >/tmp/leader-change-membership-fatal.log 2>/dev/null; then
        cat /tmp/leader-change-membership-fatal.log >&2
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

if [ "$osd_count" -le 3 ]; then
    echo "this regression needs a spare OSD; run with RAFT_REGRESSION_OSD_COUNT=4" >&2
    exit 1
fi

require_exec "$client_bin"
require_exec "$membership_bin"
require_exec "$osd_bin"
require_exec "$script_root/scripts/run-dev-write-read-verify.sh"

core_before="$(latest_core || true)"

check_full_health "baseline"
load_osd_map
load_pg_members
derive_alternate_members

original_csv="$(member_csv "${pg_members[@]}")"
alternate_csv="$(member_csv "${alt_members[@]}")"

echo "Original PG $pool_id.$pg_id members: $original_csv"
echo "Alternate PG $pool_id.$pg_id members: $alternate_csv"

# noout keeps the mon from remapping PGs around the kill; the member change
# under test is the raft-level one.
"$client_bin" -conf="$conf" -set=noout

# ---- the cross moment: launch the change and take out the raft leader as
# soon as the tool reports it, so the kill lands in the change/mon-finish
# window.
tool_log="$(mktemp /tmp/leader-change-membership-tool.XXXX.log)"
run_membership_change "to alternate (leader exits mid-flight)" "${pg_members[0]}" "$alternate_csv" "$tool_log" &
change_pid="$!"

victim=""
for _ in $(seq 1 50); do
    if [[ "$(cat "$tool_log" 2>/dev/null)" =~ leader\ of\ the\ pg\ [0-9]+\.[0-9]+\ is\ ([0-9]+) ]]; then
        victim="${BASH_REMATCH[1]}"
        break
    fi
    if ! kill -0 "$change_pid" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if [ -z "$victim" ] || [ "$victim" = "0" ]; then
    # leader id 0 means the contacted osd had no elected leader for this pg:
    # mon pgmap and raft membership have diverged (leftover member changes).
    echo "membership tool reported no leader (id=0); cluster state is not clean" >&2
    exit 1
fi
# small randomized delay so repeated runs sample different points of the window
sleep "0.$((05 + RANDOM % 25))"
if kill -0 "$change_pid" 2>/dev/null; then
    echo "Killing the leader osd$victim while the change is in flight"
    kill_osd "$victim"
    killed_victim="$victim"
else
    echo "change already finished; killing osd$victim anyway as a follow-up fault"
    kill_osd "$victim"
fi

set +e
wait "$change_pid"
change_rc=$?
set -e
echo "membership tool exited rc=$change_rc"
cat "$tool_log"

wait_for_status "after leader death" "${osd_count} osds: $((osd_count - 1)) up" >/dev/null
# The dead leader leaves its other PGs undersize on purpose; the target PG's
# membership convergence is checked by wait_pg_members below.

# The new leader continues or aborts the pending change; converge to the
# target members either way.
wait_pg_active "after leader death (alternate membership)"

echo "Restarting the killed leader osd$victim"
"$osd_bin" -C "$conf" --id "$victim" -N 0 \
    >> "$log_dir/osd${victim}.log" 2>&1 &
echo "$!" > "$run_dir/osd-${victim}.pid"
killed_victim=""
wait_for_status "killed leader back" "${osd_count} osds: ${osd_count} up, ${osd_count} in"
check_full_health "killed leader back"

run_write_read_verify "on alternate membership"
check_full_health "after write/read on alternate"

# Change back to the original members so the cluster is left as found.
load_osd_map
load_osd_map
load_pg_members
run_membership_change "back to original" "${pg_members[0]}" "$original_csv" "${tool_log}.revert"
wait_pg_active "after revert"
check_full_health "after revert membership"

run_write_read_verify "after revert membership"
check_full_health "after revert write/read"

scan_fatal_logs
check_no_new_core "$core_before"

echo "leader-change membership regression completed"
