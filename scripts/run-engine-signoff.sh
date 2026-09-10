#!/bin/bash
# Phase 1 engine signoff: run the existing regressions in a fixed order and
# stop at the first failure.  Every stage carries data verification; "pg is
# active" alone is never treated as a pass.
#
# Usage:
#   ./scripts/run-engine-signoff.sh
#   SIGNOFF_START_CLUSTER=0 ./scripts/run-engine-signoff.sh   # reuse a running cluster
set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
conf="${FASTBLOCK_CONF:-$script_root/.vstart/etc/fastblock/fastblock.json}"
bench_conf="${FASTBLOCK_BENCH_CONF:-$script_root/.vstart/etc/fastblock/block_bench.dev.json}"
client_bin="${FASTBLOCK_CLIENT_BIN:-$script_root/monitor/fastblock-client}"

osd_count="${SIGNOFF_OSD_COUNT:-3}"
replica="${SIGNOFF_REPLICA:-3}"
pg_count="${SIGNOFF_PG_COUNT:-8}"
ready_timeout="${SIGNOFF_READY_TIMEOUT:-300}"
start_cluster="${SIGNOFF_START_CLUSTER:-1}"

# AOS mkfs writes the whole sparse backing file, so the vstart default of 100G
# fills a small root partition and fails with ENOSPC (aio rc -28).
export FASTBLOCK_VSTART_AIO_FILE_SIZE="${FASTBLOCK_VSTART_AIO_FILE_SIZE:-4G}"
# OSDs claim cores 0..osd_count-1, so the bench cannot keep its 0x4 default.
export BLOCK_BENCH_CPU_MASK="${BLOCK_BENCH_CPU_MASK:-$(printf '0x%x' "$((1 << osd_count))")}"
export RAFT_REGRESSION_OSD_COUNT="$osd_count"
export RAFT_REGRESSION_PG_COUNT="$pg_count"
export RAFT_SNAPSHOT_OSD_COUNT="$osd_count"
export RAFT_SNAPSHOT_REPLICA="$replica"

msg_transport="$(jq -r '.msg_transport // "?"' "$bench_conf" 2>/dev/null || echo '?')"

require_exec() {
    if [ ! -x "$1" ]; then
        echo "required executable not found: $1" >&2
        exit 1
    fi
}

step() {
    echo
    echo "== $* =="
}

cluster_ready() {
    local st
    st="$("$client_bin" -conf="$conf" -op=status 2>/dev/null || true)"
    grep -qE "[[:space:]]${osd_count} up" <<<"$st" || return 1
    grep -qE "^[[:space:]]+${pg_count}[[:space:]]+active" <<<"$st" || return 1
    return 0
}

wait_ready() {
    local deadline=$((SECONDS + ready_timeout))
    while (( SECONDS < deadline )); do
        if cluster_ready; then
            echo "cluster ready: ${osd_count} osd up, ${pg_count} pg active"
            return 0
        fi
        sleep 5
    done
    echo "cluster not ready within ${ready_timeout}s" >&2
    "$client_bin" -conf="$conf" -op=status >&2 || true
    exit 1
}

echo "engine signoff: ${osd_count} osd / replica ${replica} / ${pg_count} pg / msg_transport=${msg_transport}"
echo "aio backing file ${FASTBLOCK_VSTART_AIO_FILE_SIZE}, bench cpu mask ${BLOCK_BENCH_CPU_MASK}"

require_exec "$client_bin"
for s in run-dev-write-read-verify.sh run-aos-restart-verify.sh \
         run-raft-dev-regression.sh run-raft-install-snapshot-regression.sh; do
    require_exec "$script_root/scripts/$s"
done

if [ "$start_cluster" = "1" ]; then
    step "bring up dev cluster"
    "$script_root/vstart.sh" -m dev -c "$osd_count" -r "$replica" -p "$pg_count"
fi

# A freshly created pool can report active before every PG has settled; a write
# issued inside that window can come back with a stale view. Gate on health.
step "wait for cluster health"
wait_ready

step "1/4 write-read-verify"
"$script_root/scripts/run-dev-write-read-verify.sh"

step "2/4 aos restart verify"
"$script_root/scripts/run-aos-restart-verify.sh"
wait_ready

step "3/4 raft dev regression"
if [ "$osd_count" -le "$replica" ]; then
    echo "skipped: membership regression needs a spare OSD outside the PG;"
    echo "         re-run with SIGNOFF_OSD_COUNT=$((replica + 1)) on this box"
else
    "$script_root/scripts/run-raft-dev-regression.sh"
fi

step "4/4 raft install-snapshot regression (brings up its own cluster)"
"$script_root/scripts/run-raft-install-snapshot-regression.sh"

step "engine signoff passed"
