#!/bin/bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/kfastblock-test-common.sh"

repo_root="$(kfastblock_repo_root)"
config_file="$(kfastblock_local_config_file "$repo_root")"
pool_name="${KFASTBLOCK_OSD_LOOP_POOL:-fb}"
target_osd="${KFASTBLOCK_OSD_LOOP_TARGET_OSD:-1}"
restart_cycles="${KFASTBLOCK_OSD_LOOP_CYCLES:-5}"
down_sleep_sec="${KFASTBLOCK_OSD_LOOP_DOWN_SEC:-2}"
stabilize_sleep_sec="${KFASTBLOCK_OSD_LOOP_STABILIZE_SEC:-2}"
reuse_cluster="${KFASTBLOCK_OSD_LOOP_REUSE_CLUSTER:-0}"
collect_report="${KFASTBLOCK_OSD_LOOP_COLLECT_REPORT:-1}"
run_dir="$(kfastblock_artifact_dir "$repo_root" osd-restart-loop)"
log_file="$run_dir/osd-restart-loop.log"
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

trap 'probe_status=$?; collect_report_on_exit; kfastblock_release_test_lock; exit $probe_status' EXIT

kfastblock_require_root
kfastblock_acquire_test_lock "$repo_root"
kfastblock_start_logging "$log_file"
echo "artifact_dir=$run_dir"
echo "target_osd=$target_osd restart_cycles=$restart_cycles down_sleep_sec=$down_sleep_sec stabilize_sleep_sec=$stabilize_sleep_sec reuse_cluster=$reuse_cluster collect_report=$collect_report"

KFASTBLOCK_TEST_REUSE_CLUSTER="$reuse_cluster" \
    kfastblock_prepare_or_reuse_dev_cluster "$repo_root" "$config_file"
kfastblock_capture_context "$config_file" "$run_dir"

: > "$status_file"
for cycle in $(seq 1 "$restart_cycles"); do
    printf '=== cycle=%s begin %s ===\n' "$cycle" \
        "$(date '+%Y-%m-%d %H:%M:%S %Z')" | tee -a "$status_file"
    "$repo_root/monitor/fastblock-client" \
        -conf="$config_file" \
        -op=status | tee -a "$status_file"
    echo | tee -a "$status_file"

    echo "restart osd-$target_osd cycle=$cycle"
    kfastblock_restart_local_osd "$repo_root" "$target_osd" "$down_sleep_sec"
    sleep "$stabilize_sleep_sec"
    kfastblock_wait_cluster_active "$repo_root" "$config_file"

    printf '=== cycle=%s end %s ===\n' "$cycle" \
        "$(date '+%Y-%m-%d %H:%M:%S %Z')" | tee -a "$status_file"
    "$repo_root/monitor/fastblock-client" \
        -conf="$config_file" \
        -op=status | tee -a "$status_file"
    echo | tee -a "$status_file"
done

echo "osd restart loop probe ok: target_osd=$target_osd cycles=$restart_cycles"
