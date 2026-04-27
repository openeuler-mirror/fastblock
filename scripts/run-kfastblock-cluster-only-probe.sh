#!/bin/bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/kfastblock-test-common.sh"

repo_root="$(kfastblock_repo_root)"
config_file="$(kfastblock_local_config_file "$repo_root")"
hold_sec="${KFASTBLOCK_CLUSTER_PROBE_HOLD_SEC:-20}"
sample_interval_sec="${KFASTBLOCK_CLUSTER_PROBE_SAMPLE_INTERVAL_SEC:-2}"
reuse_cluster="${KFASTBLOCK_CLUSTER_PROBE_REUSE_CLUSTER:-0}"
collect_report="${KFASTBLOCK_CLUSTER_PROBE_COLLECT_REPORT:-1}"
run_dir="$(kfastblock_artifact_dir "$repo_root" cluster-only-probe)"
log_file="$run_dir/cluster-only-probe.log"
status_file="$run_dir/status-samples.txt"
probe_status=0

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

kfastblock_require_root
kfastblock_acquire_test_lock "$repo_root"
kfastblock_start_logging "$log_file"
echo "artifact_dir=$run_dir"
echo "hold_sec=$hold_sec sample_interval_sec=$sample_interval_sec reuse_cluster=$reuse_cluster collect_report=$collect_report"

KFASTBLOCK_TEST_REUSE_CLUSTER="$reuse_cluster" \
    kfastblock_prepare_or_reuse_dev_cluster "$repo_root" "$config_file"
kfastblock_capture_context "$config_file" "$run_dir"

deadline=$((SECONDS + hold_sec))
: > "$status_file"
while [ "$SECONDS" -lt "$deadline" ]; do
    {
        printf '=== %s ===\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')"
        "$repo_root/monitor/fastblock-client" \
            -conf="$config_file" \
            -op=status
        printf '\n'
    } | tee -a "$status_file"
    sleep "$sample_interval_sec"
done

echo "cluster-only probe ok: hold_sec=$hold_sec"
