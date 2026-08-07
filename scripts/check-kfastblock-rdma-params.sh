#!/bin/bash
# Sanity-check kfastblock RDMA module params exist after insmod.
set -euo pipefail
need=(
  rdma_pool_enable
  rdma_cm_timeout_ms rdma_io_timeout_ms rdma_recv_depth
  rdma_use_cq_notify
  rdma_qp_max_send_wr rdma_qp_max_recv_wr
  rdma_retry_count rdma_rnr_retry_count rdma_signal_all
  rdma_send_ok rdma_send_err rdma_recv_ok rdma_recv_err
  rdma_exchange_ok rdma_exchange_err rdma_exchange_stale
  rdma_connect_ok rdma_connect_err rdma_connect_timeout
  rdma_io_timeout_total rdma_dma_map_err
  rdma_wc_err
  rdma_pool_hit rdma_pool_miss rdma_pool_evict
  rdma_pool_reclaim
  rdma_pool_invalidate_broken
  rdma_pool_put_fail
  rdma_pool_idle_max_age_s rdma_pool_max_idle
  xport_probe_cache_ttl_ms xport_probe_cache_hits xport_probe_cache_misses
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
