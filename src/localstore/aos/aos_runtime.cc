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

#include "aos_runtime.h"

#include "layout.h"

#include <spdk/log.h>

#include <algorithm>
#include <cerrno>
#include <cctype>

namespace fastblock::aos {

namespace {

constexpr uint64_t kRegionAlign = 1024ull * 1024; // 1 MiB

} // namespace

aos_runtime::~aos_runtime() { close(); }

int aos_runtime::open(const std::string& bdev_name, uint64_t region_offset,
                      uint64_t region_length, uint64_t segment_size) {
  close();
  if (bdev_name.empty() || region_length < kMetaSize + 4096) {
    return -EINVAL;
  }
  int rc = backend_.open(bdev_name.c_str(), region_offset, region_length);
  if (rc != 0) {
    SPDK_ERRLOG("aos_runtime open bdev %s off %lu len %lu failed: %d\n",
                bdev_name.c_str(), region_offset, region_length, rc);
    return rc;
  }
  store_ = std::make_unique<osd_store>(&backend_, buffer_mode::spdk_dma,
                                       segment_size);
  kv_ = std::make_unique<kv_engine>(&backend_, store_->shared_allocator(),
                                   buffer_mode::spdk_dma);
  store_->set_kv_engine(kv_.get());
  bdev_name_ = bdev_name;
  region_offset_ = region_offset;
  region_length_ = region_length;
  segment_size_ = segment_size;
  opened_ = true;
  SPDK_NOTICELOG(
      "aos_runtime open bdev=%s off=%lu len=%lu segment=%lu\n",
      bdev_name.c_str(), region_offset, region_length, segment_size);
  return 0;
}

void aos_runtime::format(op_complete cb, void* arg) {
  if (!ready()) {
    if (cb) {
      cb(arg, -EINVAL);
    }
    return;
  }
  store_->format(
      [this, cb = std::move(cb), arg](void* /*a*/, int err) {
        if (err == 0 && kv_) {
          kv_->start();
        }
        if (cb) {
          cb(arg, err);
        }
      },
      nullptr);
}

void aos_runtime::recover(op_complete cb, void* arg) {
  if (!ready()) {
    if (cb) {
      cb(arg, -EINVAL);
    }
    return;
  }
  store_->recover(
      [this, cb = std::move(cb), arg](void* /*a*/, int err) {
        if (err == 0 && store_) {
          const uint32_t dropped = store_->drop_unsealed_staging();
          if (dropped > 0) {
            SPDK_NOTICELOG(
                "aos_runtime recover dropped %u unsealed staging epochs\n",
                dropped);
          }
          if (kv_) {
            kv_->start();
          }
          if (store_) {
            const auto pgs = store_->list_pg_ids();
            SPDK_NOTICELOG("aos_runtime recover: %zu pg id(s) in latest_epoch\n",
                           pgs.size());
            for (const auto& id : pgs) {
              SPDK_NOTICELOG(
                  "aos recover pg %lu.%lu serving=%lu latest=%lu\n", id.first,
                  id.second, store_->serving_epoch(id.first, id.second),
                  store_->latest_epoch(id.first, id.second));
            }
          }
        }
        if (cb) {
          cb(arg, err);
        }
      },
      nullptr);
}

void aos_runtime::close() {
  if (kv_) {
    kv_->stop([](void*, int) {}, nullptr);
  }
  if (store_) {
    store_->set_kv_engine(nullptr);
  }
  kv_.reset();
  store_.reset();
  backend_.close();
  opened_ = false;
  bdev_name_.clear();
  region_offset_ = 0;
  region_length_ = 0;
  segment_size_ = 0;
}

object_engine_mode parse_object_engine_mode(const std::string& s) {
  std::string lower;
  lower.reserve(s.size());
  for (char c : s) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "aos" || lower == "append_only" || lower == "append-only") {
    return object_engine_mode::aos;
  }
  if (lower == "shadow") {
    return object_engine_mode::shadow;
  }
  return object_engine_mode::blob;
}

const char* object_engine_mode_name(object_engine_mode m) {
  switch (m) {
  case object_engine_mode::aos:
    return "aos";
  case object_engine_mode::shadow:
    return "shadow";
  case object_engine_mode::blob:
  default:
    return "blob";
  }
}

bool compute_shard_region(uint64_t capacity_bytes, uint32_t shard_count,
                          uint32_t shard_id, uint64_t* out_offset,
                          uint64_t* out_length) {
  if (out_offset == nullptr || out_length == nullptr || shard_count == 0 ||
      shard_id >= shard_count) {
    return false;
  }
  uint64_t per = capacity_bytes / shard_count;
  per = (per / kRegionAlign) * kRegionAlign;
  if (per < kMetaSize + kRegionAlign) {
    return false;
  }
  *out_offset = static_cast<uint64_t>(shard_id) * per;
  *out_length = per;
  return true;
}

} // namespace fastblock::aos
