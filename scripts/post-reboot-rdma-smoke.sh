#!/bin/bash
# Post-reboot kfastblock RDMA 4K smoke after kvzalloc fix.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"

LOG_DIR="/tmp/rdma-post-reboot-$$"
mkdir -p "$LOG_DIR"
exec > >(tee -a "$LOG_DIR/run.log") 2>&1

echo "=== post-reboot RDMA smoke $(date -Is) ==="
echo "repo=$REPO_ROOT log=$LOG_DIR"

kfastblock_require_root

# Soft-RoCE
bash "$REPO_ROOT/scripts/setup-soft-roce.sh"
rdma link show

# Cluster
pkill -x fastblock-osd 2>/dev/null || true
pkill -x fastblock-mon 2>/dev/null || true
sleep 1
KFASTBLOCK_TEST_REUSE_CLUSTER=0 \
  kfastblock_prepare_dev_cluster "$REPO_ROOT" \
    "$REPO_ROOT/.vstart/etc/fastblock/fastblock.json" || {
  # prepare_dev_cluster may re-run vstart fully
  ./vstart.sh -m dev -c 1 -r 1 -C 1 -n rdmanic
  sleep 35
}

# Module — always build against the *running* kernel tree.
# kfastblock/Makefile prefers /root/kernel/.hostbuild when present, which often
# has a different UTS_RELEASE and yields "Invalid module format" on insmod.
kfastblock_detach_all_volumes "$REPO_ROOT" || true
if lsmod | awk '$1=="kfastblock"{found=1} END{exit found?0:1}'; then
  rmmod kfastblock || true
fi
KDIR="/lib/modules/$(uname -r)/build"
make -C "$REPO_ROOT/kfastblock" KDIR="$KDIR" all
insmod "$REPO_ROOT/kfastblock/kfastblock.ko"
echo "rdma_connect_ok=$(cat /sys/module/kfastblock/parameters/rdma_connect_ok)"
echo "rdma_connect_err=$(cat /sys/module/kfastblock/parameters/rdma_connect_err)"

CONF="$REPO_ROOT/.vstart/etc/fastblock/fastblock.json"
MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL=fb
IMAGE="rdma-kvzalloc-$(date +%s)"

kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
# Force RDMA data plane (default attach is tcp).
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
  --monitor-addr "${MON}:3334" \
  --pool-name "$POOL" \
  --image-name "$IMAGE" \
  --osd-transport rdma
DEV="$(kfastblock_resolve_device)"
echo "device=$DEV image=$IMAGE"
./kfastblock/tool/kfastblock-admin show --pool-name "$POOL" --image-name "$IMAGE" | \
  grep -E 'osd_transport|xport\.|rdma_cache' || true

PAYLOAD="$LOG_DIR/payload.bin"
READBACK="$LOG_DIR/readback.bin"
printf 'KFASTBLOCK_RDMA_KVZ_%s' "$IMAGE" | dd of="$PAYLOAD" bs=4096 count=1 conv=sync status=none

# Bound I/O so we never hang forever in this script
timeout 30 dd if="$PAYLOAD" of="$DEV" bs=4096 count=1 oflag=direct status=none
timeout 30 dd if="$DEV" of="$READBACK" bs=4096 count=1 iflag=direct status=none
cmp -n 4096 "$PAYLOAD" "$READBACK"

echo "=== counters after IO ==="
for p in rdma_connect_ok rdma_connect_err rdma_exchange_ok rdma_exchange_err rdma_dma_map_err; do
  echo "$p=$(cat /sys/module/kfastblock/parameters/$p)"
done
./kfastblock/tool/kfastblock-admin show --pool-name "$POOL" --image-name "$IMAGE" | \
  grep -E 'pipeline\.|rdma|xport\.|volume\.(inflight|io_|last_failure|health)' || true

kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
echo "SMOKE_OK image=$IMAGE device=$DEV log=$LOG_DIR"

# Optional extended multi-round regression after basic smoke (set to 1).
if [ "${KFASTBLOCK_RDMA_MULTI_AFTER_SMOKE:-0}" = "1" ]; then
  KFASTBLOCK_KEEP_VOLUME=0 \
    bash "$REPO_ROOT/scripts/run-kfastblock-rdma-multi-io.sh" || exit 1
fi
