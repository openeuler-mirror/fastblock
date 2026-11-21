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
  if (drain_poller_ != nullptr) {
    spdk_poller_unregister(&drain_poller_);
    drain_poller_ = nullptr;
  }
  if (store_) {
    store_->set_kv_engine(nullptr);
  }
  kv_.reset();
  if (backend_.io_pending() || (store_ && store_->io_busy())) {
    // stop() drains this. Destroying here would run a bdev completion
    // against a freed pg_store / backend.
    SPDK_ERRLOG("aos_runtime destroyed with IO still in flight\n");
  }
  store_.reset();
  if (opened_) {
    backend_.close();
  }
  opened_ = false;
}

bool aos_runtime::io_still_running() const {
  if (backend_.io_pending()) {
    return true;
  }
  return store_ != nullptr && store_->io_busy();
}

int aos_runtime::drain_poller_fn(void* arg) {
  auto* self = static_cast<aos_runtime*>(arg);
  self->finish_stop(0);
  return SPDK_POLLER_BUSY;
}

int aos_runtime::gc_poller_fn(void* arg) {
  auto* self = static_cast<aos_runtime*>(arg);
  if (self == nullptr || !self->ready() || self->store_ == nullptr ||
      self->gc_in_flight_) {
    return SPDK_POLLER_IDLE;
  }
  osd_store::gc_options opt;
  opt.rewrite_victim = self->gc_cfg_.rewrite_victim;
  opt.free_dead_segments = self->gc_cfg_.free_dead_segments;
  opt.max_rewrite_extents = self->gc_cfg_.max_rewrite_extents;
  opt.persist_manifest = self->gc_cfg_.persist_manifest;
  self->gc_in_flight_ = true;
  self->store_->gc_pass_async(
      opt,
      [](void* a, int /*err*/) {
        auto* rt = static_cast<aos_runtime*>(a);
        ++rt->gc_pass_count_;
        rt->gc_in_flight_ = false;
        if (rt->stop_pending_) {
          rt->finish_stop(0);
        }
      },
      self);
  return SPDK_POLLER_BUSY;
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
        if (rt->stop_pending_ && !rt->gc_in_flight_) {
          rt->finish_stop(0);
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
  if (opened_ || kv_ || store_ || teardown_started_ || backend_.io_pending()) {
    close();
  }
  if (opened_ || kv_ || store_ || teardown_started_ || backend_.io_pending()) {
    SPDK_ERRLOG("aos_runtime open refused: previous stop still in flight\n");
    return -EBUSY;
  }
  // close() on a never-started runtime must not leave stop_finished_ set,
  // or the OSD mkfs shutdown drops its callback and waits forever.
  stop_finished_ = false;
  stop_pending_ = false;
  in_finish_stop_ = false;
  teardown_started_ = false;
  stop_cb_ = nullptr;
  stop_arg_ = nullptr;
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

void aos_runtime::finish_stop(int err) {
  if (in_finish_stop_ || teardown_started_) {
    return;
  }
  // Already closed. stop() may have stored a callback after that; dropping
  // it leaves osd_stop parked on AppendOnlyStore forever.
  if (stop_finished_ && !opened_ && !kv_ && !store_) {
    if (stop_cb_) {
      auto cb = std::move(stop_cb_);
      void* cb_arg = stop_arg_;
      stop_arg_ = nullptr;
      cb(cb_arg, 0);
    }
    return;
  }
  in_finish_stop_ = true;
  if (backend_.driver_idle() && backend_.io_pending()) {
    // Queued writes with nothing in the driver will not complete on their own.
    // Callbacks may re-enter finish_stop. Inflight IO is left alone.
    backend_.fail_queued(-ESHUTDOWN);
  }
  if (io_still_running()) {
    stop_pending_ = true;
    in_finish_stop_ = false;
    if (drain_poller_ == nullptr) {
      SPDK_NOTICELOG(
          "aos_runtime stop waiting write_inflight=%u io_pending=%d "
          "driver_idle=%d pg_busy=%d\n",
          backend_.write_inflight(), backend_.io_pending() ? 1 : 0,
          backend_.driver_idle() ? 1 : 0,
          (store_ && store_->io_busy()) ? 1 : 0);
      drain_poller_ = SPDK_POLLER_REGISTER(drain_poller_fn, this, 100);
    }
    return;
  }
  in_finish_stop_ = false;
  if (drain_poller_ != nullptr) {
    spdk_poller_unregister(&drain_poller_);
    drain_poller_ = nullptr;
  }
  // Do not set stop_finished_ until the backend is closed. A flag latched
  // by an earlier empty stop() must not skip this path.
  teardown_started_ = true;
  stop_pending_ = false;
  stop_finished_ = false;
  auto cb = std::move(stop_cb_);
  void* arg = stop_arg_;
  stop_arg_ = nullptr;
  auto finish = [this, cb = std::move(cb), arg](int e) {
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
    gc_in_flight_ = false;
    ckpt_in_flight_ = false;
    teardown_started_ = false;
    stop_finished_ = true;
    auto extra = std::move(stop_cb_);
    void* extra_arg = stop_arg_;
    stop_arg_ = nullptr;
    if (cb) {
      cb(arg, e);
    }
    if (extra) {
      extra(extra_arg, e);
    }
  };
  if (opened_ || kv_) {
    SPDK_NOTICELOG("aos_runtime stop idle, closing backend\n");
  }
  if (kv_) {
    kv_->stop(
        [finish = std::move(finish)](void* /*a*/, int e) { finish(e); },
        nullptr);
    return;
  }
  finish(err);
}

void aos_runtime::stop(op_complete cb, void* arg) {
  stop_gc_poller();
  if (teardown_started_) {
    // finish() still owns the callback captured when teardown began.
    stop_cb_ = std::move(cb);
    stop_arg_ = arg;
    return;
  }
  if (gc_in_flight_ || ckpt_in_flight_) {
    stop_pending_ = true;
    stop_cb_ = std::move(cb);
    stop_arg_ = arg;
    return;
  }
  stop_cb_ = std::move(cb);
  stop_arg_ = arg;
  finish_stop(0);
}

void aos_runtime::close() {
  // Nothing was opened. stop() would latch stop_finished_ and the real
  // shutdown after format() would return without invoking its callback.
  if (!opened_ && !kv_ && !store_ && !teardown_started_ &&
      drain_poller_ == nullptr && gc_poller_ == nullptr &&
      ckpt_poller_ == nullptr && !gc_in_flight_ && !ckpt_in_flight_ &&
      !backend_.io_pending()) {
    return;
  }
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
