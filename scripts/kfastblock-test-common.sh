#!/bin/bash

set -euo pipefail

kfastblock_repo_root() {
    cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

kfastblock_require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "run as root" >&2
        exit 1
    fi
}

kfastblock_artifact_dir() {
    local repo_root="$1"
    local test_name="$2"
    local root_dir="${KFASTBLOCK_TEST_ARTIFACT_ROOT:-$repo_root/.artifacts/kfastblock-tests}"
    local run_dir="$root_dir/${test_name}-$(date +%Y%m%d-%H%M%S)"

    mkdir -p "$run_dir"
    printf '%s\n' "$run_dir"
}

kfastblock_start_logging() {
    local log_file="$1"

    mkdir -p "$(dirname "$log_file")"
    exec > >(tee -a "$log_file") 2>&1
}

kfastblock_detach_all_volumes() {
    local repo_root="$1"
    local line
    local pool
    local image

    while IFS= read -r line; do
        [ -z "$line" ] && continue
        pool="$(printf '%s\n' "$line" | sed -n 's/.*pool=\([^ ]*\).*/\1/p')"
        image="$(printf '%s\n' "$line" | sed -n 's/.*image=\([^ ]*\).*/\1/p')"
        [ -z "$pool" ] && continue
        [ -z "$image" ] && continue
        "$repo_root/kfastblock/tool/kfastblock-admin" detach \
            --pool-name "$pool" \
            --image-name "$image" >/dev/null 2>&1 || true
    done < <("$repo_root/kfastblock/tool/kfastblock-admin" list 2>/dev/null || true)
}

kfastblock_wait_cluster_active() {
    local repo_root="$1"
    local config_file="$2"
    local attempt
    local status_output=""

    for attempt in $(seq 1 30); do
        status_output="$("$repo_root/monitor/fastblock-client" \
            -conf="$config_file" \
            -op=status 2>/dev/null || true)"
        if printf '%s\n' "$status_output" | grep -q "3 osds: 3 up, 3 in" &&
           printf '%s\n' "$status_output" | grep -q "8  active"; then
            return 0
        fi
        sleep 2
    done

    echo "cluster did not reach 3 up / 3 in / 8 active" >&2
    printf '%s\n' "$status_output" >&2
    return 1
}

kfastblock_prepare_dev_cluster() {
    local repo_root="$1"
    local config_file="$2"
    local nic="${KFASTBLOCK_DEV_NIC:-rdmanic}"
    local osd_count="${KFASTBLOCK_DEV_OSDCOUNT:-3}"
    local replica_count="${KFASTBLOCK_DEV_REPLICA:-3}"
    local cores="${KFASTBLOCK_DEV_CORES:-1}"

    kfastblock_detach_all_volumes "$repo_root"
    if ! rdma link show | grep -q "^link ${nic}/"; then
        "$repo_root/scripts/create-rdma-rxe.sh" -n "$nic"
    fi

    "$repo_root/vstart.sh" -m dev -c "$osd_count" -r "$replica_count" -C "$cores" -n "$nic"
    kfastblock_wait_cluster_active "$repo_root" "$config_file"
}

kfastblock_resolve_monitor_addr() {
    local config_file="$1"
    local monitor_addr

    monitor_addr="$(jq -r '.mon_rpc_address' "$config_file")"
    if [ -z "$monitor_addr" ] || [ "$monitor_addr" = "null" ]; then
        echo "failed to resolve monitor address from $config_file" >&2
        return 1
    fi

    printf '%s\n' "$monitor_addr"
}

kfastblock_create_image() {
    local repo_root="$1"
    local config_file="$2"
    local pool_name="$3"
    local image_name="$4"
    local image_size="${5:-67108864}"

    "$repo_root/monitor/fastblock-client" \
        -conf="$config_file" \
        -op=createimage \
        -poolname="$pool_name" \
        -imagename="$image_name" \
        -imagesize="$image_size" >/dev/null
}

kfastblock_run_rawctl_checks() {
    local repo_root="$1"
    local monitor_addr="$2"
    local pool_name="$3"
    local image_name="$4"
    local pool_id="${5:-1}"

    "$repo_root/kfastblock/tool/kfastblock-rawctl" \
        get-image-info \
        --addr "${monitor_addr}:3334" \
        --pool-name "$pool_name" \
        --image-name "$image_name" >/dev/null

    "$repo_root/kfastblock/tool/kfastblock-rawctl" \
        get-cluster-map \
        --addr "${monitor_addr}:3334" \
        --pool-id "$pool_id" >/dev/null
}

kfastblock_build_and_load_module() {
    local repo_root="$1"

    make -C "$repo_root/kfastblock" all >/dev/null
    if ! lsmod | awk '$1 == "kfastblock" { found = 1 } END { exit(found ? 0 : 1) }'; then
        insmod "$repo_root/kfastblock/kfastblock.ko"
    fi
}

kfastblock_attach_volume() {
    local repo_root="$1"
    local monitor_addr="$2"
    local pool_name="$3"
    local image_name="$4"

    "$repo_root/kfastblock/tool/kfastblock-admin" attach \
        --monitor-addr "${monitor_addr}:3334" \
        --pool-name "$pool_name" \
        --image-name "$image_name"
}

kfastblock_detach_volume() {
    local repo_root="$1"
    local pool_name="$2"
    local image_name="$3"

    "$repo_root/kfastblock/tool/kfastblock-admin" detach \
        --pool-name "$pool_name" \
        --image-name "$image_name"
}

kfastblock_resolve_device() {
    local device_path

    device_path="$(ls -1 /dev/kfb* 2>/dev/null | sort | head -n 1 || true)"
    if [ -z "$device_path" ]; then
        echo "kfastblock device not found" >&2
        return 1
    fi

    printf '%s\n' "$device_path"
}

kfastblock_capture_context() {
    local config_file="$1"
    local run_dir="$2"

    if [ -f "$config_file" ]; then
        cp "$config_file" "$run_dir/fastblock.json"
    fi
    uname -a > "$run_dir/uname.txt"
    if [ -r "/boot/config-$(uname -r)" ]; then
        cp "/boot/config-$(uname -r)" "$run_dir/kernel-config"
    fi
}
