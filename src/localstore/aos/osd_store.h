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
#include "layout.h"
#include "kv_engine.h"
#include "log_store.h"
#include "pg_store.h"
#include "segment_allocator.h"
#include "superblock.h"
#include "types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace fastblock::aos {

/**
 * Multi-PG facade sharing one segment_allocator + device (design §4 / M2).
 *
 * Device layout (see layout.h):
 *   superblock A/B | manifest slots | data segments
 *
 * - format() writes empty superblock + empty manifest.
 * - create_pg / delete_pg update in-memory epoch table and persist_manifest.
 * - recover() loads superblock+manifest then single-pass scans the data region.
 *
 * SPDK: all methods are reactor-thread only; completions via callback.
 */
class osd_store {
public:
  using op_complete = device_backend::op_complete;

  /**
   * segment_size: 0 => single data segment over full data region.
   * Uses fixed metadata front (kMetaSize); backend capacity must be larger.
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

  uint64_t data_offset() const { return data_offset_; }
  uint64_t data_length() const { return data_length_; }
  uint64_t superblock_generation() const { return sb_generation_; }
  uint64_t manifest_generation() const { return manifest_generation_; }

  size_t pg_count() const { return pgs_.size(); }

  /**
   * Distinct (pool_id, pg_id) known after recover/manifest (latest_epoch > 0).
   * Used by OSD load to rebuild raft PGs without blobstore scan (design v2 P3).
   */
  std::vector<std::pair<uint64_t, uint64_t>> list_pg_ids() const;

  // Latest store_epoch for (pool, pg), or 0 if none.
  uint64_t latest_epoch(uint64_t pool_id, uint64_t pg_id) const;

  /**
   * Serving epoch for client reads (active generation). During install
   * ingest this stays on the pre-staging epoch until activate().
   * If never set, falls back to latest_epoch.
   */
  uint64_t serving_epoch(uint64_t pool_id, uint64_t pg_id) const;
  void set_serving_epoch(uint64_t pool_id, uint64_t pg_id, uint64_t epoch);
  void set_latest_epoch(uint64_t pool_id, uint64_t pg_id, uint64_t epoch);

  pg_store* get_pg(uint64_t pool_id, uint64_t pg_id, uint64_t store_epoch);
  pg_store* get_latest_pg(uint64_t pool_id, uint64_t pg_id);
  /** Active serving PG (install-aware). */
  pg_store* get_active_pg(uint64_t pool_id, uint64_t pg_id);

  /**
   * Raft log stream for PG (design v2 §6). Shares segment allocator with objects.
   * Creates on first use; recovered from raft_log_entry records on recover().
   */
  log_store* get_or_create_log(uint64_t pool_id, uint64_t pg_id,
                               uint64_t store_epoch);
  log_store* get_log(uint64_t pool_id, uint64_t pg_id, uint64_t store_epoch);
  /** Log for active/serving epoch of (pool,pg). */
  log_store* get_active_log(uint64_t pool_id, uint64_t pg_id);

  /** Optional shard-level KV for recover routing (not owned). */
  void set_kv_engine(kv_engine* kv) { kv_ = kv; }
  kv_engine* kv_engine_ptr() const { return kv_; }

  /** Drop one epoch instance from memory (install abort / post-activate). */
  void drop_pg_epoch(uint64_t pool_id, uint64_t pg_id, uint64_t epoch);

  /**
   * After recover: drop staging epochs that have data but no install_seal
   * (incomplete install snapshot). Keeps serving_epoch on sealed/active.
   * Returns number of epochs dropped.
   */
  uint32_t drop_unsealed_staging();

  /**
   * Write empty superblock A/B + empty manifest. Required once before first use
   * on a fresh device (or after wipe).
   */
  void format(op_complete cb, void* arg);

  /**
   * Persist epoch table + allocator open cursor into the next manifest slot
   * and bump superblock (dual-write).
   */
  void persist_manifest(op_complete cb, void* arg);

  /**
   * Create a new PG instance with store_epoch = latest+1, then persist_manifest.
   */
  void create_pg(uint64_t pool_id, uint64_t pg_id, op_complete cb, void* arg);

  /**
   * Drop in-memory PG instances. Keeps active_epoch in table so recreate
   * gets a higher epoch. Persists manifest.
   */
  void delete_pg(uint64_t pool_id, uint64_t pg_id, op_complete cb, void* arg);

  /**
   * Load superblock + latest manifest, then rebuild PGs from data region.
   */
  void recover(op_complete cb, void* arg);

  /**
   * One GC pass (design §11 skeleton): free sealed segments that have no live
   * head extents from any in-memory PG. Does not rewrite data or compact.
   * Returns number of segments freed. Optional persist_manifest after success.
   */
  uint32_t gc_once(bool persist = true);

  /**
   * One GC rewrite pass (design §11.4): pick the sealed segment with the
   * lowest live-byte ratio (among those with live heads), rewrite up to
   * max_extents head extents out via pg_store::rewrite_extent, then free
   * fully dead sealed segments (pins may block free — correct).
   * Returns number of extents rewritten. Optional persist after free.
   * Mem backend completes inline; SPDK path should be driven on reactor.
   *
   * max_extents: rate-limit (0 => rewrite all live heads in victim).
   */
  uint32_t gc_rewrite_once(bool persist = true, uint32_t max_extents = 0);

  /**
   * Raft/OSD STM paths against the serving (active) generation (M4).
   * raft_write_index is the raft log index (idempotency); image_snap_seq is
   * image timeline (0 for plain object writes).
   */
  void apply(uint64_t pool_id, uint64_t pg_id, const std::string& object_name,
             uint64_t offset, const void* data, uint64_t len,
             uint64_t raft_write_index, uint64_t image_snap_seq, op_complete cb,
             void* arg);

  void read(uint64_t pool_id, uint64_t pg_id, const std::string& object_name,
            uint64_t offset, void* buf, uint64_t len, op_complete cb, void* arg);

  void remove(uint64_t pool_id, uint64_t pg_id, const std::string& object_name,
              uint64_t raft_write_index, uint64_t image_snap_seq, op_complete cb,
              void* arg);

private:
  using pg_key = std::tuple<uint64_t, uint64_t, uint64_t>; // pool, pg, epoch

  pg_store* ensure_pg(uint64_t pool_id, uint64_t pg_id, uint64_t epoch);
  void clear_all_pgs();
  manifest build_manifest() const;
  void apply_manifest_epochs(const manifest& m);

  struct meta_req;
  struct recover_req;

  void format_continue(meta_req* req);
  void persist_continue(meta_req* req);
  void recover_after_meta(recover_req* req);
  void recover_process(recover_req* req);

  device_backend* backend_{nullptr};
  buffer_mode mode_{buffer_mode::heap};
  uint64_t configured_segment_size_{0};
  uint64_t data_offset_{kMetaSize};
  uint64_t data_length_{0};
  uint64_t sb_generation_{0};
  uint64_t manifest_generation_{0};
  std::shared_ptr<segment_allocator> alloc_;
  std::map<pg_key, std::unique_ptr<pg_store>> pgs_;
  std::map<pg_key, std::unique_ptr<log_store>> logs_;
  kv_engine* kv_{nullptr}; // not owned
  // (pool, pg) -> max epoch ever created (from manifest + create_pg)
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> latest_epoch_;
  // (pool, pg) -> serving epoch for reads (0 => use latest_epoch)
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> serving_epoch_;
};

} // namespace fastblock::aos
