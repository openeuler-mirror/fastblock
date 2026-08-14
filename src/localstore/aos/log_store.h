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
#include "segment.h"
#include "segment_allocator.h"
#include "types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fastblock::aos {

/**
 * One raft log stream for a PG (design v2 §6).
 *
 * Replaces disk_log+rolling_blob for the AOS platform path.
 * Does NOT use object 4K/4MiB semantics — entries are variable-size records.
 *
 * Payload layout (little-endian):
 *   u64 term, u64 index, u64 type, u32 meta_len, meta bytes, data bytes
 * Header fields: pool/pg/epoch; raft_apply_index=index; image_snap_seq=term.
 */
struct log_entry {
  uint64_t term{0};
  uint64_t index{0};
  uint64_t type{0};
  std::string meta;
  std::string data;
};

class log_store {
public:
  using op_complete = device_backend::op_complete;
  using read_complete =
      std::function<void(void* arg, std::vector<log_entry> entries, int err)>;

  log_store(device_backend* backend, pg_id id,
            std::shared_ptr<segment_allocator> alloc,
            buffer_mode mode = buffer_mode::heap);

  log_store(const log_store&) = delete;
  log_store& operator=(const log_store&) = delete;

  const pg_id& id() const { return id_; }
  uint64_t base_index() const { return base_index_; }
  uint64_t last_index() const { return last_index_; }
  size_t entry_count() const { return index_map_.size(); }

  /**
   * Append one entry (must be last_index+1, or first entry if empty).
   * Completes after write_at + flush. Still writes type=14.
   */
  void append(const log_entry& entry, op_complete cb, void* arg);

  /** Append entries in order (each index consecutive). */
  void append_batch(std::vector<log_entry> entries, op_complete cb, void* arg);

  /**
   * Register an already-written record as last_index+1 (or first if empty).
   * phys is the record start, never the payload start; length is the aligned
   * whole record. Same index + same record_start is idempotent (returns 0).
   */
  int register_index(uint64_t index, uint64_t term, uint64_t phys,
                     uint64_t length, record_type rec_type);

  /**
   * Point an existing index at a rewritten record start (GC). Missing index
   * is a no-op.
   */
  void update_phys(uint64_t index, uint64_t new_record_start, uint64_t new_len);

  using name_lookup_fn =
      std::function<int(uint64_t object_id, std::string* name)>;
  void set_name_lookup(name_lookup_fn fn) { name_lookup_ = std::move(fn); }

  /**
   * Inclusive range [start, end]. Missing index / missing name / non-4KiB
   * data-as-log payload → error and empty entries. Never returns empty meta
   * for data / tombstone / gc_rewrite.
   */
  void read(uint64_t start_index, uint64_t end_index, read_complete cb,
            void* arg);

  uint64_t get_term(uint64_t index) const;
  bool contains(uint64_t index) const;

  /**
   * After install-snapshot: make last_index == index with term on disk.
   * Empty log: append a snapshot-base entry at index (need not start at 1).
   * Already at/above index: no-op success (idempotent / catchup already ahead).
   * Completes after the marker is durable (or immediately if no I/O).
   */
  void install_snapshot_base(uint64_t index, uint64_t term, op_complete cb,
                             void* arg);

  /**
   * Drop index map entries with index <= trim_index (inclusive).
   * Physical reclaim is deferred to GC (segments with no live log refs).
   */
  void trim_to(uint64_t trim_index);

  /**
   * Drop entries with index >= from_index (raft truncate suffix).
   * Completes synchronously for map; cb for API symmetry.
   */
  void truncate_from(uint64_t from_index, op_complete cb, void* arg);

  /** Apply one decoded raft_log_entry record (recover path). */
  int apply_record(const data_record& rec, uint64_t phys_offset);

  /**
   * Append a durable log_checkpoint of the live index_map and flush.
   * Recover can load this instead of replaying prior raft_log_entry records.
   */
  void checkpoint(op_complete cb, void* arg);
  int apply_checkpoint_record(const data_record& rec);
  uint64_t last_checkpoint_phys() const { return last_checkpoint_phys_; }

  /** Sync rebuild from a contiguous image (mem tests). */
  uint64_t rebuild_from_image(const uint8_t* image, size_t len);

  /** Visit live physical ranges for GC marking. */
  void for_each_live_extent(
      const std::function<void(uint64_t phys_offset, uint64_t length)>& fn)
      const;

  static std::string encode_payload(const log_entry& e);
  static bool decode_payload(const std::string& payload, log_entry* out);

private:
  struct idx_pos {
    uint64_t phys_offset{0}; // record start
    uint64_t length{0};      // on-disk aligned whole record
    uint64_t term{0};
    record_type rec_type{record_type::raft_log_entry};
  };

  int decode_as_log_entry(const data_record& rec, uint64_t index,
                          const idx_pos& pos, log_entry* out) const;

  void* alloc_buf(uint64_t n);
  void free_buf(void* p);

  device_backend* backend_{nullptr};
  pg_id id_{};
  buffer_mode mode_{buffer_mode::heap};
  std::shared_ptr<segment_allocator> alloc_;
  segment writer_;

  std::map<uint64_t, idx_pos> index_map_;
  uint64_t base_index_{0}; // lowest present, or 0 if empty
  uint64_t last_index_{0}; // highest present, or 0 if empty
  uint64_t last_checkpoint_phys_{0};
  name_lookup_fn name_lookup_;

  static std::string encode_checkpoint_payload(
      const std::map<uint64_t, idx_pos>& idx, uint64_t base, uint64_t last);
  static bool decode_checkpoint_payload(const std::string& p,
                                        std::map<uint64_t, idx_pos>* idx,
                                        uint64_t* base, uint64_t* last);
};

} // namespace fastblock::aos
