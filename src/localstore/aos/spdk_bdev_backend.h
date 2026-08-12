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

#include <spdk/bdev.h>
#include <spdk/thread.h>

#include <string>

namespace fastblock::aos {

/**
 * SPDK bdev-backed append-only region.
 *
 * Owns: open desc + io_channel for the calling SPDK thread.
 * Does not own: the bdev itself (created by app/blob_manager path).
 *
 * Region is a contiguous [region_offset, region_offset + region_length)
 * in bytes on the bdev. write_pointer is relative to region start and
 * only moves forward on successful append completion.
 *
 * Must be constructed/used on the SPDK thread that owns channel_.
 */
class spdk_bdev_backend final : public device_backend {
public:
  spdk_bdev_backend() = default;
  ~spdk_bdev_backend() override;

  spdk_bdev_backend(const spdk_bdev_backend&) = delete;
  spdk_bdev_backend& operator=(const spdk_bdev_backend&) = delete;

  /**
   * Open bdev by name on the current SPDK thread.
   * region_offset/region_length are byte ranges on the bdev; must be
   * block-aligned. write_pointer starts at 0 within the region (caller
   * may set_write_pointer after recovery scan).
   *
   * Returns 0 or -errno. On failure object is not usable.
   */
  int open(const char* bdev_name, uint64_t region_offset,
           uint64_t region_length);

  void close();

  // After crash recovery, set pointer to end of last good record.
  void set_write_pointer(uint64_t bytes_from_region_start) override;

  uint32_t block_size() const override { return block_size_; }
  uint64_t write_pointer() const override { return write_pointer_; }
  uint64_t capacity() const override { return region_length_; }

  void append(void* data, uint64_t nbytes, append_complete cb,
              void* arg) override;
  void read(uint64_t phys_offset, void* data, uint64_t nbytes, op_complete cb,
            void* arg) override;
  void flush(op_complete cb, void* arg) override;

private:
  static void bdev_event_cb(enum spdk_bdev_event_type type,
                            struct spdk_bdev* bdev, void* event_ctx);
  static void write_done(struct spdk_bdev_io* bdev_io, bool success,
                         void* cb_arg);
  static void read_done(struct spdk_bdev_io* bdev_io, bool success,
                        void* cb_arg);
  static void flush_done(struct spdk_bdev_io* bdev_io, bool success,
                         void* cb_arg);

  bool ready() const { return desc_ != nullptr && channel_ != nullptr; }

  struct spdk_bdev* bdev_{nullptr};
  struct spdk_bdev_desc* desc_{nullptr};
  struct spdk_io_channel* channel_{nullptr};

  uint32_t block_size_{0};
  uint64_t region_offset_{0};  // absolute byte offset on bdev
  uint64_t region_length_{0};  // capacity of this region
  uint64_t write_pointer_{0};  // relative to region start

  // Serialize appends: at most one in-flight append for M1 simplicity.
  bool append_inflight_{false};
};

} // namespace fastblock::aos
