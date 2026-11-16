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

#include <cstdint>
#include <limits>
#include <vector>

namespace fastblock::aos {

enum class segment_state : uint8_t {
  free = 0,
  open = 1,
  sealed = 2,
  gc_candidate = 3,
  cleaning = 4,
  bad = 5,
};

/**
 * Fixed-size segment allocator (design §4.4), M1 single-writer.
 *
 * - Does not perform I/O; only tracks free/open/sealed and open cursor.
 * - Guarantees: reserved ranges never overlap; no double-alloc of free segs.
 * - segment_size == capacity => single segment (legacy linear layout).
 * - When open segment cannot fit nbytes, it is sealed and next free opened.
 */
class segment_allocator {
public:
  static constexpr uint32_t kInvalidSeg =
      std::numeric_limits<uint32_t>::max();

  /**
   * region_base: absolute byte offset of segment 0 on the device.
   * capacity: usable data bytes from region_base (not including metadata).
   * segment_size: fixed segment bytes; if 0 or > capacity, clamped to capacity.
   * If capacity % segment_size != 0, trailing partial region is unused.
   */
  segment_allocator(uint64_t region_base, uint64_t capacity,
                    uint64_t segment_size, uint32_t block_size);

  // region_base = 0 (legacy single-region layout).
  segment_allocator(uint64_t capacity, uint64_t segment_size,
                    uint32_t block_size)
      : segment_allocator(0, capacity, segment_size, block_size) {}

  uint64_t region_base() const { return region_base_; }
  uint64_t capacity() const { return capacity_; }
  uint64_t segment_size() const { return segment_size_; }
  uint32_t block_size() const { return block_size_; }
  uint32_t segment_count() const {
    return static_cast<uint32_t>(states_.size());
  }
  uint32_t open_segment() const { return open_id_; }
  uint64_t open_cursor() const { return open_cursor_; }
  segment_state state(uint32_t id) const;
  uint32_t free_count() const;
  uint32_t sealed_count() const;

  // Absolute device offset of segment id.
  uint64_t segment_base(uint32_t id) const {
    return region_base_ + static_cast<uint64_t>(id) * segment_size_;
  }

  // Bytes left in the open segment (0 if none open).
  uint64_t remaining_in_open() const;

  // Open first free segment if none open. Returns 0 or -ENOSPC.
  int ensure_open();

  /**
   * Reserve nbytes (block-aligned) in the open segment, sealing and opening
   * a new free segment if needed. On success *phys_offset is absolute.
   * Advances open_cursor immediately (M1 single inflight writer).
   * Returns 0, -EINVAL, or -ENOSPC.
   */
  int reserve(uint64_t nbytes, uint64_t* phys_offset, uint32_t* seg_id = nullptr);

  // True if reserving nbytes would seal the current open segment first.
  bool would_roll(uint64_t nbytes) const;

  // Seal current open segment (if any). Idempotent.
  void seal_open();

  /**
   * Return a sealed segment to free for reuse (GC).
   * Fails with -EBUSY if open/not sealed, -EINVAL if bad id.
   * Does not wipe device contents; callers must ensure no live refs.
   */
  int free_sealed(uint32_t id);

  /**
   * Recover helper: note that [phys, phys+len) contains valid data.
   * Marks covering segments sealed (or open for the highest one) and
   * bumps open_cursor if in the open segment.
   */
  void note_written(uint64_t phys_offset, uint64_t len);

  // Reset all to free (before recover rebuild).
  void reset();

  // After recover: set open segment cursor to end of last good record in it.
  void set_open_cursor_abs(uint64_t abs_end);

  // Persist / restore segment states (one byte per segment).
  void export_states(std::vector<uint8_t>* out) const;
  // Returns false if n does not match segment_count.
  bool import_states(const uint8_t* data, uint32_t n, uint32_t open_id,
                     uint64_t cursor);

  uint32_t segment_id_of(uint64_t abs_off) const;
  bool is_free(uint32_t id) const {
    return id < states_.size() && states_[id] == segment_state::free;
  }

private:
  int open_free_unlocked();

  uint64_t region_base_{0};
  uint64_t capacity_{0};
  uint64_t segment_size_{0};
  uint32_t block_size_{0};
  std::vector<segment_state> states_;
  uint32_t open_id_{kInvalidSeg};
  uint64_t open_cursor_{0};
};

} // namespace fastblock::aos
