#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
config_file="$repo_root/.vstart/etc/fastblock/fastblock.json"
monitor_addr=""
image_name="${KFASTBLOCK_SMOKE_IMAGE:-kfb-smoke-$(date +%s)}"
payload_file="$(mktemp /tmp/kfb-smoke-payload.XXXXXX)"
readback_file="$(mktemp /tmp/kfb-smoke-readback.XXXXXX)"
device_path=""
detach_needed=0

cleanup() {
    if [ "$detach_needed" = "1" ]; then
        "$repo_root/kfastblock/tool/kfastblock-admin" detach \
            --pool-name fb \
            --image-name "$image_name" >/dev/null 2>&1 || true
    fi
    rm -f "$payload_file" "$readback_file"
}
trap cleanup EXIT

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root" >&2
    exit 1
fi

detach_all_kfastblock_volumes() {
    local line pool image

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

wait_cluster_active() {
    local attempt status_output

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

resolve_monitor_addr() {
    monitor_addr="$(jq -r '.mon_rpc_address' "$config_file")"
    if [ -z "$monitor_addr" ] || [ "$monitor_addr" = "null" ]; then
        echo "failed to resolve monitor address from $config_file" >&2
        exit 1
    fi
}

resolve_kfb_device() {
    device_path="$(ls -1 /dev/kfb* 2>/dev/null | sort | head -n 1 || true)"
    if [ -z "$device_path" ]; then
        echo "kfastblock device not found" >&2
        exit 1
    fi
}

detach_all_kfastblock_volumes

if ! rdma link show | grep -q '^link rdmanic/'; then
    "$repo_root/scripts/create-rdma-rxe.sh" -n rdmanic
fi

"$repo_root/vstart.sh" -m dev -c 3 -r 3 -C 1 -n rdmanic
wait_cluster_active
resolve_monitor_addr

"$repo_root/monitor/fastblock-client" \
    -conf="$config_file" \
    -op=createimage \
    -poolname=fb \
    -imagename="$image_name" \
    -imagesize=67108864 >/dev/null

"$repo_root/kfastblock/tool/kfastblock-rawctl" \
    get-image-info \
    --addr "${monitor_addr}:3334" \
    --pool-name fb \
    --image-name "$image_name" >/dev/null

"$repo_root/kfastblock/tool/kfastblock-rawctl" \
    get-cluster-map \
    --addr "${monitor_addr}:3334" \
    --pool-id 1 >/dev/null

make -C "$repo_root/kfastblock" all >/dev/null
if ! lsmod | awk '$1 == "kfastblock" { found = 1 } END { exit(found ? 0 : 1) }'; then
    insmod "$repo_root/kfastblock/kfastblock.ko"
fi

"$repo_root/kfastblock/tool/kfastblock-admin" attach \
    --monitor-addr "${monitor_addr}:3334" \
    --pool-name fb \
    --image-name "$image_name"
detach_needed=1

resolve_kfb_device

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
