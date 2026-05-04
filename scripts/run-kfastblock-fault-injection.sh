#!/bin/bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/kfastblock-test-common.sh"

repo_root="$(kfastblock_repo_root)"
config_file="$(kfastblock_local_config_file "$repo_root")"
pool_name="${KFASTBLOCK_FAULT_POOL:-fb}"
image_name="${KFASTBLOCK_FAULT_IMAGE:-kfb-fault-$(date +%s)}"
target_osd="${KFASTBLOCK_FAULT_TARGET_OSD:-1}"
down_sleep_sec="${KFASTBLOCK_FAULT_DOWN_SEC:-3}"
io_duration_sec="${KFASTBLOCK_FAULT_IO_DURATION_SEC:-18}"
run_dir="$(kfastblock_artifact_dir "$repo_root" fault-injection)"
log_file="$run_dir/fault.log"
payload_file="$run_dir/payload.bin"
readback_file="$run_dir/readback.bin"
monitor_addr=""
device_path=""
detach_needed=0
stop_requested=0
worker_pid=""

cleanup() {
    stop_requested=1
    if [ -n "$worker_pid" ]; then
        kill "$worker_pid" >/dev/null 2>&1 || true
        wait "$worker_pid" >/dev/null 2>&1 || true
    fi
    if [ "$detach_needed" = "1" ]; then
        kfastblock_detach_volume "$repo_root" "$pool_name" "$image_name" >/dev/null 2>&1 || true
    fi
    kfastblock_release_test_lock
    rm -f "$payload_file" "$readback_file"
}

on_error() {
    kfastblock_capture_failure_snapshot "$repo_root" "$run_dir" "$pool_name" "$image_name"
}

trap on_error ERR
trap cleanup EXIT

object_stride_blocks=$((4194304 / 4096))

write_verify_block() {
    local block="$1"
    local token="$2"

    printf 'KFB_FAULT_%s_%s' "$token" "$image_name" | dd \
        of="$payload_file" \
        bs=4096 \
        count=1 \
        conv=sync \
        status=none

    dd if="$payload_file" \
       of="$device_path" \
       bs=4096 \
       count=1 \
       seek="$block" \
       oflag=direct \
       conv=notrunc \
       status=none

    dd if="$device_path" \
       of="$readback_file" \
       bs=4096 \
       count=1 \
       skip="$block" \
       iflag=direct \
       status=none

    cmp -n 4096 "$payload_file" "$readback_file"
}

wait_for_recovery_io() {
    local base_block="$1"
    local token="$2"
    local deadline=$((SECONDS + 30))
    local attempt=0
    local block

    while [ "$SECONDS" -lt "$deadline" ]; do
        block=$((base_block + ((attempt % 16) * object_stride_blocks)))
        if write_verify_block "$block" "${token}-${attempt}" >/dev/null 2>&1; then
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 1
    done

    echo "recovery io did not succeed for block window starting at $base_block" >&2
    return 1
}

mark_osd_stopped() {
    "$repo_root/monitor/fastblock-client" \
        -conf="$config_file" \
        -op=fakestoposd \
        -osdid="$target_osd" >/dev/null
}

fault_io_worker() {
    local deadline="$1"
    local iter=0
    local ok=0
    local transient=0
    local block

    while [ "$SECONDS" -lt "$deadline" ] && [ "$stop_requested" = "0" ]; do
        block=$(((iter % 16) * object_stride_blocks))
        if write_verify_block "$block" "iter-${iter}" >/dev/null 2>&1; then
            ok=$((ok + 1))
        else
            transient=$((transient + 1))
            sleep 0.2
        fi
        iter=$((iter + 1))
    done

    printf 'ok=%d transient_failures=%d iterations=%d\n' \
        "$ok" "$transient" "$iter" > "$run_dir/fault-io.summary"
}

kfastblock_require_root
kfastblock_acquire_test_lock "$repo_root"
kfastblock_start_logging "$log_file"
echo "artifact_dir=$run_dir"
echo "target_osd=$target_osd down_sleep_sec=$down_sleep_sec io_duration_sec=$io_duration_sec"

kfastblock_prepare_or_reuse_dev_cluster "$repo_root" "$config_file"
kfastblock_capture_context "$config_file" "$run_dir"
monitor_addr="$(kfastblock_resolve_monitor_addr "$config_file")"

kfastblock_create_image "$repo_root" "$config_file" "$pool_name" "$image_name"
kfastblock_run_rawctl_checks "$repo_root" "$monitor_addr" "$pool_name" "$image_name"
kfastblock_build_and_load_module "$repo_root"
kfastblock_attach_volume "$repo_root" "$monitor_addr" "$pool_name" "$image_name"
detach_needed=1
device_path="$(kfastblock_resolve_device)"

"$repo_root/kfastblock/tool/kfastblock-admin" show \
    --pool-name "$pool_name" \
    --image-name "$image_name" > "$run_dir/admin-before.txt"

write_verify_block 0 baseline

deadline=$((SECONDS + io_duration_sec))
fault_io_worker "$deadline" &
worker_pid="$!"

sleep 2
echo "inject fault: restart osd-$target_osd"
mark_osd_stopped
kfastblock_restart_local_osd "$repo_root" "$target_osd" "$down_sleep_sec"

kfastblock_wait_cluster_active "$repo_root" "$config_file"
"$repo_root/kfastblock/tool/kfastblock-admin" force-refresh \
    --pool-name "$pool_name" \
    --image-name "$image_name" \
    --scope all >/dev/null
"$repo_root/kfastblock/tool/kfastblock-admin" reset-leaders \
    --pool-name "$pool_name" \
    --image-name "$image_name" >/dev/null

wait "$worker_pid"
worker_pid=""

wait_for_recovery_io 1 recovery-a
wait_for_recovery_io 2 recovery-b

"$repo_root/kfastblock/tool/kfastblock-admin" show \
    --pool-name "$pool_name" \
    --image-name "$image_name" > "$run_dir/admin-after.txt"

echo "fault injection ok: image=$image_name target_osd=$target_osd"
