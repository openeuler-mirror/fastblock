#!/bin/bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/kfastblock-test-common.sh"

repo_root="$(kfastblock_repo_root)"
config_file="$(kfastblock_local_config_file "$repo_root")"
pool_name="${KFASTBLOCK_STAGED_PROBE_POOL:-fb}"
image_name="${KFASTBLOCK_STAGED_PROBE_IMAGE:-kfb-stage-$(date +%s)}"
stage="${KFASTBLOCK_STAGED_PROBE_STAGE:-cluster}"
reuse_cluster="${KFASTBLOCK_STAGED_PROBE_REUSE_CLUSTER:-0}"
collect_report="${KFASTBLOCK_STAGED_PROBE_COLLECT_REPORT:-1}"
run_dir="$(kfastblock_artifact_dir "$repo_root" staged-probe)"
log_file="$run_dir/staged-probe.log"
payload_file="$run_dir/payload.bin"
readback_file="$run_dir/readback.bin"
monitor_addr=""
device_path=""
detach_needed=0
probe_status=0

stage_marker() {
    local phase="$1"
    local name="$2"

    printf '[%s] staged-probe %s %s\n' \
        "$(date '+%Y-%m-%d %H:%M:%S %Z')" "$phase" "$name"
}

cleanup() {
    kfastblock_release_test_lock
    if [ "$detach_needed" = "1" ]; then
        kfastblock_detach_volume "$repo_root" "$pool_name" "$image_name" >/dev/null 2>&1 || true
    fi
    rm -f "$payload_file" "$readback_file"
}

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

trap 'probe_status=$?; collect_report_on_exit; cleanup; exit $probe_status' EXIT

kfastblock_require_root
kfastblock_acquire_test_lock "$repo_root"
kfastblock_start_logging "$log_file"
echo "artifact_dir=$run_dir"
echo "stage=$stage pool_name=$pool_name image_name=$image_name reuse_cluster=$reuse_cluster collect_report=$collect_report"

stage_marker "begin" "cluster-prepare"
KFASTBLOCK_TEST_REUSE_CLUSTER="$reuse_cluster" \
    kfastblock_prepare_or_reuse_dev_cluster "$repo_root" "$config_file"
stage_marker "end" "cluster-prepare"
kfastblock_capture_context "$config_file" "$run_dir"
stage_marker "begin" "resolve-monitor"
monitor_addr="$(kfastblock_resolve_monitor_addr "$config_file")"
stage_marker "end" "resolve-monitor"

case "$stage" in
    cluster)
        stage_marker "begin" "cluster-status"
        "$repo_root/monitor/fastblock-client" -conf="$config_file" -op=status
        stage_marker "end" "cluster-status"
        ;;
    image|rawctl|module|attach|open|io)
        stage_marker "begin" "create-image"
        kfastblock_create_image "$repo_root" "$config_file" "$pool_name" "$image_name"
        stage_marker "end" "create-image"
        if [ "$stage" = "image" ]; then
            exit 0
        fi

        stage_marker "begin" "rawctl-checks"
        kfastblock_run_rawctl_checks "$repo_root" "$monitor_addr" "$pool_name" "$image_name"
        stage_marker "end" "rawctl-checks"
        if [ "$stage" = "rawctl" ]; then
            exit 0
        fi

        stage_marker "begin" "load-module"
        kfastblock_build_and_load_module "$repo_root"
        stage_marker "end" "load-module"
        if [ "$stage" = "module" ]; then
            exit 0
        fi

        stage_marker "begin" "attach-volume"
        kfastblock_attach_volume "$repo_root" "$monitor_addr" "$pool_name" "$image_name"
        detach_needed=1
        device_path="$(kfastblock_resolve_device)"
        stage_marker "end" "attach-volume"
        if [ "$stage" = "attach" ]; then
            exit 0
        fi

        if [ "$stage" = "open" ]; then
            stage_marker "begin" "open-device"
            exec {fd}<>"$device_path"
            exec {fd}>&-
            stage_marker "end" "open-device"
            exit 0
        fi

        stage_marker "begin" "io-verify"
        printf 'KFASTBLOCK_STAGE_%s' "$image_name" | dd \
            of="$payload_file" bs=4096 count=1 conv=sync status=none
        dd if="$payload_file" of="$device_path" \
            bs=4096 count=1 oflag=direct status=none
        dd if="$device_path" of="$readback_file" \
            bs=4096 count=1 iflag=direct status=none
        cmp -n 4096 "$payload_file" "$readback_file"
        stage_marker "end" "io-verify"
        ;;
    *)
        echo "unsupported stage: $stage" >&2
        exit 1
        ;;
esac

echo "staged probe ok: stage=$stage"
