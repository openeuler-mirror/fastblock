#!/bin/bash
# Sequential 16 x 4K RDMA write+read (64KiB span) on one volume.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"
kfastblock_require_root
CONF="$REPO_ROOT/.vstart/etc/fastblock/fastblock.json"
[ -f "$CONF" ] || { echo "no conf" >&2; exit 1; }
lsmod | awk '$1=="kfastblock"{f=1} END{exit f?0:1}' || bash scripts/kfastblock-reload-module.sh
MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL=fb
IMAGE="rdma-seq-$(date +%s)"
BLOCKS="${KFASTBLOCK_SEQ_BLOCKS:-16}"
kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
  --monitor-addr "${MON}:3334" --pool-name "$POOL" --image-name "$IMAGE" \
  --osd-transport rdma
DEV="$(kfastblock_resolve_device)"
err0=$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)
for i in $(seq 0 $((BLOCKS-1))); do
  pay=/tmp/seq-$i.pay; rb=/tmp/seq-$i.rb
  printf 'SEQ%04d_%s' "$i" "$IMAGE" | dd of="$pay" bs=4096 count=1 conv=sync status=none
  timeout 30 dd if="$pay" of="$DEV" bs=4096 count=1 oflag=direct seek=$i status=none
  timeout 30 dd if="$DEV" of="$rb" bs=4096 count=1 iflag=direct skip=$i status=none
  cmp -n 4096 "$pay" "$rb"
done
err1=$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)
echo "blocks=$BLOCKS exchange_err delta=$((err1-err0))"
[ "$((err1-err0))" -eq 0 ]
kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
echo "RDMA_SEQ_OK blocks=$BLOCKS"
