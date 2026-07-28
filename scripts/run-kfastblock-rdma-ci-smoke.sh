#!/bin/bash
# CI-oriented: reload module then run minimal suite (cluster must be up).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
bash scripts/kfastblock-reload-module.sh
bash scripts/run-kfastblock-rdma-suite.sh
echo "RDMA_CI_SMOKE_OK"
