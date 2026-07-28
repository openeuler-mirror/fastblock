#!/bin/bash
# Parallel 4K RDMA writers on one volume (N jobs, M rounds each).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"

JOBS="${KFASTBLOCK_RDMA_PARALLEL_JOBS:-4}"
ROUNDS="${KFASTBLOCK_RDMA_PARALLEL_ROUNDS:-4}"
TIMEOUT_S="${KFASTBLOCK_IO_TIMEOUT_S:-30}"

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
IMAGE="${KFASTBLOCK_IMAGE:-rdma-par-$(date +%s)}"
kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE" 268435456
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
  --monitor-addr "${MON}:3334" --pool-name "$POOL" --image-name "$IMAGE" \
  --osd-transport rdma
DEV="$(kfastblock_resolve_device)"
echo "device=$DEV jobs=$JOBS rounds=$ROUNDS"

err0="$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)"
stale0="$(cat /sys/module/kfastblock/parameters/rdma_exchange_stale 2>/dev/null || echo 0)"

worker() {
  local id="$1" r pay rb seek
  for r in $(seq 0 $((ROUNDS - 1))); do
    pay="/tmp/par-$id-$r.pay"
    rb="/tmp/par-$id-$r.rb"
    seek=$(( (id * ROUNDS + r) % 64 ))
    printf 'PAR_%u_%u_%s' "$id" "$r" "$IMAGE" | dd of="$pay" bs=4096 count=1 conv=sync status=none
    timeout "$TIMEOUT_S" dd if="$pay" of="$DEV" bs=4096 count=1 oflag=direct seek="$seek" status=none
    timeout "$TIMEOUT_S" dd if="$DEV" of="$rb" bs=4096 count=1 iflag=direct skip="$seek" status=none
    cmp -n 4096 "$pay" "$rb"
  done
  echo "worker $id OK"
}

pids=()
for j in $(seq 0 $((JOBS - 1))); do
  worker "$j" &
  pids+=($!)
done
fail=0
for p in "${pids[@]}"; do
  wait "$p" || fail=1
done

err1="$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)"
stale1="$(cat /sys/module/kfastblock/parameters/rdma_exchange_stale 2>/dev/null || echo 0)"
echo "exchange_err delta=$((err1-err0)) stale delta=$((stale1-stale0))"
kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
[ "$fail" -eq 0 ] && [ "$((err1-err0))" -eq 0 ] && [ "$((stale1-stale0))" -eq 0 ] || {
  echo "RDMA_PARALLEL_FAIL" >&2
  exit 1
}
echo "RDMA_PARALLEL_OK jobs=$JOBS rounds=$ROUNDS"
