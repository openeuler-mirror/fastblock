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
#include "record.h"
#include "segment_allocator.h"
#include "types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace fastblock::aos {

/**
 * Append-only records via device_backend + segment_allocator (SPDK async).
 *
 * Responsibilities:
 * - encode records, pad to block_size
 * - reserve space from allocator (may seal/open segments)
 * - write_at reserved phys offset
 * - expose phys_offset of record start for extent_map
 *
 * Does NOT block. All methods complete via callback on the SPDK thread.
 */
class segment {
public:
  using op_complete = device_backend::op_complete;
  using append_record_complete =
      std::function<void(void* arg, uint64_t phys_offset, int err)>;

  segment(device_backend* backend, segment_allocator* alloc,
          buffer_mode mode = buffer_mode::heap)
      : backend_(backend), alloc_(alloc), mode_(mode) {}

  device_backend* backend() const { return backend_; }
  segment_allocator* allocator() const { return alloc_; }
  void set_allocator(segment_allocator* alloc) { alloc_ = alloc; }

  /**
   * Encode rec, pad to block_size, reserve + write_at.
   * On success: phys_offset is byte offset of the start of the on-disk
   * record (including header) within the backend region.
   *
   * dma_buf is only a size hint (must hold the aligned record). I/O uses an
   * owned copy so callers may reuse their scratch after this call returns.
   */
  void append_record(const data_record& rec, void* dma_buf, uint64_t dma_cap,
                     append_record_complete cb, void* arg);

  void flush(op_complete cb, void* arg) {
    backend_->flush(std::move(cb), arg);
  }

  // Round up nbytes to backend block size.
  uint64_t align_up(uint64_t nbytes) const {
    const uint32_t bs = backend_->block_size();
    return (nbytes + bs - 1) / bs * bs;
  }

private:
  device_backend* backend_{nullptr};
  segment_allocator* alloc_{nullptr};
  buffer_mode mode_{buffer_mode::heap};
};

} // namespace fastblock::aos
