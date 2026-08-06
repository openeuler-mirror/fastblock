#!/bin/bash
# RDMA 4K write-only smoke (no read) for isolating write path.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"
kfastblock_require_root
CONF="$REPO_ROOT/.vstart/etc/fastblock/fastblock.json"
[ -f "$CONF" ] || { echo "no conf" >&2; exit 1; }
lsmod | awk '$1=="kfastblock"{f=1} END{exit f?0:1}' || bash scripts/kfastblock-reload-module.sh
MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL=fb; IMAGE="rdma-wo-$(date +%s)"
kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
  --monitor-addr "${MON}:3334" --pool-name "$POOL" --image-name "$IMAGE" \
  --osd-transport rdma
DEV="$(kfastblock_resolve_device)"
pay=/tmp/wo.pay
printf 'WO_%s' "$IMAGE" | dd of="$pay" bs=4096 count=1 conv=sync status=none
timeout 30 dd if="$pay" of="$DEV" bs=4096 count=1 oflag=direct status=none
echo "write ok exchange_ok=$(cat /sys/module/kfastblock/parameters/rdma_exchange_ok)"
kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
echo "RDMA_WRITE_ONLY_OK"
