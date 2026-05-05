#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_root="${KFASTBLOCK_TEST_ARTIFACT_ROOT:-$repo_root/.artifacts/kfastblock-tests}"
run_dir="$artifact_root/endpoint-parallel-probe-$(date +%Y%m%d-%H%M%S)"
log_file="$run_dir/endpoint-parallel-probe.log"
parallel_limit="${KFASTBLOCK_ENDPOINT_PROBE_LIMIT:-1}"
duration_sec="${KFASTBLOCK_ENDPOINT_PROBE_DURATION_SEC:-10}"
io_workers="${KFASTBLOCK_ENDPOINT_PROBE_IO_WORKERS:-2}"
open_workers="${KFASTBLOCK_ENDPOINT_PROBE_OPEN_WORKERS:-1}"
attach_cycles="${KFASTBLOCK_ENDPOINT_PROBE_ATTACH_CYCLES:-0}"
dispatch_window="${KFASTBLOCK_ENDPOINT_PROBE_DISPATCH_WINDOW:-}"
reuse_cluster="${KFASTBLOCK_ENDPOINT_PROBE_REUSE_CLUSTER:-0}"
force_reload="${KFASTBLOCK_ENDPOINT_PROBE_FORCE_RELOAD_MODULE:-1}"
collect_report="${KFASTBLOCK_ENDPOINT_PROBE_COLLECT_REPORT:-1}"
probe_status=0

mkdir -p "$run_dir"
exec > >(tee -a "$log_file") 2>&1

collect_report_on_exit() {
    if [ "$collect_report" != "1" ]; then
        return 0
    fi
    if [ ! -x "$repo_root/scripts/collect-kfastblock-crash-report.sh" ]; then
        return 0
    fi

    KFASTBLOCK_TEST_ARTIFACT_ROOT="$run_dir" \
        bash "$repo_root/scripts/collect-kfastblock-crash-report.sh" || true
}

trap 'probe_status=$?; collect_report_on_exit; exit $probe_status' EXIT

echo "artifact_dir=$run_dir"
echo "parallel_limit=$parallel_limit duration_sec=$duration_sec io_workers=$io_workers open_workers=$open_workers attach_cycles=$attach_cycles"
echo "dispatch_window=${dispatch_window:-default}"
echo "reuse_cluster=$reuse_cluster force_reload=$force_reload collect_report=$collect_report"

env \
    KFASTBLOCK_TEST_ARTIFACT_ROOT="$run_dir" \
    KFASTBLOCK_TEST_REUSE_CLUSTER="$reuse_cluster" \
    KFASTBLOCK_MODULE_PARAMS="osd_endpoint_parallel_limit=$parallel_limit" \
    KFASTBLOCK_FORCE_RELOAD_MODULE="$force_reload" \
    KFASTBLOCK_CONCURRENCY_DURATION_SEC="$duration_sec" \
    KFASTBLOCK_CONCURRENCY_IO_WORKERS="$io_workers" \
    KFASTBLOCK_CONCURRENCY_OPEN_WORKERS="$open_workers" \
    KFASTBLOCK_CONCURRENCY_ATTACH_CYCLES="$attach_cycles" \
    KFASTBLOCK_CONCURRENCY_DISPATCH_WINDOW="$dispatch_window" \
    "$repo_root/scripts/run-kfastblock-concurrency.sh"

echo "probe ok: osd_endpoint_parallel_limit=$parallel_limit"
