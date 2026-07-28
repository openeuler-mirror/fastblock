#!/bin/bash
# Smoke: set short idle_max_age, do IO, sleep past age, do IO again (forces reconnect).
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
AGE_PARAM=/sys/module/kfastblock/parameters/rdma_pool_idle_max_age_s
AGE_PARAM_OLD="$(cat "$AGE_PARAM")"
trap 'echo "$AGE_PARAM_OLD" > "$AGE_PARAM"' EXIT
# 2 second idle max age
echo 2 > "$AGE_PARAM"
AGE_SLEEP="${KFASTBLOCK_IDLE_AGE_SLEEP:-3}"
TIMEOUT_S="${KFASTBLOCK_IO_TIMEOUT_S:-30}"

MON="$(kfastblock_resolve_monitor_addr "$CONF")"
POOL="${KFASTBLOCK_POOL:-fb}"
IMAGE="${KFASTBLOCK_IMAGE:-rdma-age-$(date +%s)}"
kfastblock_create_image "$REPO_ROOT" "$CONF" "$POOL" "$IMAGE"
"$REPO_ROOT/kfastblock/tool/kfastblock-admin" attach \
  --monitor-addr "${MON}:3334" --pool-name "$POOL" --image-name "$IMAGE" \
  --osd-transport rdma
DEV="$(kfastblock_resolve_device)"
pay=/tmp/age-pay.bin; rb=/tmp/age-rb.bin
printf 'AGE1_%s' "$IMAGE" | dd of="$pay" bs=4096 count=1 conv=sync status=none
timeout "$TIMEOUT_S" dd if="$pay" of="$DEV" bs=4096 count=1 oflag=direct status=none
timeout "$TIMEOUT_S" dd if="$DEV" of="$rb" bs=4096 count=1 iflag=direct status=none
cmp -n 4096 "$pay" "$rb"
miss0=$(cat /sys/module/kfastblock/parameters/rdma_pool_miss)
echo "sleep ${AGE_SLEEP}s for idle age..."
sleep "$AGE_SLEEP"
printf 'AGE2_%s' "$IMAGE" | dd of="$pay" bs=4096 count=1 conv=sync status=none
timeout "$TIMEOUT_S" dd if="$pay" of="$DEV" bs=4096 count=1 oflag=direct status=none
timeout "$TIMEOUT_S" dd if="$DEV" of="$rb" bs=4096 count=1 iflag=direct status=none
cmp -n 4096 "$pay" "$rb"
miss1=$(cat /sys/module/kfastblock/parameters/rdma_pool_miss)
echo "rdma_pool_miss: $miss0 -> $miss1"
kfastblock_detach_volume "$REPO_ROOT" "$POOL" "$IMAGE" || true
echo "RDMA_IDLE_AGE_OK"
