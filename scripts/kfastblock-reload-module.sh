#!/bin/bash
# Rebuild and reload kfastblock against the running kernel.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"
kfastblock_require_root
kfastblock_detach_all_volumes "$REPO_ROOT" || true
if lsmod | awk '$1=="kfastblock"{f=1} END{exit f?0:1}'; then
  rmmod kfastblock
fi
KDIR="/lib/modules/$(uname -r)/build"
make -C "$REPO_ROOT/kfastblock" KDIR="$KDIR" modules
insmod "$REPO_ROOT/kfastblock/kfastblock.ko"
bash "$REPO_ROOT/scripts/check-kfastblock-rdma-params.sh"
echo "RELOAD_OK $(modinfo -F version "$REPO_ROOT/kfastblock/kfastblock.ko" 2>/dev/null || true)"
