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
#include <cstdlib>
#include <cstring>

namespace fastblock::aos {

namespace {

constexpr uint64_t kRegionAlign = 1024ull * 1024; // 1 MiB

} // namespace

aos_runtime::~aos_runtime() {
  stop_gc_poller();
  if (store_) {
    store_->set_kv_engine(nullptr);
  }
  kv_.reset();
  store_.reset();
  if (opened_) {
    backend_.close();
  }
  opened_ = false;
}

int aos_runtime::gc_poller_fn(void* arg) {
  auto* self = static_cast<aos_runtime*>(arg);
  if (self == nullptr || !self->ready() || self->store_ == nullptr) {
    return SPDK_POLLER_IDLE;
  }
  osd_store::gc_options opt;
  opt.rewrite_victim = self->gc_cfg_.rewrite_victim;
  opt.free_dead_segments = self->gc_cfg_.free_dead_segments;
  opt.max_rewrite_extents = self->gc_cfg_.max_rewrite_extents;
  opt.persist_manifest = self->gc_cfg_.persist_manifest;
  const auto st = self->store_->gc_pass(opt);
  ++self->gc_pass_count_;
  if (st.extents_rewritten > 0 || st.segments_freed > 0) {
    SPDK_NOTICELOG(
        "aos_runtime GC pass #%lu rewrite=%u freed=%u\n", self->gc_pass_count_,
        st.extents_rewritten, st.segments_freed);
    if (st.segments_freed > 0 && !self->ckpt_in_flight_) {
      self->ckpt_in_flight_ = true;
      self->store_->checkpoint_pass(
          [](void* a, int err) {
            auto* rt = static_cast<aos_runtime*>(a);
            rt->ckpt_in_flight_ = false;
            if (err == 0) {
              ++rt->ckpt_pass_count_;
            }
          },
          self);
    }
    return SPDK_POLLER_BUSY;
  }
  return SPDK_POLLER_IDLE;
}

void aos_runtime::start_gc_poller() { start_gc_poller(gc_poller_config{}); }

int aos_runtime::ckpt_poller_fn(void* arg) {
  auto* self = static_cast<aos_runtime*>(arg);
  if (self == nullptr || !self->ready() || self->store_ == nullptr ||
      self->ckpt_in_flight_) {
    return SPDK_POLLER_IDLE;
  }
  self->ckpt_in_flight_ = true;
  self->store_->checkpoint_pass(
      [](void* a, int err) {
        auto* rt = static_cast<aos_runtime*>(a);
        rt->ckpt_in_flight_ = false;
        if (err == 0) {
          ++rt->ckpt_pass_count_;
          SPDK_NOTICELOG("aos_runtime checkpoint pass #%lu\n",
                         rt->ckpt_pass_count_);
        } else {
          SPDK_ERRLOG("aos_runtime checkpoint pass failed: %d\n", err);
        }
      },
      self);
  return SPDK_POLLER_BUSY;
}

void aos_runtime::start_gc_poller(const gc_poller_config& cfg) {
  stop_gc_poller();
  if (!cfg.enabled || !ready()) {
    return;
  }
  gc_cfg_ = cfg;
  const uint64_t period =
      cfg.period_us == 0 ? 5ull * 1000 * 1000 : cfg.period_us;
  gc_poller_ = SPDK_POLLER_REGISTER(gc_poller_fn, this, period);
  if (cfg.checkpoint_period_us > 0) {
    ckpt_poller_ =
        SPDK_POLLER_REGISTER(ckpt_poller_fn, this, cfg.checkpoint_period_us);
  }
  SPDK_NOTICELOG(
      "aos_runtime GC poller started period_us=%lu max_rewrite=%u "
      "ckpt_period_us=%lu\n",
      period, cfg.max_rewrite_extents, cfg.checkpoint_period_us);
}

void aos_runtime::stop_gc_poller() {
  if (ckpt_poller_ != nullptr) {
    spdk_poller_unregister(&ckpt_poller_);
    ckpt_poller_ = nullptr;
  }
  if (gc_poller_ != nullptr) {
    spdk_poller_unregister(&gc_poller_);
    gc_poller_ = nullptr;
    SPDK_NOTICELOG("aos_runtime GC poller stopped (passes=%lu ckpts=%lu)\n",
                   gc_pass_count_, ckpt_pass_count_);
  }
}

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
        if (err == 0) {
          start_gc_poller();
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
            char keys[192]{};
            size_t used = 0;
            kv_->for_each_key([&](const std::string& key) {
              if (used + key.size() + 2 >= sizeof(keys)) {
                return;
              }
              if (used > 0) {
                keys[used++] = ',';
              }
              std::memcpy(keys + used, key.data(), key.size());
              used += key.size();
            });
            SPDK_NOTICELOG("aos recover kv table_size=%zu keys=%s\n",
                           kv_->table_size(), used > 0 ? keys : "-");
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
          start_gc_poller();
        }
        if (cb) {
          cb(arg, err);
        }
      },
      nullptr);
}

void aos_runtime::stop(op_complete cb, void* arg) {
  stop_gc_poller();
  auto finish = [this, cb = std::move(cb), arg](int err) {
    if (store_) {
      store_->set_kv_engine(nullptr);
    }
    kv_.reset();
    store_.reset();
    if (opened_) {
      backend_.close();
    }
    opened_ = false;
    bdev_name_.clear();
    region_offset_ = 0;
    region_length_ = 0;
    segment_size_ = 0;
    gc_pass_count_ = 0;
    if (cb) {
      cb(arg, err);
    }
  };
  if (kv_) {
    kv_->stop(
        [finish = std::move(finish)](void* /*a*/, int err) { finish(err); },
        nullptr);
    return;
  }
  finish(0);
}

void aos_runtime::close() {
  stop([](void*, int) {}, nullptr);
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

namespace {
bool g_unified_log = true;
bool g_unified_log_inited = false;
}

bool unified_log_enabled() {
  if (!g_unified_log_inited) {
    g_unified_log_inited = true;
    const char* e = std::getenv("FASTBLOCK_AOS_UNIFIED_LOG");
    // Dev default is on (log=STM). Only an explicit "0" disables.
    if (e != nullptr && e[0] == '0' && e[1] == '\0') {
      g_unified_log = false;
    } else {
      g_unified_log = true;
    }
  }
  return g_unified_log;
}

void set_unified_log_enabled(bool on) {
  g_unified_log = on;
  g_unified_log_inited = true;
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
