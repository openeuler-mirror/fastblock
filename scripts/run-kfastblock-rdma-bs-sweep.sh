#!/bin/bash
# Sweep block sizes 4K and 8K (two 4K) on RDMA path.
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
IMAGE="${KFASTBLOCK_IMAGE:-rdma-bs-$(date +%s)}"
kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
  --monitor-addr "${MON}:3334" --pool-name "$POOL" --image-name "$IMAGE" \
  --osd-transport rdma
DEV="$(kfastblock_resolve_device)"
for bs in 4096 8192; do
  count=$((bs/4096))
  pay=/tmp/bs-$bs.pay; rb=/tmp/bs-$bs.rb
  dd if=/dev/urandom of="$pay" bs=4096 count=$count status=none
  timeout 30 dd if="$pay" of="$DEV" bs=4096 count=$count oflag=direct seek=0 status=none
  timeout 30 dd if="$DEV" of="$rb" bs=4096 count=$count iflag=direct skip=0 status=none
  cmp -n "$bs" "$pay" "$rb"
  echo "bs=$bs OK"
done
kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
echo "RDMA_BS_SWEEP_OK"
