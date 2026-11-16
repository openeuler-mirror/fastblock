/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "mem_device_backend.h"

#include <cerrno>
#include <cstring>

namespace fastblock::aos {

mem_device_backend::mem_device_backend(uint64_t capacity_bytes,
                                       uint32_t block_size)
    : block_size_(block_size), capacity_(capacity_bytes) {
  storage_.resize(static_cast<size_t>(capacity_bytes), 0);
}

void mem_device_backend::append(void* data, uint64_t nbytes, append_complete cb,
                                 void* arg) {
  write_at(write_pointer_, data, nbytes, std::move(cb), arg);
}

void mem_device_backend::write_at(uint64_t phys_offset, void* data,
                                  uint64_t nbytes, append_complete cb,
                                  void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (data == nullptr || nbytes == 0 || (nbytes % block_size_) != 0 ||
      (phys_offset % block_size_) != 0) {
    cb(arg, 0, -EINVAL);
    return;
  }
  if (phys_offset + nbytes > capacity_) {
    cb(arg, 0, -ENOSPC);
    return;
  }
  std::memcpy(storage_.data() + static_cast<size_t>(phys_offset), data,
              static_cast<size_t>(nbytes));
  if (phys_offset + nbytes > write_pointer_) {
    write_pointer_ = phys_offset + nbytes;
  }
  cb(arg, phys_offset, 0);
}

void mem_device_backend::read(uint64_t phys_offset, void* data, uint64_t nbytes,
                               op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (data == nullptr || nbytes == 0 || (nbytes % block_size_) != 0 ||
      (phys_offset % block_size_) != 0) {
    cb(arg, -EINVAL);
    return;
  }
  if (phys_offset + nbytes > capacity_) {
    cb(arg, -ERANGE);
    return;
  }
  // High-water bound: allow reading any committed range.
  if (phys_offset + nbytes > write_pointer_) {
    cb(arg, -ERANGE);
    return;
  }
  std::memcpy(data, storage_.data() + static_cast<size_t>(phys_offset),
              static_cast<size_t>(nbytes));
  cb(arg, 0);
}

void mem_device_backend::flush(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  // Memory model: already durable once append completed.
  cb(arg, 0);
}

} // namespace fastblock::aos
