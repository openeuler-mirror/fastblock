#!/bin/bash
# Run 4K verify for each of tcp / rdma / auto (cluster must be up).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"

kfastblock_require_root
CONF="$REPO_ROOT/.vstart/etc/fastblock/fastblock.json"
[ -f "$CONF" ] || { echo "missing conf" >&2; exit 1; }
pgrep -x fastblock-mon >/dev/null || { echo "mon down" >&2; exit 1; }
lsmod | awk '$1=="kfastblock"{f=1} END{exit f?0:1}' || {
  make -C kfastblock KDIR=/lib/modules/$(uname -r)/build modules >/dev/null
  insmod kfastblock/kfastblock.ko
}

MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL="${KFASTBLOCK_POOL:-fb}"
TIMEOUT_S="${KFASTBLOCK_IO_TIMEOUT_S:-30}"
fail=0

for xport in tcp rdma auto; do
  IMAGE="xport-${xport}-$(date +%s)"
  echo "=== transport=$xport image=$IMAGE ==="
  kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
  if ! "$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
      --monitor-addr "${MON}:3334" \
      --pool-name "$POOL" --image-name "$IMAGE" \
      --osd-transport "$xport"; then
    echo "attach failed transport=$xport"
    fail=1
    continue
  fi
  DEV="$(kfastblock_resolve_device)"
  got="$("$REPO_ROOT/kfastblock/tool/kfastblock-admin" show \
    --pool-name "$POOL" --image-name "$IMAGE" | awk -F= '/^osd_transport=/{print $2}')"
  echo "osd_transport=$got (requested=$xport)"
  pay="/tmp/mtx-$xport-pay.bin"
  rb="/tmp/mtx-$xport-rb.bin"
  printf 'XPORT_%s_%s' "$xport" "$IMAGE" | dd of="$pay" bs=4096 count=1 conv=sync status=none
  if timeout "$TIMEOUT_S" dd if="$pay" of="$DEV" bs=4096 count=1 oflag=direct status=none \
    && timeout "$TIMEOUT_S" dd if="$DEV" of="$rb" bs=4096 count=1 iflag=direct status=none \
    && cmp -n 4096 "$pay" "$rb"; then
    echo "transport=$xport OK"
  else
    echo "transport=$xport FAIL"
    fail=1
  fi
  kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
done

[ "$fail" -eq 0 ] && echo "TRANSPORT_MATRIX_OK" || {
  echo "TRANSPORT_MATRIX_FAIL" >&2
  exit 1
}
