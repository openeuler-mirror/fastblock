#!/bin/bash
# Print common env knobs for kfastblock RDMA scripts.
cat <<'EOT'
KFASTBLOCK_OSD_TRANSPORT   tcp|rdma|auto (default rdma for RDMA scripts)
KFASTBLOCK_RDMA_IO_ROUNDS  multi-io rounds (default 8)
KFASTBLOCK_RDMA_PARALLEL_JOBS / _ROUNDS
KFASTBLOCK_SEQ_BLOCKS      seq-64k block count (default 16)
KFASTBLOCK_READ_ROUNDS     read-only read count (default 8)
KFASTBLOCK_BS_LIST         bs-sweep sizes, e.g. "4096 8192 16384"
KFASTBLOCK_IO_TIMEOUT_S    dd timeout seconds (default 30)
KFASTBLOCK_SUITE_PARALLEL=1 / KFASTBLOCK_SUITE_SEQ=1
KFASTBLOCK_FORCE_RELOAD_MODULE=1
KFASTBLOCK_KEEP_VOLUME=1
KFASTBLOCK_POOL / KFASTBLOCK_IMAGE
EOT
