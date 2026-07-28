#!/bin/bash
# 4K verify with --osd-transport auto (prefer RDMA when map has ports).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"
kfastblock_require_root
CONF="$REPO_ROOT/.vstart/etc/fastblock/fastblock.json"
[ -f "$CONF" ] || { echo "no conf" >&2; exit 1; }
lsmod | awk '$1=="kfastblock"{f=1} END{exit f?0:1}' || {
  bash "$REPO_ROOT/scripts/kfastblock-reload-module.sh"
}
MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL=fb
IMAGE="auto-4k-$(date +%s)"
kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
  --monitor-addr "${MON}:3334" --pool-name "$POOL" --image-name "$IMAGE" \
  --osd-transport auto
DEV="$(kfastblock_resolve_device)"
xport=$("$REPO_ROOT/kfastblock/tool/kfastblock-admin" show --pool-name "$POOL" --image-name "$IMAGE" | awk -F= '/^osd_transport=/{print $2}')
echo "osd_transport=$xport"
pay=/tmp/auto-pay.bin; rb=/tmp/auto-rb.bin
printf 'AUTO_%s' "$IMAGE" | dd of="$pay" bs=4096 count=1 conv=sync status=none
timeout 30 dd if="$pay" of="$DEV" bs=4096 count=1 oflag=direct status=none
timeout 30 dd if="$DEV" of="$rb" bs=4096 count=1 iflag=direct status=none
cmp -n 4096 "$pay" "$rb"
# Prefer seeing rdma when Soft-RoCE cluster is up; warn only if tcp.
if [ "$xport" != "rdma" ] && [ "$xport" != "auto" ] && [ "$xport" != "tcp" ]; then
  echo "unexpected transport $xport" >&2
  exit 1
fi
kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
echo "AUTO_4K_VERIFY_OK transport=$xport"
