#!/bin/bash
# Sanity-check kfastblock RDMA module params exist after insmod.
set -euo pipefail
need=(
  rdma_pool_enable
  rdma_cm_timeout_ms rdma_io_timeout_ms rdma_recv_depth
  rdma_use_cq_notify
  rdma_poll_backoff_threshold rdma_poll_backoff_us
  rdma_qp_max_send_wr rdma_qp_max_recv_wr
  rdma_retry_count rdma_rnr_retry_count rdma_signal_all
  rdma_send_ok rdma_send_err rdma_recv_ok rdma_recv_err
  rdma_exchange_ok rdma_exchange_err rdma_exchange_stale
  rdma_connect_ok rdma_connect_err rdma_connect_timeout
  rdma_reconnect_total
  rdma_io_timeout_total rdma_dma_map_err
  rdma_send_timeout rdma_recv_timeout
  rdma_wc_err rdma_wc_flush_err rdma_wc_retry_err
  rdma_wc_rnr_err rdma_wc_remote_err rdma_wc_fatal_err rdma_wc_other_err
  rdma_wc_unknown_id
  rdma_poll_empty_total
  rdma_conn_error_transitions
  rdma_inline_threshold
  rdma_send_bytes rdma_recv_bytes
  rdma_send_wr_total rdma_recv_wr_total
  rdma_connect_lat_min_us rdma_connect_lat_max_us rdma_connect_lat_total_us
  rdma_send_lat_min_us rdma_send_lat_max_us rdma_send_lat_avg_us
  rdma_recv_lat_min_us rdma_recv_lat_max_us rdma_recv_lat_avg_us
  rdma_pool_hit rdma_pool_miss rdma_pool_evict
  rdma_pool_reclaim rdma_pool_invalidate_broken
  rdma_pool_put_fail rdma_pool_destroy_busy rdma_pool_aged_out
  rdma_pool_idle_max_age_s rdma_pool_max_idle
  rdma_cached_idle_max_age_s rdma_cached_aged_out
  rdma_xport_write_ops rdma_xport_read_ops rdma_xport_delete_ops
  rdma_xport_io_err rdma_xport_fallback_tcp
  recovery_rdma_invalidate_leader recovery_rdma_flush
  xport_probe_cache_ttl_ms xport_probe_cache_hits xport_probe_cache_misses
  xport_probe_cache_total
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
