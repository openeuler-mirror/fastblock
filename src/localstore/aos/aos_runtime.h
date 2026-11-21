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

#include <spdk/thread.h>

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

  /** Background GC knobs (design P5). Defaults are production-safe. */
  struct gc_poller_config {
    bool enabled{true};
    /** Period between gc_pass runs (microseconds). Default 5s. */
    uint64_t period_us{5ull * 1000 * 1000};
    /** Max rewrite extents per pass (rate limit). */
    uint32_t max_rewrite_extents{16};
    bool rewrite_victim{true};
    bool free_dead_segments{true};
    bool persist_manifest{true};
    /** Period between coordinated checkpoints (0 = disabled). Default 30s. */
    uint64_t checkpoint_period_us{30ull * 1000 * 1000};
  };

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

  /**
   * Stop GC, drain kv WAL (term/vote/cfg), then tear down. cb on this
   * reactor after kv flush. close() is a fire-and-forget wrapper.
   */
  void stop(op_complete cb, void* arg);

  /**
   * Start/stop background GC poller on the current SPDK thread.
   * Safe to call multiple times; start is idempotent when already running.
   */
  void start_gc_poller();
  void start_gc_poller(const gc_poller_config& cfg);
  void stop_gc_poller();
  bool gc_poller_running() const { return gc_poller_ != nullptr; }
  uint64_t gc_pass_count() const { return gc_pass_count_; }
  uint64_t checkpoint_pass_count() const { return ckpt_pass_count_; }

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
  static int gc_poller_fn(void* arg);
  static int ckpt_poller_fn(void* arg);
  static int drain_poller_fn(void* arg);
  bool io_still_running() const;

  spdk_bdev_backend backend_;
  std::unique_ptr<osd_store> store_;
  std::unique_ptr<kv_engine> kv_;
  std::string bdev_name_;
  uint64_t region_offset_{0};
  uint64_t region_length_{0};
  uint64_t segment_size_{0};
  bool opened_{false};

  struct spdk_poller* gc_poller_{nullptr};
  struct spdk_poller* ckpt_poller_{nullptr};
  struct spdk_poller* drain_poller_{nullptr};
  gc_poller_config gc_cfg_{};
  uint64_t gc_pass_count_{0};
  uint64_t ckpt_pass_count_{0};
  bool ckpt_in_flight_{false};
  bool gc_in_flight_{false};
  bool stop_pending_{false};
  // True only after kv is destroyed and the backend is closed.
  bool stop_finished_{false};
  bool in_finish_stop_{false};
  // kv::stop is in flight. A second finish_stop must not close again.
  bool teardown_started_{false};
  op_complete stop_cb_;
  void* stop_arg_{nullptr};

  void finish_stop(int err);
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

/**
 * ZNS-oriented segment sizing (design P5+): prefer zone_size when known;
 * otherwise fall back to kDefaultSegmentSize. Aligns down to 1 MiB.
 */
inline uint64_t choose_segment_size_for_device(uint64_t zone_size_bytes = 0) {
  constexpr uint64_t kAlign = 1024ull * 1024;
  if (zone_size_bytes >= kAlign) {
    return (zone_size_bytes / kAlign) * kAlign;
  }
  return kDefaultSegmentSize;
}

} // namespace fastblock::aos
