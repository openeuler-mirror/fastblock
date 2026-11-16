/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "segment_allocator.h"

#include <cerrno>

namespace fastblock::aos {

segment_allocator::segment_allocator(uint64_t capacity, uint64_t segment_size,
                                     uint32_t block_size)
    : capacity_(capacity), segment_size_(segment_size),
      block_size_(block_size) {
  if (block_size_ == 0) {
    block_size_ = 4096;
  }
  if (segment_size_ == 0 || segment_size_ > capacity_) {
    segment_size_ = capacity_ == 0 ? block_size_ : capacity_;
  }
  // Align down segment_size to block.
  segment_size_ = (segment_size_ / block_size_) * block_size_;
  if (segment_size_ == 0) {
    segment_size_ = block_size_;
  }
  capacity_ = (capacity_ / block_size_) * block_size_;
  const uint32_t n = segment_size_ == 0
                         ? 0
                         : static_cast<uint32_t>(capacity_ / segment_size_);
  states_.assign(n, segment_state::free);
}

segment_state segment_allocator::state(uint32_t id) const {
  if (id >= states_.size()) {
    return segment_state::bad;
  }
  return states_[id];
}

uint32_t segment_allocator::free_count() const {
  uint32_t n = 0;
  for (auto s : states_) {
    if (s == segment_state::free) {
      ++n;
    }
  }
  return n;
}

uint32_t segment_allocator::sealed_count() const {
  uint32_t n = 0;
  for (auto s : states_) {
    if (s == segment_state::sealed) {
      ++n;
    }
  }
  return n;
}

void segment_allocator::reset() {
  for (auto& s : states_) {
    s = segment_state::free;
  }
  open_id_ = kInvalidSeg;
  open_cursor_ = 0;
}

void segment_allocator::seal_open() {
  if (open_id_ == kInvalidSeg) {
    return;
  }
  states_[open_id_] = segment_state::sealed;
  open_id_ = kInvalidSeg;
  open_cursor_ = 0;
}

int segment_allocator::open_free_unlocked() {
  for (uint32_t i = 0; i < states_.size(); ++i) {
    if (states_[i] == segment_state::free) {
      states_[i] = segment_state::open;
      open_id_ = i;
      open_cursor_ = 0;
      return 0;
    }
  }
  return -ENOSPC;
}

int segment_allocator::ensure_open() {
  if (open_id_ != kInvalidSeg) {
    return 0;
  }
  return open_free_unlocked();
}

int segment_allocator::reserve(uint64_t nbytes, uint64_t* phys_offset,
                               uint32_t* seg_id) {
  if (phys_offset == nullptr || nbytes == 0 ||
      (nbytes % block_size_) != 0) {
    return -EINVAL;
  }
  if (nbytes > segment_size_) {
    return -EINVAL; // single record cannot span segments
  }

  if (ensure_open() != 0) {
    return -ENOSPC;
  }

  if (open_cursor_ + nbytes > segment_size_) {
    seal_open();
    if (open_free_unlocked() != 0) {
      return -ENOSPC;
    }
  }

  *phys_offset = segment_base(open_id_) + open_cursor_;
  if (seg_id) {
    *seg_id = open_id_;
  }
  open_cursor_ += nbytes;
  return 0;
}

void segment_allocator::note_written(uint64_t phys_offset, uint64_t len) {
  if (len == 0 || segment_size_ == 0 || states_.empty()) {
    return;
  }
  const uint64_t end = phys_offset + len;
  const uint32_t first =
      static_cast<uint32_t>(phys_offset / segment_size_);
  const uint32_t last = end == 0
                            ? first
                            : static_cast<uint32_t>((end - 1) / segment_size_);
  for (uint32_t id = first; id <= last && id < states_.size(); ++id) {
    if (states_[id] == segment_state::free) {
      states_[id] = segment_state::sealed;
    }
  }
  // Highest touched segment becomes open with cursor at local end.
  if (last < states_.size()) {
    // Sealed all lower that we marked; reopen last as open.
    for (uint32_t id = first; id < last && id < states_.size(); ++id) {
      if (states_[id] != segment_state::bad) {
        states_[id] = segment_state::sealed;
      }
    }
    states_[last] = segment_state::open;
    open_id_ = last;
    const uint64_t base = segment_base(last);
    open_cursor_ = end > base ? end - base : 0;
    if (open_cursor_ > segment_size_) {
      open_cursor_ = segment_size_;
      states_[last] = segment_state::sealed;
      open_id_ = kInvalidSeg;
      open_cursor_ = 0;
    }
  }
}

void segment_allocator::set_open_cursor_abs(uint64_t abs_end) {
  if (open_id_ == kInvalidSeg || segment_size_ == 0) {
    // Try map abs_end into a segment and open it.
    if (abs_end == 0 || states_.empty()) {
      return;
    }
    const uint32_t id =
        static_cast<uint32_t>((abs_end - 1) / segment_size_);
    if (id >= states_.size()) {
      return;
    }
    for (uint32_t i = 0; i < id; ++i) {
      if (states_[i] == segment_state::free) {
        states_[i] = segment_state::sealed;
      }
    }
    states_[id] = segment_state::open;
    open_id_ = id;
    open_cursor_ = abs_end - segment_base(id);
    return;
  }
  const uint64_t base = segment_base(open_id_);
  if (abs_end <= base) {
    open_cursor_ = 0;
    return;
  }
  open_cursor_ = abs_end - base;
  if (open_cursor_ > segment_size_) {
    open_cursor_ = segment_size_;
  }
}

} // namespace fastblock::aos
