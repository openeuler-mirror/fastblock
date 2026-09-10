#!/bin/bash
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

conf="${FASTBLOCK_CONF:-$script_root/.vstart/etc/fastblock/fastblock.json}"
bench_conf="${FASTBLOCK_BENCH_CONF:-$script_root/.vstart/etc/fastblock/block_bench.dev.json}"
monitor_log="${FASTBLOCK_MONITOR_LOG:-$script_root/.vstart/var/log/fastblock/monitor.log}"
log_dir="${FASTBLOCK_LOG_DIR:-$script_root/.vstart/var/log/fastblock}"
client_bin="${FASTBLOCK_CLIENT_BIN:-$script_root/monitor/fastblock-client}"
membership_bin="${RAFT_MEMBERSHIP_BIN:-$script_root/build/src/test/raft_membership}"
pool_id="${RAFT_REGRESSION_POOL_ID:-1}"
pg_id="${RAFT_REGRESSION_PG_ID:-4}"
expected_osds="${RAFT_REGRESSION_OSD_COUNT:-4}"
expected_pgs="${RAFT_REGRESSION_PG_COUNT:-8}"
membership_cpu_mask="${RAFT_MEMBERSHIP_CPU_MASK:-0x10}"
bench_cpu_mask="${BLOCK_BENCH_CPU_MASK:-0x80}"

require_file() {
    local path="$1"
    if [ ! -f "$path" ]; then
        echo "required file not found: $path" >&2
        exit 1
    fi
}

require_exec() {
    local path="$1"
    if [ ! -x "$path" ]; then
        echo "required executable not found: $path" >&2
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

latest_core() {
    find /var/lib/systemd/coredump -maxdepth 1 -name 'core.*' -printf '%T@ %p\n' 2>/dev/null \
        | sort -n \
        | tail -n 1
}

load_osd_map() {
    local parsed
    # Query the live osd map instead of scraping monitor.log: the log line only
    # appears when some client pulls the map, so after an osd restart (ports are
    # reassigned at every boot) the last logged line holds stale ports and the
    # membership tool gets refused connections it never recovers from.  The
    # -detail view prints one row per shard; take shard 0.
    parsed="$("$client_bin" -conf="$conf" -op=getosdmap -detail 2>/dev/null \
        | awk 'NR > 1 && $4 == 0 {print $1, $2, $5}')"

    if [ -z "$parsed" ]; then
        echo "failed to derive OSD map via $client_bin -op=getosdmap -detail" >&2
        exit 1
    fi

    osd_ids=()
    declare -gA osd_addr=()
    declare -gA osd_port=()
    while read -r osd_id addr port; do
        [ -n "$osd_id" ] || continue
        osd_ids+=("$osd_id")
        osd_addr["$osd_id"]="$addr"
        osd_port["$osd_id"]="$port"
    done <<<"$parsed"
}

load_pg_members() {
    local members
    members="$(RAFT_REGRESSION_PG_ID="$pg_id" perl -0777 -ne '
        my $pg = $ENV{RAFT_REGRESSION_PG_ID};
        my @lines = /^(.*pool\.go:721.*)$/mg;
        for (my $i = $#lines; $i >= 0; --$i) {
            my $line = $lines[$i];
            if ($line =~ /(?:map\[|\s)\Q$pg\E:\{\d+\s+\d+\s+\d+\s+\[([^\]]+)\]/) {
                print "$1\n";
                last;
            }
        }
    ' "$monitor_log")"

    if [ -z "$members" ]; then
        echo "failed to derive PG $pool_id.$pg_id members from $monitor_log" >&2
        exit 1
    fi

    read -r -a pg_members <<<"$members"
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

member_csv() {
    local first=1
    local out=""
    local osd_id
    for osd_id in "$@"; do
        if [ -z "${osd_addr[$osd_id]:-}" ] || [ -z "${osd_port[$osd_id]:-}" ]; then
            echo "missing address/port for osd $osd_id" >&2
            exit 1
        fi
        if [ "$first" = 1 ]; then
            first=0
        else
            out+=","
        fi
        out+="${osd_id}:${osd_addr[$osd_id]}:${osd_port[$osd_id]}"
    done
    printf "%s" "$out"
}

derive_alternate_members() {
    if [ "${#pg_members[@]}" -lt 2 ]; then
        echo "PG $pool_id.$pg_id has too few members for membership regression" >&2
        exit 1
    fi

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

    alt_members=("${pg_members[@]}")
    alt_members[((${#alt_members[@]} - 1))]="$spare"
}

run_membership_change() {
    local label="$1"
    local contact_osd="$2"
    local nodes_csv="$3"

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
        -O "$nodes_csv"
}

run_write_read_verify() {
    local label="$1"
    echo "Running write/read verify: $label"
    BLOCK_BENCH_CPU_MASK="$bench_cpu_mask" \
        "$script_root/scripts/run-dev-write-read-verify.sh" "$bench_conf"
}

scan_fatal_logs() {
    if rg -n "assert|Assertion|SIGSEGV|segmentation|core dumped" "$log_dir"/*.log >/tmp/raft-dev-regression-fatal.log 2>/dev/null; then
        cat /tmp/raft-dev-regression-fatal.log >&2
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

require_file "$conf"
require_file "$bench_conf"
require_file "$monitor_log"
require_exec "$client_bin"
require_exec "$membership_bin"

core_before="$(latest_core || true)"

check_health "baseline"
load_osd_map
load_pg_members
derive_alternate_members

original_csv="$(member_csv "${pg_members[@]}")"
alternate_csv="$(member_csv "${alt_members[@]}")"

echo "Original PG $pool_id.$pg_id members: $original_csv"
echo "Alternate PG $pool_id.$pg_id members: $alternate_csv"

run_membership_change "to alternate" "${pg_members[0]}" "$alternate_csv"
check_health "after alternate membership"
run_write_read_verify "after alternate membership"
check_health "after alternate write/read"

run_membership_change "back to original" "${alt_members[0]}" "$original_csv"
check_health "after revert membership"
run_write_read_verify "after revert membership"
check_health "after revert write/read"

scan_fatal_logs
check_no_new_core "$core_before"

echo "raft dev regression completed"
