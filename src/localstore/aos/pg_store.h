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
#include <tuple>
#include <unordered_map>
#include <vector>

namespace fastblock::aos {

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
 * Checkpoint: append checkpoint_marker with ObjectMap payload (§12.1).
 * M3: raft snapshot pin is O(1); snapshot read uses per-object version log.
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
  // Absolute start of last durable checkpoint record (0 if none).
  uint64_t last_checkpoint_phys() const { return last_checkpoint_phys_; }

  /**
   * Append a data record, flush, then publish head extent map.
   * data is copied into an internal DMA/aligned I/O buffer before submit.
   */
  void write(const std::string& object_name, uint64_t offset, const void* data,
             uint64_t len, uint64_t raft_write_index, uint64_t image_snap_seq,
             op_complete cb, void* arg);

  /**
   * Read object view. Missing ranges are zero-filled.
   * raft_read_index == 0 && image_read_seq == 0: head.
   * raft_read_index > 0: raft snapshot view at that index (M3).
   * image_read_seq > 0: image snapshot view (ops with image_snap_seq <= seq).
   * Both non-zero: intersection (ops satisfying both filters).
   * buf is caller-owned; may be non-DMA (staged through internal buffer).
   */
  void read(const std::string& object_name, uint64_t offset, void* buf,
            uint64_t len, op_complete cb, void* arg) {
    read(object_name, offset, buf, len, /*raft_read_index=*/0,
         /*image_read_seq=*/0, std::move(cb), arg);
  }
  void read(const std::string& object_name, uint64_t offset, void* buf,
            uint64_t len, uint64_t raft_read_index, op_complete cb, void* arg) {
    read(object_name, offset, buf, len, raft_read_index, /*image_read_seq=*/0,
         std::move(cb), arg);
  }
  void read(const std::string& object_name, uint64_t offset, void* buf,
            uint64_t len, uint64_t raft_read_index, uint64_t image_read_seq,
            op_complete cb, void* arg);

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
   * GC rewrite one head extent (design §11.4): read old payload, append
   * gc_rewrite record to a new physical location, update head + version log
   * phys pointers only (does not advance raft/image version).
   * object_name + logical (offset,len) must match a live head extent.
   */
  void rewrite_extent(const std::string& object_name, uint64_t logical_offset,
                      uint64_t len, op_complete cb, void* arg);

  /**
   * O(1) raft snapshot pin (design M3): append pin record + flush, pin index.
   * Does not scan objects. pin_count(index) increments (idempotent re-pin OK).
   */
  void create_raft_snapshot(uint64_t raft_apply_index, uint64_t raft_term,
                            op_complete cb, void* arg);

  /**
   * Release one pin on raft_apply_index. When pin_count hits 0, GC may reclaim
   * extents only needed by that snapshot.
   */
  void release_raft_snapshot(uint64_t raft_apply_index, op_complete cb,
                             void* arg);

  uint32_t snapshot_pin_count(uint64_t raft_apply_index) const;
  uint32_t total_snapshot_pins() const { return total_snapshot_pins_; }

  /**
   * O(1) image snapshot pin (design §10.2 / M3): append pin + flush.
   * Control plane / clone may re-pin; pin_count increments.
   */
  void create_image_snapshot(uint64_t image_snap_seq, op_complete cb,
                             void* arg);

  /**
   * Release one pin on image_snap_seq. When count hits 0, GC may reclaim
   * extents only needed by that image snapshot (after all pins gone).
   */
  void release_image_snapshot(uint64_t image_snap_seq, op_complete cb,
                              void* arg);

  uint32_t image_snapshot_pin_count(uint64_t image_snap_seq) const;
  uint32_t total_image_snapshot_pins() const {
    return total_image_snapshot_pins_;
  }

  /**
   * Clone pin (design P5 / §11): re-pin image_snap_seq so a new image/clone
   * reader can hold the parent view. Equivalent to create_image_snapshot.
   */
  void clone_image(uint64_t image_snap_seq, op_complete cb, void* arg) {
    create_image_snapshot(image_snap_seq, std::move(cb), arg);
  }

  /**
   * Flatten (design P5): materialize parent image view into new head data
   * written under new_image_seq (and raft_write_index). After success the
   * parent pin may be released by the control plane.
   *
   * - parent_image_seq: view to copy (must be > 0)
   * - new_image_seq: image timeline for rewritten extents (must be >
   *   parent_image_seq)
   * - raft_write_index: base raft index; each rewritten extent uses
   *   raft_write_index + n (n = 0..)
   */
  void flatten_image(uint64_t parent_image_seq, uint64_t new_image_seq,
                     uint64_t raft_write_index, op_complete cb, void* arg);

  /**
   * Flatten a single object from parent image view (used by flatten_image).
   */
  void flatten_object(const std::string& object_name,
                      uint64_t parent_image_seq, uint64_t new_image_seq,
                      uint64_t raft_write_index, op_complete cb, void* arg);

  /**
   * Durable install-snapshot markers on this PG epoch (M5 follower staging).
   * seal: staging ready; activate: generation promoted to active.
   */
  void mark_install_seal(uint64_t snapshot_index, uint64_t snapshot_term,
                         op_complete cb, void* arg);
  void mark_install_activate(uint64_t snapshot_index, op_complete cb,
                             void* arg);

  bool install_sealed() const { return install_sealed_; }
  bool install_activated() const { return install_activated_; }
  uint64_t install_snapshot_index() const { return install_snapshot_index_; }
  uint64_t install_snapshot_term() const { return install_snapshot_term_; }

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
   * Visit every live physical extent. Head always; if any raft/image snapshot
   * pin is held, also all historical put extents (conservative pin for GC).
   */
  void for_each_live_extent(
      const std::function<void(uint64_t phys_offset, uint64_t length)>& fn)
      const;

  /**
   * Collect live head extents whose physical range intersects
   * [phys_begin, phys_end). Output: (object_name, logical_offset, len).
   */
  void collect_head_extents_in_range(
      uint64_t phys_begin, uint64_t phys_end,
      std::vector<std::tuple<std::string, uint64_t, uint64_t>>* out) const;

  /**
   * Visit each non-empty snapshot object view (for install-snapshot streaming).
   * Uses view_at(raft_read_index, image_read_seq); skips deleted/empty names.
   */
  void for_each_snapshot_object(
      uint64_t raft_read_index, uint64_t image_read_seq,
      const std::function<void(const std::string& name,
                               const extent_map& view)>& fn) const;

  /**
   * Live (non-deleted) object names in this PG (for install-snapshot leader).
   */
  void list_live_object_names(std::vector<std::string>* out) const;

  bool object_exists(const std::string& name) const;

private:
  // In-memory version log for snapshot reads (M3).
  struct version_op {
    enum class kind : uint8_t { put = 1, discard = 2, remove = 3 };
    kind k{kind::put};
    extent e{};
    uint64_t offset{0};
    uint64_t len{0};
    uint64_t raft_index{0};
    uint64_t image_seq{0};
  };

  struct object_state {
    std::string name;
    extent_map head;
    bool deleted{false};
    std::vector<version_op> versions;
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
  struct snap_req;
  struct rewrite_req;
  struct recover_req;
  struct flatten_obj_req;
  struct flatten_img_req;

  object_state* find_object(const std::string& name);
  object_state* find_or_alloc_object(const std::string& name, bool* is_new);
  uint64_t alloc_object_id();

  checkpoint_snapshot build_snapshot(uint64_t generation) const;
  void clear_maps();

  // Build extent_map for snapshot view. Both 0 => head. deleted => empty.
  extent_map view_at(const object_state& obj, uint64_t raft_read_index,
                     uint64_t image_read_seq = 0) const;
  void record_version_put(object_state* obj, const extent& e);
  void record_version_discard(object_state* obj, uint64_t off, uint64_t len,
                              uint64_t raft_index, uint64_t image_seq);
  void record_version_remove(object_state* obj, uint64_t raft_index,
                             uint64_t image_seq);

  void pin_snapshot_index(std::map<uint64_t, uint32_t>* pins,
                          uint32_t* total, uint64_t index);
  void unpin_snapshot_index(std::map<uint64_t, uint32_t>* pins,
                            uint32_t* total, uint64_t index);

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

  static void flatten_obj_step(flatten_obj_req* req);
  static void flatten_obj_after_read(void* arg, int err);
  static void flatten_obj_after_write(void* arg, int err);
  static void flatten_img_step(flatten_img_req* req);

  static void remove_after_tomb(void* arg, uint64_t phys, int err);
  static void remove_after_flush(void* arg, int err);

  static void discard_after_tomb(void* arg, uint64_t phys, int err);
  static void discard_after_flush(void* arg, int err);

  static void checkpoint_after_append(void* arg, uint64_t phys, int err);
  static void checkpoint_after_flush(void* arg, int err);

  static void snap_after_append(void* arg, uint64_t phys, int err);
  static void snap_after_flush(void* arg, int err);

  static void rewrite_after_read(void* arg, int err);
  static void rewrite_after_append(void* arg, uint64_t phys, int err);
  static void rewrite_after_flush(void* arg, int err);
  void apply_rewrite_phys(uint64_t object_id, uint64_t logical_offset,
                          uint64_t len, uint64_t new_data_phys);

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
  uint64_t last_checkpoint_phys_{0};

  std::unordered_map<std::string, uint64_t> name_to_id_;
  std::unordered_map<uint64_t, object_state> objects_;

  // raft_apply_index -> pin count
  std::map<uint64_t, uint32_t> snapshot_pins_;
  uint32_t total_snapshot_pins_{0};
  // image_snap_seq -> pin count (clone / image snapshot reader)
  std::map<uint64_t, uint32_t> image_snapshot_pins_;
  uint32_t total_image_snapshot_pins_{0};

  // Install-snapshot staging markers (rebuilt on recover via apply_record).
  bool install_sealed_{false};
  bool install_activated_{false};
  uint64_t install_snapshot_index_{0};
  uint64_t install_snapshot_term_{0};

  void* scratch_ptr_{nullptr};
  uint64_t scratch_cap_{0};

  bool busy_{false};
  std::deque<std::function<void()>> queue_;
};

} // namespace fastblock::aos
