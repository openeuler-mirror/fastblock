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

#include <cstdint>
#include <functional>
#include <vector>

namespace fastblock::aos {

/**
 * Append-only segment on top of device_backend (SPDK async).
 *
 * Responsibilities:
 * - encode records and append via backend
 * - pad to block_size for bdev alignment
 * - expose phys_offset of payload for extent_map
 *
 * Does NOT block. All methods complete via callback on the SPDK thread.
 */
class segment {
public:
  using op_complete = device_backend::op_complete;
  using append_record_complete =
      std::function<void(void* arg, uint64_t phys_offset, int err)>;

  explicit segment(device_backend* backend) : backend_(backend) {}

  device_backend* backend() const { return backend_; }

  /**
   * Encode rec, pad to block_size, append to device.
   * On success: phys_offset is byte offset of the start of the on-disk
   * record (including header) within the backend region.
   *
   * buf_holder must remain valid until cb fires; ownership stays with caller.
   * For SPDK path, payload inside rec must already reference DMA memory
   * or be copied into a DMA buffer by the caller before encode — M1 helper
   * append_record_dma copies into a provided DMA slab.
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
};

} // namespace fastblock::aos
