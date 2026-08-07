#!/bin/bash
# RDMA 4K write then multiple reads (read-path soak on one block).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"
kfastblock_require_root
CONF="$REPO_ROOT/.vstart/etc/fastblock/fastblock.json"
[ -f "$CONF" ] || { echo "no conf" >&2; exit 1; }
lsmod | awk '$1=="kfastblock"{f=1} END{exit f?0:1}' || bash scripts/kfastblock-reload-module.sh
MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL="${KFASTBLOCK_POOL:-fb}"
IMAGE="${KFASTBLOCK_IMAGE:-rdma-ro-$(date +%s)}"
READS="${KFASTBLOCK_READ_ROUNDS:-8}"
TIMEOUT_S="${KFASTBLOCK_IO_TIMEOUT_S:-30}"
kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
  --monitor-addr "${MON}:3334" --pool-name "$POOL" --image-name "$IMAGE" \
  --osd-transport rdma
DEV="$(kfastblock_resolve_device)"
pay=/tmp/ro.pay; rb=/tmp/ro.rb
printf 'RO_%s' "$IMAGE" | dd of="$pay" bs=4096 count=1 conv=sync status=none
timeout "$TIMEOUT_S" dd if="$pay" of="$DEV" bs=4096 count=1 oflag=direct status=none
err0=$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)
stale0=$(cat /sys/module/kfastblock/parameters/rdma_exchange_stale 2>/dev/null || echo 0)
for i in $(seq 1 "$READS"); do
  timeout "$TIMEOUT_S" dd if="$DEV" of="$rb" bs=4096 count=1 iflag=direct status=none
  cmp -n 4096 "$pay" "$rb"
done
err1=$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)
stale1=$(cat /sys/module/kfastblock/parameters/rdma_exchange_stale 2>/dev/null || echo 0)
[ "$((err1-err0))" -eq 0 ] && [ "$((stale1-stale0))" -eq 0 ]
kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
echo "RDMA_READ_ONLY_OK reads=$READS"
