#!/bin/bash
# Sanity-check kfastblock RDMA module params exist after insmod.
set -euo pipefail
need=(
  rdma_cm_timeout_ms rdma_io_timeout_ms rdma_recv_depth
  rdma_exchange_ok rdma_exchange_err rdma_exchange_stale
  rdma_connect_ok rdma_pool_hit rdma_pool_miss
  rdma_pool_idle_max_age_s rdma_pool_max_idle
  rdma_object_log
  object_io_max_attempts
)
missing=0
for n in "${need[@]}"; do
  p="/sys/module/kfastblock/parameters/$n"
  if [ ! -r "$p" ]; then
    echo "MISSING $n"
    missing=1
  else
    echo "$n=$(tr -d '\n' < "$p")"
  fi
done
[ "$missing" -eq 0 ] && echo "RDMA_PARAMS_OK" || exit 1
