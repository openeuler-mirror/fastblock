#!/bin/bash
# Run a minimal RDMA regression suite against a live cluster.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
echo "=== RDMA suite $(date -Is) ==="
bash scripts/check-kfastblock-rdma-params.sh
bash scripts/run-kfastblock-rdma-4k-verify.sh
KFASTBLOCK_RDMA_IO_ROUNDS="${KFASTBLOCK_RDMA_IO_ROUNDS:-4}" \
  bash scripts/run-kfastblock-rdma-multi-io.sh
bash scripts/run-kfastblock-tcp-4k-verify.sh
bash scripts/run-kfastblock-auto-4k-verify.sh
if [ "${KFASTBLOCK_SUITE_PARALLEL:-0}" = "1" ]; then
  bash scripts/run-kfastblock-rdma-parallel-4k.sh
fi
echo "RDMA_SUITE_OK"
