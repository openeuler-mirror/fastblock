/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#pragma once

#include "device_backend.h"
#include "pg_store.h"
#include "segment_allocator.h"
#include "types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

namespace fastblock::aos {

/**
 * Multi-PG facade sharing one segment_allocator + device (design §4 / M2).
 *
 * - create_pg allocates a fresh store_epoch for (pool_id, pg_id).
 * - All PG writes interleave on the shared open segment.
 * - recover() single-pass scans the device and routes records by
 *   (pool_id, pg_id, store_epoch).
 *
 * SPDK: all methods are reactor-thread only; completions via callback.
 */
class osd_store {
public:
  using op_complete = device_backend::op_complete;

  /**
   * segment_size: 0 => single segment over full capacity.
   * backend lifetime must exceed osd_store and all child pg_store.
   */
  osd_store(device_backend* backend, buffer_mode mode = buffer_mode::heap,
            uint64_t segment_size = 0);
  ~osd_store() = default;

  osd_store(const osd_store&) = delete;
  osd_store& operator=(const osd_store&) = delete;

  device_backend* backend() const { return backend_; }
  segment_allocator* allocator() { return alloc_.get(); }
  const segment_allocator* allocator() const { return alloc_.get(); }
  std::shared_ptr<segment_allocator> shared_allocator() const { return alloc_; }

  size_t pg_count() const { return pgs_.size(); }

  // Latest store_epoch for (pool, pg), or 0 if none.
  uint64_t latest_epoch(uint64_t pool_id, uint64_t pg_id) const;

  pg_store* get_pg(uint64_t pool_id, uint64_t pg_id, uint64_t store_epoch);
  pg_store* get_latest_pg(uint64_t pool_id, uint64_t pg_id);

  /**
   * Create a new PG instance with store_epoch = latest+1.
   * Callback: err=0, then get_latest_pg(pool,pg) is valid.
   */
  void create_pg(uint64_t pool_id, uint64_t pg_id, op_complete cb, void* arg);

  /**
   * Drop in-memory PG (all epochs for pool+pg). Does not free on-disk
   * segments (GC later). Future create_pg gets a higher epoch so old
   * records are ignored on recover.
   */
  void delete_pg(uint64_t pool_id, uint64_t pg_id, op_complete cb, void* arg);

  /**
   * Rebuild all PGs from device image (single scan, apply checkpoints).
   * Discovers (pool,pg,epoch) from records.
   */
  void recover(op_complete cb, void* arg);

private:
  using pg_key = std::tuple<uint64_t, uint64_t, uint64_t>; // pool, pg, epoch

  pg_store* ensure_pg(uint64_t pool_id, uint64_t pg_id, uint64_t epoch);
  void clear_all_pgs();

  struct recover_req;

  static void recover_read_done(void* arg, int err);
  void recover_process(recover_req* req);

  device_backend* backend_{nullptr};
  buffer_mode mode_{buffer_mode::heap};
  std::shared_ptr<segment_allocator> alloc_;
  std::map<pg_key, std::unique_ptr<pg_store>> pgs_;
  // (pool, pg) -> max epoch ever created in this process
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> latest_epoch_;
};

} // namespace fastblock::aos
