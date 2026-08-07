#!/bin/bash
# 4K write/read with explicit --osd-transport tcp (control for RDMA scripts).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"
kfastblock_require_root
CONF="$REPO_ROOT/.vstart/etc/fastblock/fastblock.json"
[ -f "$CONF" ] || { echo "no conf" >&2; exit 1; }
lsmod | awk '$1=="kfastblock"{f=1} END{exit f?0:1}' || {
  make -C kfastblock KDIR=/lib/modules/$(uname -r)/build modules >/dev/null
  insmod kfastblock/kfastblock.ko
}
MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL="${KFASTBLOCK_POOL:-fb}"
IMAGE="${KFASTBLOCK_IMAGE:-tcp-4k-$(date +%s)}"
kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
  --monitor-addr "${MON}:3334" --pool-name "$POOL" --image-name "$IMAGE" \
  --osd-transport tcp
DEV="$(kfastblock_resolve_device)"
xport=$("$REPO_ROOT/kfastblock/tool/kfastblock-admin" show --pool-name "$POOL" --image-name "$IMAGE" | awk -F= '/^osd_transport=/{print $2}')
echo "osd_transport=$xport"
[ "$xport" = "tcp" ] || { echo "expected tcp" >&2; exit 1; }
pay=/tmp/tcp-pay.bin; rb=/tmp/tcp-rb.bin
printf 'TCP_%s' "$IMAGE" | dd of="$pay" bs=4096 count=1 conv=sync status=none
timeout 30 dd if="$pay" of="$DEV" bs=4096 count=1 oflag=direct status=none
timeout 30 dd if="$DEV" of="$rb" bs=4096 count=1 iflag=direct status=none
cmp -n 4096 "$pay" "$rb"
kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
echo "TCP_4K_VERIFY_OK"
