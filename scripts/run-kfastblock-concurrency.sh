#!/bin/bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/kfastblock-test-common.sh"

repo_root="$(kfastblock_repo_root)"
config_file="$repo_root/.vstart/etc/fastblock/fastblock.json"
pool_name="${KFASTBLOCK_CONCURRENCY_POOL:-fb}"
image_name="${KFASTBLOCK_CONCURRENCY_IMAGE:-kfb-concurrency-$(date +%s)}"
duration_sec="${KFASTBLOCK_CONCURRENCY_DURATION_SEC:-45}"
io_workers="${KFASTBLOCK_CONCURRENCY_IO_WORKERS:-4}"
open_workers="${KFASTBLOCK_CONCURRENCY_OPEN_WORKERS:-2}"
attach_cycles="${KFASTBLOCK_CONCURRENCY_ATTACH_CYCLES:-8}"
refresh_interval_sec="${KFASTBLOCK_CONCURRENCY_REFRESH_INTERVAL_SEC:-1}"
dispatch_window="${KFASTBLOCK_CONCURRENCY_DISPATCH_WINDOW:-}"
run_dir="$(kfastblock_artifact_dir "$repo_root" concurrency)"
log_file="$run_dir/concurrency.log"
monitor_addr=""
device_state_file="$run_dir/current_device"
detach_needed=0
stop_requested=0
bg_pids=()

cleanup() {
    local pid

    stop_requested=1
    for pid in "${bg_pids[@]:-}"; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    for pid in "${bg_pids[@]:-}"; do
        wait "$pid" >/dev/null 2>&1 || true
    done
    if [ "$detach_needed" = "1" ]; then
        kfastblock_detach_volume "$repo_root" "$pool_name" "$image_name" >/dev/null 2>&1 || true
    fi
    kfastblock_release_test_lock
}

on_error() {
    kfastblock_capture_failure_snapshot "$repo_root" "$run_dir" "$pool_name" "$image_name"
}

trap on_error ERR
trap cleanup EXIT

write_current_device() {
    printf '%s\n' "$1" > "$device_state_file"
}

read_current_device() {
    cat "$device_state_file" 2>/dev/null || true
}

attach_and_publish_device() {
    local device_path

    kfastblock_attach_volume "$repo_root" "$monitor_addr" "$pool_name" "$image_name" >/dev/null
    if [ -n "$dispatch_window" ]; then
        kfastblock_set_dispatch_window "$repo_root" "$pool_name" "$image_name" \
            "$dispatch_window"
    fi
    device_path="$(kfastblock_resolve_device)"
    write_current_device "$device_path"
    detach_needed=1
    printf '%s\n' "$device_path"
}

refresh_worker() {
    local deadline="$1"
    local ok_count=0
    local fail_count=0

    while [ "$SECONDS" -lt "$deadline" ] && [ "$stop_requested" = "0" ]; do
        if "$repo_root/kfastblock/tool/kfastblock-admin" force-refresh \
            --pool-name "$pool_name" \
            --image-name "$image_name" \
            --scope all >/dev/null 2>&1; then
            ok_count=$((ok_count + 1))
        else
            fail_count=$((fail_count + 1))
        fi
        sleep "$refresh_interval_sec"
    done

    printf 'refresh ok=%d fail=%d\n' "$ok_count" "$fail_count" > "$run_dir/refresh.summary"
}

io_worker() {
    local worker_id="$1"
    local deadline="$2"
    local payload_file="$run_dir/io-${worker_id}.payload"
    local readback_file="$run_dir/io-${worker_id}.readback"
    local iter=0
    local retry_count=0
    local device_path
    local block

    while [ "$SECONDS" -lt "$deadline" ] && [ "$stop_requested" = "0" ]; do
        device_path="$(read_current_device)"
        if [ -z "$device_path" ] || [ ! -b "$device_path" ]; then
            sleep 0.2
            continue
        fi

        block=$((worker_id * 128 + (iter % 32)))
        printf 'KFB_CONC_%02d_%08d_%s' "$worker_id" "$iter" "$image_name" | dd \
            of="$payload_file" \
            bs=4096 \
            count=1 \
            conv=sync \
            status=none

        if ! dd if="$payload_file" \
            of="$device_path" \
            bs=4096 \
            count=1 \
            seek="$block" \
            oflag=direct \
            conv=notrunc \
            status=none 2>/dev/null; then
            retry_count=$((retry_count + 1))
            sleep 0.1
            continue
        fi

        if ! dd if="$device_path" \
            of="$readback_file" \
            bs=4096 \
            count=1 \
            skip="$block" \
            iflag=direct \
            status=none 2>/dev/null; then
            retry_count=$((retry_count + 1))
            sleep 0.1
            continue
        fi

        if ! cmp -n 4096 "$payload_file" "$readback_file"; then
            retry_count=$((retry_count + 1))
            sleep 0.1
            continue
        fi
        iter=$((iter + 1))
    done

    printf 'worker=%d iterations=%d retries=%d\n' \
        "$worker_id" "$iter" "$retry_count" > "$run_dir/io-${worker_id}.summary"
}

open_worker() {
    local worker_id="$1"
    local deadline="$2"
    local device_path
    local fd
    local opens=0

    while [ "$SECONDS" -lt "$deadline" ] && [ "$stop_requested" = "0" ]; do
        device_path="$(read_current_device)"
        if [ -z "$device_path" ] || [ ! -b "$device_path" ]; then
            sleep 0.1
            continue
        fi

        if { exec {fd}<>"$device_path"; } 2>/dev/null; then
            opens=$((opens + 1))
            sleep 0.05
            exec {fd}>&-
            sleep 0.05
        else
            sleep 0.1
        fi
    done

    printf 'worker=%d opens=%d\n' "$worker_id" "$opens" > "$run_dir/open-${worker_id}.summary"
}

attach_detach_worker() {
    local deadline="$1"
    local cycles=0
    local busy_count=0
    local detach_ok=0
    local attach_ok=0
    local device_path=""

    if [ "$attach_cycles" -le 0 ]; then
        printf 'cycles=0 detach_ok=0 attach_ok=0 busy=0 skipped=1\n' \
            > "$run_dir/attach-detach.summary"
        return 0
    fi

    while [ "$SECONDS" -lt "$deadline" ] && [ "$stop_requested" = "0" ] && [ "$cycles" -lt "$attach_cycles" ]; do
        if kfastblock_detach_volume "$repo_root" "$pool_name" "$image_name" >/dev/null 2>&1; then
            detach_ok=$((detach_ok + 1))
            detach_needed=0
            : > "$device_state_file"
            sleep 0.2
            device_path="$(attach_and_publish_device)"
            attach_ok=$((attach_ok + 1))
            cycles=$((cycles + 1))
            echo "reattach cycle=$cycles device=$device_path"
        else
            busy_count=$((busy_count + 1))
            sleep 0.2
        fi
    done

    if [ "$detach_ok" -eq 0 ] || [ "$attach_ok" -eq 0 ]; then
        echo "attach/detach worker did not complete any successful cycle" >&2
        return 1
    fi

    printf 'cycles=%d detach_ok=%d attach_ok=%d busy=%d\n' \
        "$cycles" "$detach_ok" "$attach_ok" "$busy_count" > "$run_dir/attach-detach.summary"
}

kfastblock_require_root
kfastblock_acquire_test_lock "$repo_root"
kfastblock_start_logging "$log_file"
echo "artifact_dir=$run_dir"
echo "duration_sec=$duration_sec io_workers=$io_workers open_workers=$open_workers attach_cycles=$attach_cycles"
echo "dispatch_window=${dispatch_window:-default}"

kfastblock_prepare_or_reuse_dev_cluster "$repo_root" "$config_file"
kfastblock_capture_context "$config_file" "$run_dir"
monitor_addr="$(kfastblock_resolve_monitor_addr "$config_file")"

kfastblock_create_image "$repo_root" "$config_file" "$pool_name" "$image_name"
kfastblock_run_rawctl_checks "$repo_root" "$monitor_addr" "$pool_name" "$image_name"
kfastblock_build_and_load_module "$repo_root"
attach_and_publish_device >/dev/null
kfastblock_show_volume "$repo_root" "$pool_name" "$image_name" \
    > "$run_dir/admin-before.txt"

deadline=$((SECONDS + duration_sec))
refresh_worker "$deadline" &
bg_pids+=("$!")
for worker_id in $(seq 1 "$io_workers"); do
    io_worker "$worker_id" "$deadline" &
    bg_pids+=("$!")
done
for worker_id in $(seq 1 "$open_workers"); do
    open_worker "$worker_id" "$deadline" &
    bg_pids+=("$!")
done
if [ "$attach_cycles" -gt 0 ]; then
    attach_detach_worker "$deadline" &
    bg_pids+=("$!")
else
    printf 'cycles=0 detach_ok=0 attach_ok=0 busy=0 skipped=1\n' \
        > "$run_dir/attach-detach.summary"
fi

for pid in "${bg_pids[@]}"; do
    if ! wait "$pid"; then
        stop_requested=1
        for other_pid in "${bg_pids[@]}"; do
            kill "$other_pid" >/dev/null 2>&1 || true
        done
        for other_pid in "${bg_pids[@]}"; do
            wait "$other_pid" >/dev/null 2>&1 || true
        done
        echo "concurrency worker failed" >&2
        exit 1
    fi
done
bg_pids=()

kfastblock_show_volume "$repo_root" "$pool_name" "$image_name" \
    > "$run_dir/admin-after.txt"

echo "concurrency ok: image=$image_name device=$(read_current_device)"
