#!/bin/bash
# Detach every kfastblock volume (best-effort).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
source "$REPO_ROOT/scripts/kfastblock-test-common.sh"
kfastblock_require_root
if ! lsmod | awk '$1=="kfastblock"{f=1} END{exit f?0:1}'; then
  echo "kfastblock not loaded"
  exit 0
fi
kfastblock_detach_all_volumes "$REPO_ROOT"
echo "DETACH_ALL_OK"
