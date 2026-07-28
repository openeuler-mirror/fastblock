#!/bin/bash
cat <<'EOT'
run-kfastblock-rdma-suite.sh runs:
  1) check-kfastblock-rdma-params.sh
  2) run-kfastblock-rdma-4k-verify.sh
  3) run-kfastblock-rdma-multi-io.sh
  4) run-kfastblock-tcp-4k-verify.sh
  5) run-kfastblock-auto-4k-verify.sh
optional:
  KFASTBLOCK_SUITE_PARALLEL=1 -> parallel-4k
  KFASTBLOCK_SUITE_SEQ=1      -> seq-64k
EOT
