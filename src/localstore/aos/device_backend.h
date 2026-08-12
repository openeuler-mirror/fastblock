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

#include <cerrno>
#include <cstdint>
#include <functional>

namespace fastblock::aos {

/**
 * DeviceBackend is the ONLY I/O boundary for AOS.
 *
 * Hard rules (SPDK / fastblock):
 * - All I/O is asynchronous; completion is always via callback.
 * - Callers run on an SPDK thread (reactor); backends must not block.
 * - Buffers for DMA I/O must come from spdk_malloc / buffer pools, not new[].
 * - Append advances a write pointer; there is no random overwrite API.
 * - flush() is the durability barrier (maps to spdk_bdev_flush_*).
 *
 * POSIX pwrite/fsync is NOT an acceptable production shape.
 * A test double, if any, must still present this async callback model.
 */
class device_backend {
public:
  // err: 0 success, negative errno otherwise.
  using op_complete = std::function<void(void* arg, int err)>;
  // phys_offset: byte offset from device/region start where data was appended.
  using append_complete =
      std::function<void(void* arg, uint64_t phys_offset, int err)>;

  virtual ~device_backend() = default;

  // Logical block size of the underlying device (usually 512 or 4096).
  virtual uint32_t block_size() const = 0;

  // Current append write pointer in bytes (monotonic).
  virtual uint64_t write_pointer() const = 0;

  // After recovery, set the append cursor to end of last good record.
  virtual void set_write_pointer(uint64_t bytes_from_region_start) = 0;

  // Usable capacity in bytes for this backend region.
  virtual uint64_t capacity() const = 0;

  /**
   * Append [data, data+nbytes) at the current write pointer.
   * nbytes must be block_size aligned.
   * data must be DMA-safe when using spdk_bdev_backend.
   * On success callback: phys_offset is the start of this append.
   * write_pointer advances only after successful completion.
   */
  virtual void append(void* data, uint64_t nbytes, append_complete cb,
                      void* arg) = 0;

  /**
   * Absolute write at phys_offset (block aligned). Used by multi-segment
   * allocator; does not require phys_offset == write_pointer().
   * On success, write_pointer becomes max(write_pointer, phys+nbytes)
   * (high-water for recover/read bounds).
   * Default: not implemented (-ENOTSUP).
   */
  virtual void write_at(uint64_t phys_offset, void* data, uint64_t nbytes,
                        append_complete cb, void* arg) {
    (void)phys_offset;
    (void)data;
    (void)nbytes;
    if (cb) {
      cb(arg, 0, -ENOTSUP);
    }
  }

  /**
   * Read nbytes from absolute phys_offset into data (DMA-safe).
   * phys_offset and nbytes must be block aligned.
   */
  virtual void read(uint64_t phys_offset, void* data, uint64_t nbytes,
                    op_complete cb, void* arg) = 0;

  /**
   * Durability barrier for previously completed appends.
   * Store write callbacks that require durability must wait for flush
   * (design §8), possibly batched.
   */
  virtual void flush(op_complete cb, void* arg) = 0;
};

} // namespace fastblock::aos
