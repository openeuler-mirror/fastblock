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

#include <cstdint>
#include <vector>

namespace fastblock::aos {

/**
 * In-memory device_backend for unit tests only.
 *
 * Preserves the async callback API shape used by SPDK paths.
 * Completions are invoked on the calling stack after the operation is
 * applied (no reactor). Production code must use spdk_bdev_backend.
 */
class mem_device_backend final : public device_backend {
public:
  explicit mem_device_backend(uint64_t capacity_bytes,
                              uint32_t block_size = 4096);

  uint32_t block_size() const override { return block_size_; }
  uint64_t write_pointer() const override { return write_pointer_; }
  uint64_t capacity() const override { return capacity_; }

  void set_write_pointer(uint64_t bytes) override { write_pointer_ = bytes; }

  // Expose storage for crash/torn-write test injection.
  std::vector<uint8_t>& storage() { return storage_; }
  const std::vector<uint8_t>& storage() const { return storage_; }

  void append(void* data, uint64_t nbytes, append_complete cb,
              void* arg) override;
  void write_at(uint64_t phys_offset, void* data, uint64_t nbytes,
                append_complete cb, void* arg) override;
  void read(uint64_t phys_offset, void* data, uint64_t nbytes, op_complete cb,
            void* arg) override;
  void flush(op_complete cb, void* arg) override;

private:
  uint32_t block_size_{4096};
  uint64_t capacity_{0};
  uint64_t write_pointer_{0};
  std::vector<uint8_t> storage_;
};

} // namespace fastblock::aos
