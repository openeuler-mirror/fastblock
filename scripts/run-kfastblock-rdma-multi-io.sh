#!/bin/bash
# Multi-round 4K RDMA IO: exercises same-connection exchange reinit (recv_done).
# Write+read N times on one volume; fail if exchange_err rises or cmp fails.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"

ROUNDS="${KFASTBLOCK_RDMA_IO_ROUNDS:-8}"
TIMEOUT_S="${KFASTBLOCK_IO_TIMEOUT_S:-30}"
LOG_DIR="${KFASTBLOCK_RDMA_MULTI_LOG:-/tmp/kfb-rdma-multi-$$}"
mkdir -p "$LOG_DIR"
exec > >(tee -a "$LOG_DIR/run.log") 2>&1

echo "=== kfastblock RDMA multi-IO rounds=$ROUNDS $(date -Is) ==="
kfastblock_require_root
bash "$REPO_ROOT/scripts/setup-soft-roce.sh" >/dev/null || true

CONF="$REPO_ROOT/.vstart/etc/fastblock/fastblock.json"
[ -f "$CONF" ] || {
	echo "missing $CONF — start cluster first (vstart or post-reboot smoke)" >&2
	exit 1
}
pgrep -x fastblock-mon >/dev/null || {
	echo "fastblock-mon not running" >&2
	exit 1
}
lsmod | awk '$1=="kfastblock"{found=1} END{exit found?0:1}' || {
	KDIR="/lib/modules/$(uname -r)/build"
	make -C "$REPO_ROOT/kfastblock" KDIR="$KDIR" modules >/dev/null
	insmod "$REPO_ROOT/kfastblock/kfastblock.ko"
}

MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL="${KFASTBLOCK_POOL:-fb}"
IMAGE="${KFASTBLOCK_IMAGE:-rdma-multi-$(date +%s)}"

kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
	--monitor-addr "${MON}:3334" \
	--pool-name "$POOL" \
	--image-name "$IMAGE" \
	--osd-transport rdma
DEV="$(kfastblock_resolve_device)"
echo "device=$DEV image=$IMAGE"

err0="$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)"
ex0="$(cat /sys/module/kfastblock/parameters/rdma_exchange_ok)"
stale0="$(cat /sys/module/kfastblock/parameters/rdma_exchange_stale 2>/dev/null || echo 0)"

i=0
while [ "$i" -lt "$ROUNDS" ]; do
	pay="$LOG_DIR/p-$i.bin"
	rb="$LOG_DIR/r-$i.bin"
	# Distinct payload each round so stale-response bugs fail cmp.
	printf 'KFB_MULTI_%04d_%s' "$i" "$IMAGE" | \
		dd of="$pay" bs=4096 count=1 conv=sync status=none
	# Round-robin 4K blocks so multi-object paths get light exercise.
	seek=$((i % 16))
	timeout "$TIMEOUT_S" dd if="$pay" of="$DEV" bs=4096 count=1 \
		oflag=direct seek="$seek" status=none
	timeout "$TIMEOUT_S" dd if="$DEV" of="$rb" bs=4096 count=1 \
		iflag=direct skip="$seek" status=none
	cmp -n 4096 "$pay" "$rb"
	echo "round $i OK"
	i=$((i + 1))
done

err1="$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)"
stale1="$(cat /sys/module/kfastblock/parameters/rdma_exchange_stale 2>/dev/null || echo 0)"
ex1="$(cat /sys/module/kfastblock/parameters/rdma_exchange_ok)"
echo "rdma_exchange_ok: $ex0 -> $ex1 (delta=$((ex1 - ex0)))"
echo "rdma_exchange_err: $err0 -> $err1 (delta=$((err1 - err0)))"
echo "rdma_exchange_stale: $stale0 -> $stale1 (delta=$((stale1 - stale0)))"
[ "$((err1 - err0))" -eq 0 ] || {
	echo "exchange_err increased during multi-IO" >&2
	exit 1
}
[ "$((stale1 - stale0))" -eq 0 ] || {
	echo "exchange_stale increased during multi-IO" >&2
	exit 1
}
# At least one write+read exchange pair per round (plus leader queries).
[ "$((ex1 - ex0))" -ge "$ROUNDS" ] || {
	echo "exchange_ok did not grow enough for $ROUNDS rounds" >&2
	exit 1
}

"$REPO_ROOT/kfastblock/tool/kfastblock-admin" show \
	--pool-name "$POOL" --image-name "$IMAGE" | \
	grep -E 'io_|object_io|rdma_cache|health' || true

kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
echo "RDMA_MULTI_IO_OK rounds=$ROUNDS image=$IMAGE log=$LOG_DIR"
