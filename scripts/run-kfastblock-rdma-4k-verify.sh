#!/bin/bash
# Quick kfastblock RDMA 4K write/read verify (reuse running cluster when possible).
# Expects Soft-RoCE + mon/osd already up, or will start a dev cluster.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"

LOG_DIR="${KFASTBLOCK_RDMA_VERIFY_LOG:-/tmp/kfb-rdma-4k-$$}"
mkdir -p "$LOG_DIR"
exec > >(tee -a "$LOG_DIR/run.log") 2>&1

echo "=== kfastblock RDMA 4K verify $(date -Is) ==="
echo "repo=$REPO_ROOT log=$LOG_DIR"

kfastblock_require_root
bash "$REPO_ROOT/scripts/setup-soft-roce.sh" >/dev/null || true

CONF="$REPO_ROOT/.vstart/etc/fastblock/fastblock.json"
if [ ! -f "$CONF" ] || ! pgrep -x fastblock-mon >/dev/null 2>&1; then
	KFASTBLOCK_TEST_REUSE_CLUSTER=0 \
		kfastblock_prepare_dev_cluster "$REPO_ROOT" "$CONF" || {
		./vstart.sh -m dev -c 3 -r 3 -C 1 -n rdmanic
		sleep 25
	}
fi

# Reload module only when requested (default: rebuild if not loaded).
KDIR="/lib/modules/$(uname -r)/build"
if ! lsmod | awk '$1=="kfastblock"{found=1} END{exit found?0:1}'; then
	make -C "$REPO_ROOT/kfastblock" KDIR="$KDIR" modules >/dev/null
	insmod "$REPO_ROOT/kfastblock/kfastblock.ko"
elif [ "${KFASTBLOCK_FORCE_RELOAD_MODULE:-0}" = "1" ]; then
	kfastblock_detach_all_volumes "$REPO_ROOT" || true
	rmmod kfastblock || true
	make -C "$REPO_ROOT/kfastblock" KDIR="$KDIR" modules >/dev/null
	insmod "$REPO_ROOT/kfastblock/kfastblock.ko"
fi

MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL="${KFASTBLOCK_POOL:-fb}"
IMAGE="${KFASTBLOCK_IMAGE:-rdma-4k-$(date +%s)}"
TIMEOUT_S="${KFASTBLOCK_IO_TIMEOUT_S:-30}"

kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
	--monitor-addr "${MON}:3334" \
	--pool-name "$POOL" \
	--image-name "$IMAGE" \
	--osd-transport "${KFASTBLOCK_OSD_TRANSPORT:-rdma}"

DEV="$(kfastblock_resolve_device)"
echo "device=$DEV image=$IMAGE mon=$MON"
xport="$("$REPO_ROOT/kfastblock/tool/kfastblock-admin" show \
	--pool-name "$POOL" --image-name "$IMAGE" | awk -F= '/^osd_transport=/{print $2}')"
echo "osd_transport=$xport"
expect_xport="${KFASTBLOCK_OSD_TRANSPORT:-rdma}"
# auto may resolve to rdma or tcp depending on map.
if [ "$expect_xport" = "auto" ]; then
	:
elif [ "$xport" != "$expect_xport" ]; then
	echo "expected osd_transport=$expect_xport got=$xport" >&2
	exit 1
fi

PAYLOAD="$LOG_DIR/payload.bin"
READBACK="$LOG_DIR/readback.bin"
printf 'KFB_RDMA_4K_%s' "$IMAGE" | dd of="$PAYLOAD" bs=4096 count=1 conv=sync status=none

ex_before="$(cat /sys/module/kfastblock/parameters/rdma_exchange_ok)"
err_before="$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)"

timeout "$TIMEOUT_S" dd if="$PAYLOAD" of="$DEV" bs=4096 count=1 oflag=direct status=none
timeout "$TIMEOUT_S" dd if="$DEV" of="$READBACK" bs=4096 count=1 iflag=direct status=none
cmp -n 4096 "$PAYLOAD" "$READBACK"

ex_after="$(cat /sys/module/kfastblock/parameters/rdma_exchange_ok)"
err_after="$(cat /sys/module/kfastblock/parameters/rdma_exchange_err)"
echo "rdma_exchange_ok: $ex_before -> $ex_after"
echo "rdma_exchange_err: $err_before -> $err_after"
[ "$err_after" -ge "$err_before" ] || true
# New errors during this run are not allowed.
[ "$((err_after - err_before))" -eq 0 ] || {
	echo "rdma_exchange_err increased during verify" >&2
	exit 1
}

"$REPO_ROOT/kfastblock/tool/kfastblock-admin" show \
	--pool-name "$POOL" --image-name "$IMAGE" | \
	grep -E 'io_|object_io|osd_transport|health|rdma_cache' || true

kfastblock_print_rdma_counters || true
bash "$REPO_ROOT/scripts/check-kfastblock-rdma-params.sh" || true

if [ "${KFASTBLOCK_KEEP_VOLUME:-0}" != "1" ]; then
	kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
fi

echo "RDMA_4K_VERIFY_OK image=$IMAGE device=$DEV log=$LOG_DIR"
