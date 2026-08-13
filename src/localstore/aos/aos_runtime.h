/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#pragma once

#include "device_backend.h"
#include "kv_engine.h"
#include "osd_store.h"
#include "spdk_bdev_backend.h"
#include "types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace fastblock::aos {

/**
 * Per-shard AOS lifecycle for OSD process integration (design M4).
 *
 * Owns spdk_bdev_backend + osd_store for one reactor shard region.
 * Call open() then format() (mkfs) or recover() (normal start) on the
 * owning SPDK thread only.
 */
class aos_runtime {
public:
  using op_complete = device_backend::op_complete;

  aos_runtime() = default;
  ~aos_runtime();

  aos_runtime(const aos_runtime&) = delete;
  aos_runtime& operator=(const aos_runtime&) = delete;

  /**
   * Open bdev region and construct osd_store. Does not format/recover.
   * segment_size: 0 => single data segment (tests); production uses e.g. 64MiB.
   * Returns 0 or -errno.
   */
  int open(const std::string& bdev_name, uint64_t region_offset,
           uint64_t region_length, uint64_t segment_size = 0);

  /** Fresh layout: superblock + empty manifest. */
  void format(op_complete cb, void* arg);

  /** Load superblock/manifest and replay data region. */
  void recover(op_complete cb, void* arg);

  void close();

  bool ready() const { return store_ != nullptr && opened_; }
  osd_store* store() { return store_.get(); }
  const osd_store* store() const { return store_.get(); }
  kv_engine* kv() { return kv_.get(); }
  const kv_engine* kv() const { return kv_.get(); }
  spdk_bdev_backend* backend() { return &backend_; }
  const std::string& bdev_name() const { return bdev_name_; }
  uint64_t region_offset() const { return region_offset_; }
  uint64_t region_length() const { return region_length_; }
  uint64_t segment_size() const { return segment_size_; }

private:
  spdk_bdev_backend backend_;
  std::unique_ptr<osd_store> store_;
  std::unique_ptr<kv_engine> kv_;
  std::string bdev_name_;
  uint64_t region_offset_{0};
  uint64_t region_length_{0};
  uint64_t segment_size_{0};
  bool opened_{false};
};

/** Parse "blob" | "aos" | "shadow". Unknown => blob. */
object_engine_mode parse_object_engine_mode(const std::string& s);

/** Human-readable mode name. */
const char* object_engine_mode_name(object_engine_mode m);

/**
 * Split a device into per-shard regions (same idea as blobstore cores).
 * Aligns length down to 1MiB; returns false if too small for meta+data.
 */
bool compute_shard_region(uint64_t capacity_bytes, uint32_t shard_count,
                          uint32_t shard_id, uint64_t* out_offset,
                          uint64_t* out_length);

/** Default production segment size (64 MiB). */
inline constexpr uint64_t kDefaultSegmentSize = 64ull * 1024 * 1024;

} // namespace fastblock::aos
