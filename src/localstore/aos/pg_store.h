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

#include "checkpoint.h"
#include "device_backend.h"
#include "extent_map.h"
#include "segment.h"
#include "segment_allocator.h"
#include "types.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastblock::aos {

enum class buffer_mode {
  // posix/aligned heap — unit tests with mem_device_backend
  heap = 0,
  // spdk_dma_zmalloc — production / spdk_app (env must be initialized)
  spdk_dma = 1,
};

/**
 * Single-PG append-only object store (M1).
 *
 * SPDK rules:
 * - All public methods are asynchronous (callback completion).
 * - Must be driven on one reactor thread (no internal locking).
 * - Uses device_backend for I/O (spdk_bdev_backend in production).
 * - Durability: user write/remove complete only after backend flush (§8).
 * - Idempotency key: (object_id, offset, len, raft_apply_index).
 *
 * M1: head only, no image pin / GC / multi-segment.
 * Checkpoint: append checkpoint_marker with ObjectMap payload (§12.1);
 * recovery loads latest complete checkpoint then replays the tail.
 */
class pg_store {
public:
  using op_complete = device_backend::op_complete;

  /**
   * backend lifetime must exceed pg_store.
   * segment_size: fixed segment bytes; 0 means single segment spanning
   * the whole backend capacity (compatible with linear M1 layout).
   */
  pg_store(device_backend* backend, pg_id id,
           buffer_mode mode = buffer_mode::heap, uint64_t segment_size = 0);

  /**
   * Share an existing segment_allocator (M2 multi-PG). Caller keeps the
   * allocator alive; multiple pg_store instances may share one device.
   */
  pg_store(device_backend* backend, pg_id id,
           std::shared_ptr<segment_allocator> alloc,
           buffer_mode mode = buffer_mode::heap);
  ~pg_store();

  pg_store(const pg_store&) = delete;
  pg_store& operator=(const pg_store&) = delete;

  const pg_id& id() const { return id_; }
  device_backend* backend() const { return backend_; }
  buffer_mode mode() const { return mode_; }
  segment_allocator* allocator() { return alloc_.get(); }
  const segment_allocator* allocator() const { return alloc_.get(); }
  std::shared_ptr<segment_allocator> shared_allocator() const { return alloc_; }
  uint64_t checkpoint_generation() const { return checkpoint_generation_; }
  // Phys offset of last durable checkpoint record end (0 if none).
  uint64_t last_checkpoint_end() const { return last_checkpoint_end_; }

  /**
   * Append a data record, flush, then publish head extent map.
   * data is copied into an internal DMA/aligned I/O buffer before submit.
   */
  void write(const std::string& object_name, uint64_t offset, const void* data,
             uint64_t len, uint64_t raft_apply_index, uint64_t image_snap_seq,
             op_complete cb, void* arg);

  /**
   * Read head view. Missing ranges are zero-filled.
   * buf is caller-owned; may be non-DMA (staged through internal buffer).
   */
  void read(const std::string& object_name, uint64_t offset, void* buf,
            uint64_t len, op_complete cb, void* arg);

  void remove(const std::string& object_name, uint64_t raft_apply_index,
              uint64_t image_snap_seq, op_complete cb, void* arg);

  /**
   * Range discard (design §7.3): append range_tombstone, flush, then punch
   * hole in head extent map. Discarded range reads as zero. 4K-aligned.
   * Idempotency key: (object, offset, len, raft_apply_index).
   */
  void discard(const std::string& object_name, uint64_t offset, uint64_t len,
               uint64_t raft_apply_index, uint64_t image_snap_seq,
               op_complete cb, void* arg);

  void flush(op_complete cb, void* arg);

  /**
   * Append a durable checkpoint_marker (ObjectMap snapshot) and flush.
   * generation bumps on success. Half-written markers are ignored on recover
   * (checksum fail → torn stop; prior complete checkpoint remains valid).
   */
  void checkpoint(op_complete cb, void* arg);

  /**
   * Async recovery (design §12.1):
   * 1) scan for latest complete checkpoint_marker
   * 2) load that snapshot
   * 3) replay records after it to last good tail
   * If no checkpoint: full replay from offset 0.
   */
  void recover(op_complete cb, void* arg);

  // Apply one decoded record into memory maps (no I/O). Used by recover.
  int apply_record(const data_record& rec, uint64_t phys_offset);

  // Load ObjectMap from a decoded checkpoint payload (clears prior maps).
  int apply_checkpoint_snapshot(const checkpoint_snapshot& snap,
                                uint64_t checkpoint_end_phys);

  // Sync rebuild from a contiguous image (mem tests / inject).
  uint64_t rebuild_from_image(const uint8_t* image, size_t len);

  uint64_t object_count() const { return objects_.size(); }
  bool busy() const { return busy_; }

  /**
   * Visit every live head extent (not deleted objects). Used by GC to decide
   * whether a physical segment still has references.
   * fn(phys_offset, length) — absolute device offsets.
   */
  void for_each_live_extent(
      const std::function<void(uint64_t phys_offset, uint64_t length)>& fn)
      const;

private:
  struct object_state {
    std::string name;
    extent_map head;
    bool deleted{false};
    // key = (offset, len) -> last applied raft_apply_index
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> applied_writes;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> applied_discards;
    uint64_t delete_raft_index{0};
  };

  struct write_req;
  struct read_req;
  struct remove_req;
  struct discard_req;
  struct checkpoint_req;
  struct recover_req;

  object_state* find_object(const std::string& name);
  object_state* find_or_alloc_object(const std::string& name, bool* is_new);
  uint64_t alloc_object_id();

  checkpoint_snapshot build_snapshot(uint64_t generation) const;
  void clear_maps();

  void finish_op(op_complete cb, void* arg, int err);
  void enqueue_or_run(std::function<void()> op);
  void pump_queue();

  void* alloc_buf(size_t size, size_t align);
  void free_buf(void* p);
  void ensure_scratch(uint64_t need);
  void* scratch_ptr() { return scratch_ptr_; }
  uint64_t scratch_cap() const { return scratch_cap_; }

  static void write_after_name(void* arg, uint64_t phys, int err);
  static void write_after_data(void* arg, uint64_t phys, int err);
  static void write_after_flush(void* arg, int err);

  static void remove_after_tomb(void* arg, uint64_t phys, int err);
  static void remove_after_flush(void* arg, int err);

  static void discard_after_tomb(void* arg, uint64_t phys, int err);
  static void discard_after_flush(void* arg, int err);

  static void checkpoint_after_append(void* arg, uint64_t phys, int err);
  static void checkpoint_after_flush(void* arg, int err);

  static void read_extent_done(void* arg, int err);

  static void recover_read_done(void* arg, int err);
  void recover_process_buffer(recover_req* req);
  void recover_start_replay(recover_req* req, uint64_t from_offset);

  device_backend* backend_{nullptr};
  std::shared_ptr<segment_allocator> alloc_;
  segment seg_;
  pg_id id_{};
  buffer_mode mode_{buffer_mode::heap};
  uint64_t next_object_id_{1};
  uint64_t checkpoint_generation_{0};
  uint64_t last_checkpoint_end_{0};

  std::unordered_map<std::string, uint64_t> name_to_id_;
  std::unordered_map<uint64_t, object_state> objects_;

  void* scratch_ptr_{nullptr};
  uint64_t scratch_cap_{0};

  bool busy_{false};
  std::deque<std::function<void()>> queue_;
};

} // namespace fastblock::aos
