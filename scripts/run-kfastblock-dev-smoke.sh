#!/bin/bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/kfastblock-test-common.sh"

repo_root="$(kfastblock_repo_root)"
config_file="$repo_root/.vstart/etc/fastblock/fastblock.json"
pool_name="${KFASTBLOCK_SMOKE_POOL:-fb}"
image_name="${KFASTBLOCK_SMOKE_IMAGE:-kfb-smoke-$(date +%s)}"
run_dir="$(kfastblock_artifact_dir "$repo_root" smoke)"
log_file="$run_dir/smoke.log"
payload_file="$run_dir/payload.bin"
readback_file="$run_dir/readback.bin"
monitor_addr=""
device_path=""
detach_needed=0

cleanup() {
    kfastblock_release_test_lock
    if [ "$detach_needed" = "1" ]; then
        kfastblock_detach_volume "$repo_root" "$pool_name" "$image_name" >/dev/null 2>&1 || true
    fi
    rm -f "$payload_file" "$readback_file"
}

on_error() {
    kfastblock_capture_failure_snapshot "$repo_root" "$run_dir" "$pool_name" "$image_name"
}

trap on_error ERR
trap cleanup EXIT

kfastblock_require_root
kfastblock_acquire_test_lock "$repo_root"
kfastblock_start_logging "$log_file"
echo "artifact_dir=$run_dir"

kfastblock_prepare_dev_cluster "$repo_root" "$config_file"
kfastblock_capture_context "$config_file" "$run_dir"
monitor_addr="$(kfastblock_resolve_monitor_addr "$config_file")"

kfastblock_create_image "$repo_root" "$config_file" "$pool_name" "$image_name"
kfastblock_run_rawctl_checks "$repo_root" "$monitor_addr" "$pool_name" "$image_name"
kfastblock_build_and_load_module "$repo_root"
kfastblock_attach_volume "$repo_root" "$monitor_addr" "$pool_name" "$image_name"
detach_needed=1

device_path="$(kfastblock_resolve_device)"

printf 'KFASTBLOCK_SMOKE_%s' "$image_name" | dd \
    of="$payload_file" \
    bs=4096 \
    count=1 \
    conv=sync \
    status=none

dd if="$payload_file" \
   of="$device_path" \
   bs=4096 \
   count=1 \
   oflag=direct \
   status=none

dd if="$device_path" \
   of="$readback_file" \
   bs=4096 \
   count=1 \
   iflag=direct \
   status=none

cmp -n 4096 "$payload_file" "$readback_file"

echo "smoke ok: $device_path image=$image_name"
