/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "osd_store.h"
#include "checkpoint.h"
#include "record.h"

#include <spdk/env.h>
#include <spdk/log.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>
#include <tuple>
#include <vector>

namespace fastblock::aos {
namespace {
constexpr uint64_t kRecoverChunk = 256 * 1024;
}

struct osd_store::meta_req {
  osd_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  int phase{0};
  int err{0};
  std::vector<uint8_t> sb_bytes;
  std::vector<uint8_t> mf_bytes;
  void* dma{nullptr};
  uint64_t dma_cap{0};
  superblock sb{};
  manifest mf{};
  uint64_t zero_off{0};
  uint64_t zero_end{0};
};

struct osd_store::recover_req {
  osd_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  int phase{0}; // 0=read sba, 1=sbb, 2=manifest, 3=data scan
  uint64_t pos{0};
  uint64_t last_good{0};       // decode cursor: device offset of pending[0]
  uint64_t last_valid_end{0};  // end of last successfully applied record
  uint64_t scan_end{0};        // exclusive upper bound for the data scan
  void* buf{nullptr};
  uint64_t buf_cap{0};
  std::vector<uint8_t> pending;
  superblock sb_a{};
  superblock sb_b{};
  bool sb_a_ok{false};
  bool sb_b_ok{false};
  superblock sb_best{};
  manifest mf{};
  bool mf_ok{false};
  uint32_t applied{0};
  uint32_t applied_data{0};
  uint32_t applied_log{0};
  uint32_t applied_kv{0};
  uint32_t skipped{0};
  int first_decode_st{-1};
  uint32_t first_magic{0};
  uint32_t zero_streak{0};
  char kv_keys[160]{};
  uint32_t kv_keys_n{0};
  bool have_seg_table{false};
  std::vector<uint64_t> ckpt_queue;
  size_t ckpt_idx{0};
};

osd_store::osd_store(device_backend* backend, buffer_mode mode,
                     uint64_t segment_size)
    : backend_(backend), mode_(mode),
      configured_segment_size_(segment_size) {
  const uint64_t cap = backend ? backend->capacity() : 0;
  const uint32_t bs = backend ? backend->block_size() : 4096;
  data_offset_ = kMetaSize;
  if (cap > kMetaSize) {
    data_length_ = ((cap - kMetaSize) / bs) * bs;
  } else {
    data_length_ = 0;
  }
  const uint64_t ss =
      segment_size == 0 ? data_length_ : segment_size;
  alloc_ = std::make_shared<segment_allocator>(data_offset_, data_length_, ss,
                                               bs);
}

uint64_t osd_store::latest_epoch(uint64_t pool_id, uint64_t pg_id) const {
  auto it = latest_epoch_.find({pool_id, pg_id});
  if (it == latest_epoch_.end()) {
    return 0;
  }
  return it->second;
}

std::vector<std::pair<uint64_t, uint64_t>> osd_store::list_pg_ids() const {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  out.reserve(latest_epoch_.size());
  for (const auto& kv : latest_epoch_) {
    if (kv.second != 0) {
      out.push_back(kv.first);
    }
  }
  return out;
}

uint64_t osd_store::serving_epoch(uint64_t pool_id, uint64_t pg_id) const {
  auto it = serving_epoch_.find({pool_id, pg_id});
  if (it != serving_epoch_.end() && it->second != 0) {
    return it->second;
  }
  return latest_epoch(pool_id, pg_id);
}

void osd_store::set_serving_epoch(uint64_t pool_id, uint64_t pg_id,
                                  uint64_t epoch) {
  if (epoch == 0) {
    serving_epoch_.erase({pool_id, pg_id});
  } else {
    serving_epoch_[{pool_id, pg_id}] = epoch;
  }
}

void osd_store::set_latest_epoch(uint64_t pool_id, uint64_t pg_id,
                                 uint64_t epoch) {
  if (epoch == 0) {
    latest_epoch_.erase({pool_id, pg_id});
  } else {
    latest_epoch_[{pool_id, pg_id}] = epoch;
  }
}

pg_store* osd_store::get_pg(uint64_t pool_id, uint64_t pg_id,
                            uint64_t store_epoch) {
  auto it = pgs_.find({pool_id, pg_id, store_epoch});
  if (it == pgs_.end()) {
    return nullptr;
  }
  return it->second.get();
}

pg_store* osd_store::get_latest_pg(uint64_t pool_id, uint64_t pg_id) {
  const uint64_t e = latest_epoch(pool_id, pg_id);
  if (e == 0) {
    return nullptr;
  }
  return get_pg(pool_id, pg_id, e);
}

pg_store* osd_store::get_active_pg(uint64_t pool_id, uint64_t pg_id) {
  const uint64_t e = serving_epoch(pool_id, pg_id);
  if (e == 0) {
    return nullptr;
  }
  return get_pg(pool_id, pg_id, e);
}

pg_store* osd_store::find_pg(uint64_t pool_id, uint64_t pg_id) {
  pg_store* best_data = nullptr;
  pg_store* best_any = nullptr;
  uint64_t best_data_epoch = 0;
  uint64_t best_any_epoch = 0;
  for (const auto& kv : pgs_) {
    if (std::get<0>(kv.first) != pool_id || std::get<1>(kv.first) != pg_id) {
      continue;
    }
    const uint64_t epoch = std::get<2>(kv.first);
    if (epoch >= best_any_epoch) {
      best_any_epoch = epoch;
      best_any = kv.second.get();
    }
    if (kv.second && kv.second->object_count() > 0 && epoch >= best_data_epoch) {
      best_data_epoch = epoch;
      best_data = kv.second.get();
    }
  }
  return best_data != nullptr ? best_data : best_any;
}

void osd_store::retarget_serving_to_recovered() {
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> seen;
  for (const auto& kv : pgs_) {
    seen[{std::get<0>(kv.first), std::get<1>(kv.first)}] = 1;
  }
  for (const auto& kv : seen) {
    pg_store* pg = find_pg(kv.first.first, kv.first.second);
    if (pg != nullptr) {
      set_serving_epoch(kv.first.first, kv.first.second, pg->id().store_epoch);
      SPDK_NOTICELOG(
          "aos recover retarget pg %lu.%lu serving=%lu objects=%lu latest=%lu\n",
          kv.first.first, kv.first.second, pg->id().store_epoch,
          static_cast<unsigned long>(pg->object_count()),
          latest_epoch(kv.first.first, kv.first.second));
    }
  }
}

pg_store* osd_store::bind_existing_pg(uint64_t pool_id, uint64_t pg_id) {
  if (auto* existing = find_pg(pool_id, pg_id)) {
    set_serving_epoch(pool_id, pg_id, existing->id().store_epoch);
    return existing;
  }
  const uint64_t le = latest_epoch(pool_id, pg_id);
  if (le == 0) {
    return nullptr;
  }
  pg_store* pg = ensure_pg(pool_id, pg_id, le);
  if (pg != nullptr) {
    set_serving_epoch(pool_id, pg_id, le);
  }
  return pg;
}

void osd_store::drop_pg_epoch(uint64_t pool_id, uint64_t pg_id,
                              uint64_t epoch) {
  pgs_.erase({pool_id, pg_id, epoch});
}

uint32_t osd_store::drop_unsealed_staging() {
  uint32_t dropped = 0;
  // Collect keys first — erase invalidates iterators.
  std::vector<pg_key> doomed;
  for (const auto& kv : pgs_) {
    const uint64_t pool = std::get<0>(kv.first);
    const uint64_t pg = std::get<1>(kv.first);
    const uint64_t epoch = std::get<2>(kv.first);
    const uint64_t serve = serving_epoch(pool, pg);
    if (epoch == serve) {
      continue; // never drop serving generation
    }
    pg_store* s = kv.second.get();
    if (s == nullptr) {
      continue;
    }
    // Incomplete install: has objects or is newer than serve, but not sealed.
    if (!s->install_sealed() && !s->install_activated() &&
        epoch > serve) {
      doomed.push_back(kv.first);
    }
  }
  for (const auto& key : doomed) {
    const uint64_t pool = std::get<0>(key);
    const uint64_t pg = std::get<1>(key);
    const uint64_t epoch = std::get<2>(key);
    pgs_.erase(key);
    if (latest_epoch(pool, pg) == epoch) {
      set_latest_epoch(pool, pg, serving_epoch(pool, pg));
    }
    ++dropped;
  }
  return dropped;
}

pg_store* osd_store::ensure_pg(uint64_t pool_id, uint64_t pgid,
                               uint64_t epoch) {
  auto key = pg_key{pool_id, pgid, epoch};
  auto it = pgs_.find(key);
  if (it != pgs_.end()) {
    return it->second.get();
  }
  const fastblock::aos::pg_id pid{pool_id, pgid, epoch};
  auto store = std::make_unique<pg_store>(backend_, pid, alloc_, mode_);
  pg_store* raw = store.get();
  pgs_.emplace(key, std::move(store));
  auto& le = latest_epoch_[{pool_id, pgid}];
  if (epoch > le) {
    le = epoch;
  }
  return raw;
}

void osd_store::clear_all_pgs() {
  pgs_.clear();
  logs_.clear();
  serving_epoch_.clear();
  // Keep latest_epoch_ if loading from manifest first; recover clears both.
}

log_store* osd_store::get_or_create_log(uint64_t pool_id, uint64_t pg_id,
                                        uint64_t store_epoch) {
  auto key = pg_key{pool_id, pg_id, store_epoch};
  auto it = logs_.find(key);
  if (it != logs_.end()) {
    return it->second.get();
  }
  if (backend_ == nullptr || alloc_ == nullptr) {
    return nullptr;
  }
  const fastblock::aos::pg_id pid{pool_id, pg_id, store_epoch};
  auto log = std::make_unique<log_store>(backend_, pid, alloc_, mode_);
  log_store* raw = log.get();
  raw->set_name_lookup(
      [this, pool_id, pg_id](uint64_t object_id, std::string* name) -> int {
        pg_store* pg = get_active_pg(pool_id, pg_id);
        if (pg == nullptr) {
          return -ENOENT;
        }
        return pg->name_of(object_id, name);
      });
  logs_.emplace(key, std::move(log));
  return raw;
}

log_store* osd_store::get_log(uint64_t pool_id, uint64_t pg_id,
                              uint64_t store_epoch) {
  auto it = logs_.find(pg_key{pool_id, pg_id, store_epoch});
  if (it == logs_.end()) {
    return nullptr;
  }
  return it->second.get();
}

log_store* osd_store::get_active_log(uint64_t pool_id, uint64_t pg_id) {
  const uint64_t epoch = serving_epoch(pool_id, pg_id);
  if (epoch == 0) {
    return nullptr;
  }
  return get_or_create_log(pool_id, pg_id, epoch);
}

manifest osd_store::build_manifest() const {
  manifest m;
  m.generation = manifest_generation_ + 1;
  uint64_t hw = backend_ ? backend_->write_pointer() : data_offset_;
  if (alloc_ != nullptr) {
    const uint32_t open = alloc_->open_segment();
    if (open != segment_allocator::kInvalidSeg) {
      const uint64_t cur =
          alloc_->segment_base(open) + alloc_->open_cursor();
      if (cur > hw) {
        hw = cur;
      }
    }
  }
  m.data_high_water = hw;
  if (m.data_high_water < data_offset_) {
    m.data_high_water = data_offset_;
  }
  if (alloc_) {
    m.open_segment_id = alloc_->open_segment();
    m.open_cursor = alloc_->open_cursor();
    alloc_->export_states(&m.segment_states);
  }
  m.recover_scan_from = recover_scan_from_;
  if (kv_ != nullptr) {
    m.kv_checkpoint_phys = kv_->last_checkpoint_phys();
  }
  for (const auto& kv : pgs_) {
    const uint64_t phys = kv.second->last_checkpoint_phys();
    if (phys == 0) {
      continue;
    }
    manifest_ckpt c;
    c.pool_id = std::get<0>(kv.first);
    c.pg_id = std::get<1>(kv.first);
    c.store_epoch = std::get<2>(kv.first);
    c.phys = phys;
    m.object_checkpoints.push_back(c);
  }
  for (const auto& kv : logs_) {
    const uint64_t phys = kv.second->last_checkpoint_phys();
    if (phys == 0) {
      continue;
    }
    manifest_ckpt c;
    c.pool_id = std::get<0>(kv.first);
    c.pg_id = std::get<1>(kv.first);
    c.store_epoch = std::get<2>(kv.first);
    c.phys = phys;
    m.log_checkpoints.push_back(c);
  }
  for (const auto& kv : latest_epoch_) {
    manifest_pg_entry e;
    e.pool_id = kv.first.first;
    e.pg_id = kv.first.second;
    e.active_epoch = kv.second;
    m.pgs.push_back(e);
  }
  return m;
}

void osd_store::apply_manifest_epochs(const manifest& m) {
  for (const auto& e : m.pgs) {
    auto& le = latest_epoch_[{e.pool_id, e.pg_id}];
    if (e.active_epoch > le) {
      le = e.active_epoch;
    }
  }
}

namespace {

void* meta_alloc_buf(buffer_mode mode, size_t size, size_t align) {
  if (align < 4096) {
    align = 4096;
  }
  if (mode == buffer_mode::spdk_dma) {
    return spdk_dma_zmalloc(size, align, nullptr);
  }
  void* p = nullptr;
  if (posix_memalign(&p, align, size) != 0) {
    return nullptr;
  }
  std::memset(p, 0, size);
  return p;
}

void meta_free_buf(void* p, buffer_mode mode) {
  if (p == nullptr) {
    return;
  }
  if (mode == buffer_mode::spdk_dma) {
    spdk_dma_free(p);
  } else {
    free(p);
  }
}

} // namespace

void osd_store::format(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr || data_length_ == 0) {
    cb(arg, -EINVAL);
    return;
  }

  auto* req = new (std::nothrow) meta_req();
  if (req == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }
  req->self = this;
  req->cb = std::move(cb);
  req->arg = arg;
  req->phase = 0;

  superblock sb;
  sb.generation = 1;
  sb.segment_size =
      configured_segment_size_ == 0 ? data_length_ : configured_segment_size_;
  sb.data_offset = data_offset_;
  sb.data_length = data_length_;
  sb.manifest_generation = 1;
  req->sb = sb;
  req->sb_bytes = encode_superblock(sb);

  manifest mf;
  mf.generation = 1;
  mf.data_high_water = data_offset_;
  recover_scan_from_ = data_offset_;
  mf.recover_scan_from = data_offset_;
  mf.open_segment_id = segment_allocator::kInvalidSeg;
  mf.open_cursor = 0;
  if (alloc_) {
    alloc_->export_states(&mf.segment_states);
  }
  req->mf = mf;
  req->mf_bytes = encode_manifest(mf);

  const uint32_t bs = backend_->block_size();
  constexpr uint64_t kZeroChunk = 256 * 1024;
  const uint64_t need =
      std::max({kSuperblockBytes, kManifestSlotBytes, kZeroChunk});
  req->dma = meta_alloc_buf(mode_, static_cast<size_t>(need), bs);
  req->dma_cap = need;
  if (req->dma == nullptr) {
    auto c = std::move(req->cb);
    void* a = req->arg;
    delete req;
    c(a, -ENOMEM);
    return;
  }

  clear_all_pgs();
  latest_epoch_.clear();
  if (alloc_) {
    alloc_->reset();
  }
  sb_generation_ = 1;
  manifest_generation_ = 1;
  backend_->set_write_pointer(data_offset_);

  format_continue(req);
}

void osd_store::format_continue(meta_req* req) {
  osd_store* self = req->self;
  auto fail = [&](int err) {
    meta_free_buf(req->dma, req->self ? req->self->mode_ : buffer_mode::heap);
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    if (cb) {
      cb(a, err);
    }
  };

  // phase 0: sb A, 1: sb B, 2: manifest, 3: zero data region, 4: flush
  if (req->phase == 0 || req->phase == 1) {
    const uint64_t off =
        (req->phase == 0) ? kSuperblockAOffset : kSuperblockBOffset;
    std::memset(req->dma, 0, static_cast<size_t>(kSuperblockBytes));
    std::memcpy(req->dma, req->sb_bytes.data(),
                std::min(req->sb_bytes.size(),
                         static_cast<size_t>(kSuperblockBytes)));
    self->backend_->write_at(
        off, req->dma, kSuperblockBytes,
        [](void* a, uint64_t /*phys*/, int err) {
          auto* r = static_cast<meta_req*>(a);
          if (err != 0) {
            r->self->format_continue(r); // will fail below via phase hack
            // simpler:
          }
          if (err != 0) {
            meta_free_buf(r->dma, r->self ? r->self->mode_ : buffer_mode::heap);
            auto cb = std::move(r->cb);
            void* user = r->arg;
            delete r;
            if (cb) {
              cb(user, err);
            }
            return;
          }
          ++r->phase;
          r->self->format_continue(r);
        },
        req);
    return;
  }
  if (req->phase == 2) {
    const uint64_t off = manifest_slot_offset(req->mf.generation);
    std::memset(req->dma, 0, static_cast<size_t>(kManifestSlotBytes));
    if (req->mf_bytes.size() > kManifestSlotBytes) {
      fail(-ENOSPC);
      return;
    }
    std::memcpy(req->dma, req->mf_bytes.data(), req->mf_bytes.size());
    self->backend_->write_at(
        off, req->dma, kManifestSlotBytes,
        [](void* a, uint64_t /*phys*/, int err) {
          auto* r = static_cast<meta_req*>(a);
          if (err != 0) {
            meta_free_buf(r->dma, r->self ? r->self->mode_ : buffer_mode::heap);
            auto cb = std::move(r->cb);
            void* user = r->arg;
            delete r;
            if (cb) {
              cb(user, err);
            }
            return;
          }
          ++r->phase;
          r->self->format_continue(r);
        },
        req);
    return;
  }
  if (req->phase == 3) {
    if (req->zero_end == 0) {
      req->zero_off = self->data_offset_;
      req->zero_end = self->data_offset_ + self->data_length_;
    }
    if (req->zero_off >= req->zero_end) {
      req->phase = 4;
      self->format_continue(req);
      return;
    }
    const uint32_t bs = self->backend_->block_size();
    uint64_t chunk = std::min<uint64_t>(req->dma_cap, req->zero_end - req->zero_off);
    chunk = (chunk / bs) * bs;
    if (chunk == 0) {
      req->phase = 4;
      self->format_continue(req);
      return;
    }
    std::memset(req->dma, 0, static_cast<size_t>(chunk));
    const uint64_t off = req->zero_off;
    self->backend_->write_at(
        off, req->dma, chunk,
        [chunk](void* a, uint64_t /*phys*/, int err) {
          auto* r = static_cast<meta_req*>(a);
          if (err != 0) {
            meta_free_buf(r->dma, r->self ? r->self->mode_ : buffer_mode::heap);
            auto cb = std::move(r->cb);
            void* user = r->arg;
            delete r;
            if (cb) {
              cb(user, err);
            }
            return;
          }
          r->zero_off += chunk;
          r->self->format_continue(r);
        },
        req);
    return;
  }
  // phase 4: flush, then reset append cursor to start of data region.
  self->backend_->flush(
      [](void* a, int err) {
        auto* r = static_cast<meta_req*>(a);
        if (err == 0 && r->self && r->self->backend_) {
          r->self->backend_->set_write_pointer(r->self->data_offset_);
        }
        meta_free_buf(r->dma, r->self ? r->self->mode_ : buffer_mode::heap);
        auto cb = std::move(r->cb);
        void* user = r->arg;
        delete r;
        if (cb) {
          cb(user, err);
        }
      },
      req);
}

void osd_store::persist_manifest(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr) {
    cb(arg, -EINVAL);
    return;
  }

  auto* req = new (std::nothrow) meta_req();
  if (req == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }
  req->self = this;
  req->cb = std::move(cb);
  req->arg = arg;
  req->phase = 0;

  req->mf = build_manifest();
  req->mf_bytes = encode_manifest(req->mf);
  if (req->mf_bytes.size() > kManifestSlotBytes) {
    delete req;
    cb(arg, -ENOSPC);
    return;
  }

  superblock sb;
  sb.generation = sb_generation_ + 1;
  sb.segment_size =
      configured_segment_size_ == 0 ? data_length_ : configured_segment_size_;
  sb.data_offset = data_offset_;
  sb.data_length = data_length_;
  sb.manifest_generation = req->mf.generation;
  req->sb = sb;
  req->sb_bytes = encode_superblock(sb);

  const uint32_t bs = backend_->block_size();
  const uint64_t need = std::max(kSuperblockBytes, kManifestSlotBytes);
  req->dma = meta_alloc_buf(mode_, static_cast<size_t>(need), bs);
  req->dma_cap = need;
  if (req->dma == nullptr) {
    auto c = std::move(req->cb);
    void* a = req->arg;
    delete req;
    c(a, -ENOMEM);
    return;
  }
  persist_continue(req);
}

void osd_store::persist_continue(meta_req* req) {
  osd_store* self = req->self;
  // 0: write manifest slot, 1: sb A, 2: sb B, 3: flush + commit gens
  if (req->phase == 0) {
    const uint64_t off = manifest_slot_offset(req->mf.generation);
    std::memset(req->dma, 0, static_cast<size_t>(kManifestSlotBytes));
    std::memcpy(req->dma, req->mf_bytes.data(), req->mf_bytes.size());
    self->backend_->write_at(
        off, req->dma, kManifestSlotBytes,
        [](void* a, uint64_t, int err) {
          auto* r = static_cast<meta_req*>(a);
          if (err != 0) {
            meta_free_buf(r->dma, r->self ? r->self->mode_ : buffer_mode::heap);
            auto cb = std::move(r->cb);
            void* user = r->arg;
            delete r;
            if (cb) {
              cb(user, err);
            }
            return;
          }
          ++r->phase;
          r->self->persist_continue(r);
        },
        req);
    return;
  }
  if (req->phase == 1 || req->phase == 2) {
    const uint64_t off =
        (req->phase == 1) ? kSuperblockAOffset : kSuperblockBOffset;
    std::memset(req->dma, 0, static_cast<size_t>(kSuperblockBytes));
    std::memcpy(req->dma, req->sb_bytes.data(),
                std::min(req->sb_bytes.size(),
                         static_cast<size_t>(kSuperblockBytes)));
    self->backend_->write_at(
        off, req->dma, kSuperblockBytes,
        [](void* a, uint64_t, int err) {
          auto* r = static_cast<meta_req*>(a);
          if (err != 0) {
            meta_free_buf(r->dma, r->self ? r->self->mode_ : buffer_mode::heap);
            auto cb = std::move(r->cb);
            void* user = r->arg;
            delete r;
            if (cb) {
              cb(user, err);
            }
            return;
          }
          ++r->phase;
          r->self->persist_continue(r);
        },
        req);
    return;
  }
  self->backend_->flush(
      [](void* a, int err) {
        auto* r = static_cast<meta_req*>(a);
        if (err == 0) {
          r->self->sb_generation_ = r->sb.generation;
          r->self->manifest_generation_ = r->mf.generation;
        }
        meta_free_buf(r->dma, r->self ? r->self->mode_ : buffer_mode::heap);
        auto cb = std::move(r->cb);
        void* user = r->arg;
        delete r;
        if (cb) {
          cb(user, err);
        }
      },
      req);
}

osd_store::gc_stats osd_store::gc_pass() {
  return gc_pass(gc_options{});
}

osd_store::gc_stats osd_store::gc_pass(const gc_options& opt) {
  gc_stats st;
  if (opt.rewrite_victim) {
    // gc_rewrite_once rewrites then free_dead via gc_once.
    st.extents_rewritten =
        gc_rewrite_once(opt.persist_manifest, opt.max_rewrite_extents);
  } else if (opt.free_dead_segments) {
    st.segments_freed = gc_once(opt.persist_manifest);
  }
  return st;
}

void osd_store::create_image_snapshot(uint64_t pool_id, uint64_t pg_id,
                                      uint64_t image_snap_seq, op_complete cb,
                                      void* arg) {
  if (cb == nullptr) {
    return;
  }
  pg_store* pg = get_active_pg(pool_id, pg_id);
  if (pg == nullptr) {
    cb(arg, -ENOENT);
    return;
  }
  pg->create_image_snapshot(image_snap_seq, std::move(cb), arg);
}

void osd_store::release_image_snapshot(uint64_t pool_id, uint64_t pg_id,
                                       uint64_t image_snap_seq, op_complete cb,
                                       void* arg) {
  if (cb == nullptr) {
    return;
  }
  pg_store* pg = get_active_pg(pool_id, pg_id);
  if (pg == nullptr) {
    cb(arg, -ENOENT);
    return;
  }
  pg->release_image_snapshot(image_snap_seq, std::move(cb), arg);
}

void osd_store::flatten_image(uint64_t pool_id, uint64_t pg_id,
                              uint64_t parent_image_seq,
                              uint64_t new_image_seq,
                              uint64_t raft_write_index, op_complete cb,
                              void* arg) {
  if (cb == nullptr) {
    return;
  }
  pg_store* pg = get_active_pg(pool_id, pg_id);
  if (pg == nullptr) {
    cb(arg, -ENOENT);
    return;
  }
  pg->flatten_image(parent_image_seq, new_image_seq, raft_write_index,
                    std::move(cb), arg);
}

void osd_store::apply(uint64_t pool_id, uint64_t pg_id,
                      const std::string& object_name, uint64_t offset,
                      const void* data, uint64_t len, uint64_t raft_write_index,
                      uint64_t image_snap_seq, op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (data == nullptr || object_name.empty() || raft_write_index == 0) {
    cb(arg, -EINVAL);
    return;
  }
  pg_store* pg = get_active_pg(pool_id, pg_id);
  if (pg == nullptr) {
    cb(arg, -ENOENT);
    return;
  }
  pg->write(object_name, offset, data, len, raft_write_index, image_snap_seq,
            std::move(cb), arg);
}

void osd_store::read(uint64_t pool_id, uint64_t pg_id,
                     const std::string& object_name, uint64_t offset, void* buf,
                     uint64_t len, op_complete cb, void* arg) {
  read(pool_id, pg_id, object_name, offset, buf, len, /*raft*/ 0, /*image*/ 0,
       std::move(cb), arg);
}

void osd_store::read(uint64_t pool_id, uint64_t pg_id,
                     const std::string& object_name, uint64_t offset, void* buf,
                     uint64_t len, uint64_t raft_read_index,
                     uint64_t image_read_seq, op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (buf == nullptr || object_name.empty()) {
    cb(arg, -EINVAL);
    return;
  }
  pg_store* pg = get_active_pg(pool_id, pg_id);
  if (pg == nullptr) {
    cb(arg, -ENOENT);
    return;
  }
  pg->read(object_name, offset, buf, len, raft_read_index, image_read_seq,
           std::move(cb), arg);
}

void osd_store::remove(uint64_t pool_id, uint64_t pg_id,
                       const std::string& object_name, uint64_t raft_write_index,
                       uint64_t image_snap_seq, op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (object_name.empty() || raft_write_index == 0) {
    cb(arg, -EINVAL);
    return;
  }
  pg_store* pg = get_active_pg(pool_id, pg_id);
  if (pg == nullptr) {
    cb(arg, -ENOENT);
    return;
  }
  pg->remove(object_name, raft_write_index, image_snap_seq, std::move(cb),
             arg);
}

void osd_store::create_pg(uint64_t pool_id, uint64_t pg_id, op_complete cb,
                          void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr || alloc_ == nullptr) {
    cb(arg, -EINVAL);
    return;
  }
  const uint64_t epoch = latest_epoch(pool_id, pg_id) + 1;
  ensure_pg(pool_id, pg_id, epoch);
  // First PG: pin serving to this epoch. Install staging leaves serving as-is
  // (snapshot_ingest::begin sets it to the pre-staging epoch).
  if (serving_epoch_.find({pool_id, pg_id}) == serving_epoch_.end()) {
    set_serving_epoch(pool_id, pg_id, epoch);
  }
  persist_manifest(std::move(cb), arg);
}

void osd_store::delete_pg(uint64_t pool_id, uint64_t pg_id, op_complete cb,
                          void* arg) {
  if (cb == nullptr) {
    return;
  }
  for (auto it = pgs_.begin(); it != pgs_.end();) {
    if (std::get<0>(it->first) == pool_id &&
        std::get<1>(it->first) == pg_id) {
      it = pgs_.erase(it);
    } else {
      ++it;
    }
  }
  // latest_epoch_ retained for next create_pg.
  persist_manifest(std::move(cb), arg);
}

struct osd_store::ckpt_pass_req {
  osd_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  std::vector<pg_store*> pgs;
  std::vector<log_store*> logs;
  size_t pi{0};
  size_t li{0};
  int stage{0};
};

void osd_store::checkpoint_pass(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  auto* req = new (std::nothrow) ckpt_pass_req();
  if (req == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }
  req->self = this;
  req->cb = std::move(cb);
  req->arg = arg;
  for (auto& kv : pgs_) {
    if (kv.second) {
      req->pgs.push_back(kv.second.get());
    }
  }
  for (auto& kv : logs_) {
    if (kv.second) {
      req->logs.push_back(kv.second.get());
    }
  }
  checkpoint_pass_continue(req, 0);
}

void osd_store::checkpoint_pass_continue(void* arg, int err) {
  auto* req = static_cast<ckpt_pass_req*>(arg);
  osd_store* self = req->self;
  if (err != 0) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    if (cb) {
      cb(a, err);
    }
    return;
  }
  if (req->stage == 0) {
    if (req->pi < req->pgs.size()) {
      pg_store* pg = req->pgs[req->pi++];
      pg->checkpoint(
          [](void* a, int e) {
            auto* r = static_cast<ckpt_pass_req*>(a);
            r->self->checkpoint_pass_continue(a, e);
          },
          req);
      return;
    }
    req->stage = 1;
  }
  if (req->stage == 1) {
    if (req->li < req->logs.size()) {
      log_store* lg = req->logs[req->li++];
      lg->checkpoint(
          [](void* a, int e) {
            auto* r = static_cast<ckpt_pass_req*>(a);
            r->self->checkpoint_pass_continue(a, e);
          },
          req);
      return;
    }
    req->stage = 2;
  }
  if (req->stage == 2) {
    req->stage = 3;
    if (self->kv_ != nullptr) {
      self->kv_->checkpoint(
          [](void* a, int e) {
            auto* r = static_cast<ckpt_pass_req*>(a);
            r->self->checkpoint_pass_continue(a, e);
          },
          req);
      return;
    }
  }
  auto cb = std::move(req->cb);
  void* a = req->arg;
  delete req;
  uint64_t hw = self->backend_ ? self->backend_->write_pointer() : 0;
  if (self->alloc_ != nullptr) {
    const uint32_t open = self->alloc_->open_segment();
    if (open != segment_allocator::kInvalidSeg) {
      const uint64_t cur =
          self->alloc_->segment_base(open) + self->alloc_->open_cursor();
      if (cur > hw) {
        hw = cur;
      }
    }
  }
  self->recover_scan_from_ = hw;
  self->persist_manifest(std::move(cb), a);
}

uint32_t osd_store::gc_once(bool persist) {
  if (alloc_ == nullptr) {
    return 0;
  }
  const uint32_t nseg = alloc_->segment_count();
  const uint64_t seg_size = alloc_->segment_size();
  if (nseg == 0 || seg_size == 0) {
    return 0;
  }

  // Mark segments that have any live extent reference (object + log + kv).
  std::vector<uint8_t> live(nseg, 0);
  auto mark = [&](uint64_t phys, uint64_t len) {
    if (len == 0 || phys + len <= alloc_->region_base()) {
      return;
    }
    const uint64_t rel0 =
        phys > alloc_->region_base() ? phys - alloc_->region_base() : 0;
    const uint64_t rel1 = phys + len - alloc_->region_base();
    const uint32_t first = static_cast<uint32_t>(rel0 / seg_size);
    const uint32_t last =
        rel1 == 0 ? first : static_cast<uint32_t>((rel1 - 1) / seg_size);
    for (uint32_t id = first; id <= last && id < nseg; ++id) {
      live[id] = 1;
    }
  };
  visit_live_extents(mark);

  uint32_t freed = 0;
  for (uint32_t id = 0; id < nseg; ++id) {
    if (alloc_->state(id) != segment_state::sealed) {
      continue;
    }
    if (live[id]) {
      continue;
    }
    if (alloc_->free_sealed(id) == 0) {
      ++freed;
    }
  }

  if (freed > 0 && persist) {
    // Fire-and-forget style: synchronous mem path completes in callback;
    // for async callers, pass nullptr is invalid — use a no-op stack result.
    struct {
      int err{-1};
    } r;
    persist_manifest(
        [](void* a, int err) {
          static_cast<decltype(r)*>(a)->err = err;
        },
        &r);
    (void)r;
  }
  return freed;
}

void osd_store::visit_live_extents(
    const std::function<void(uint64_t phys, uint64_t len)>& fn) const {
  if (!fn) {
    return;
  }
  for (const auto& kv : pgs_) {
    kv.second->for_each_live_extent(fn);
  }
  for (const auto& kv : logs_) {
    kv.second->for_each_live_extent(fn);
  }
  if (kv_ != nullptr) {
    kv_->for_each_live_extent(fn);
  }
}

uint32_t osd_store::gc_rewrite_once(bool persist, uint32_t max_extents) {
  if (alloc_ == nullptr) {
    return 0;
  }
  if (pgs_.empty()) {
    // No object rewrite work; still reclaim sealed segments with no log/kv refs.
    gc_once(persist);
    return 0;
  }
  const uint32_t nseg = alloc_->segment_count();
  const uint64_t seg_size = alloc_->segment_size();
  if (nseg == 0 || seg_size == 0) {
    return 0;
  }

  // Accumulate live bytes per segment for victim selection (lowest live ratio).
  std::vector<uint64_t> live_bytes(nseg, 0);
  auto mark = [&](uint64_t phys, uint64_t len) {
    if (len == 0 || phys + len <= alloc_->region_base()) {
      return;
    }
    const uint64_t rel0 =
        phys > alloc_->region_base() ? phys - alloc_->region_base() : 0;
    const uint64_t rel1 = phys + len - alloc_->region_base();
    const uint32_t first = static_cast<uint32_t>(rel0 / seg_size);
    const uint32_t last =
        rel1 == 0 ? first : static_cast<uint32_t>((rel1 - 1) / seg_size);
    for (uint32_t id = first; id <= last && id < nseg; ++id) {
      // Attribute full extent length to each overlapping segment (upper bound).
      live_bytes[id] += len;
    }
  };
  for (const auto& kv : pgs_) {
    kv.second->for_each_live_extent(mark);
  }

  // Victim = sealed segment with live data and minimal live_bytes (most garbage).
  uint32_t victim = segment_allocator::kInvalidSeg;
  uint64_t best_live = UINT64_MAX;
  for (uint32_t id = 0; id < nseg; ++id) {
    if (alloc_->state(id) != segment_state::sealed) {
      continue;
    }
    if (live_bytes[id] == 0) {
      continue;
    }
    if (live_bytes[id] < best_live) {
      best_live = live_bytes[id];
      victim = id;
    }
  }
  if (victim == segment_allocator::kInvalidSeg) {
    // No rewrite work; still free fully dead sealed segments.
    return gc_once(persist);
  }

  const uint64_t phys_begin = alloc_->segment_base(victim);
  const uint64_t phys_end = phys_begin + seg_size;

  uint32_t rewritten = 0;
  for (const auto& kv : pgs_) {
    if (max_extents != 0 && rewritten >= max_extents) {
      break;
    }
    pg_store* pg = kv.second.get();
    std::vector<std::tuple<std::string, uint64_t, uint64_t>> targets;
    pg->collect_head_extents_in_range(phys_begin, phys_end, &targets);
    for (const auto& t : targets) {
      if (max_extents != 0 && rewritten >= max_extents) {
        break;
      }
      struct {
        int err{-1};
      } r;
      pg->rewrite_extent(std::get<0>(t), std::get<1>(t), std::get<2>(t),
                         [](void* a, int err) {
                           static_cast<decltype(r)*>(a)->err = err;
                         },
                         &r);
      if (r.err == 0) {
        ++rewritten;
      }
    }
  }

  // After rewrite, free sealed segments with no remaining live refs.
  const uint32_t freed = gc_once(persist);
  (void)freed;
  return rewritten;
}

void osd_store::recover(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr) {
    cb(arg, -EINVAL);
    return;
  }

  pgs_.clear();
  logs_.clear();
  latest_epoch_.clear();
  if (kv_ != nullptr) {
    kv_->reset_for_recover();
  }
  if (alloc_) {
    alloc_->reset();
  }

  auto* req = new (std::nothrow) recover_req();
  if (req == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }
  req->self = this;
  req->cb = std::move(cb);
  req->arg = arg;
  req->phase = 0;

  const uint32_t bs = backend_->block_size();
  const uint64_t chunk =
      std::max<uint64_t>(kRecoverChunk, kManifestSlotBytes);
  req->buf_cap = chunk;
  req->buf = meta_alloc_buf(mode_, static_cast<size_t>(chunk), bs);
  if (req->buf == nullptr) {
    auto c = std::move(req->cb);
    void* a = req->arg;
    delete req;
    c(a, -ENOMEM);
    return;
  }

  // Ensure we can read metadata even if write_pointer is still 0.
  if (backend_->write_pointer() < kMetaSize) {
    backend_->set_write_pointer(kMetaSize);
  }

  // Read superblock A
  backend_->read(
      kSuperblockAOffset, req->buf, kSuperblockBytes,
      [](void* a, int err) {
        auto* r = static_cast<recover_req*>(a);
        osd_store* self = r->self;
        if (err == 0) {
          r->sb_a_ok = decode_superblock(static_cast<uint8_t*>(r->buf),
                                         static_cast<size_t>(kSuperblockBytes),
                                         &r->sb_a);
        }
        self->backend_->read(
            kSuperblockBOffset, r->buf, kSuperblockBytes,
            [](void* a2, int err2) {
              auto* r2 = static_cast<recover_req*>(a2);
              osd_store* self2 = r2->self;
              if (err2 == 0) {
                r2->sb_b_ok = decode_superblock(
                    static_cast<uint8_t*>(r2->buf),
                    static_cast<size_t>(kSuperblockBytes), &r2->sb_b);
              }
              if (!prefer_superblock(r2->sb_a, r2->sb_a_ok, r2->sb_b,
                                     r2->sb_b_ok, &r2->sb_best)) {
                // Unformatted device: treat as empty data region.
                self2->sb_generation_ = 0;
                self2->manifest_generation_ = 0;
                r2->last_good = self2->data_offset_;
                r2->last_valid_end = self2->data_offset_;
                r2->pos = self2->data_offset_;
                r2->phase = 3;
                self2->backend_->set_write_pointer(self2->data_offset_);
                self2->recover_process(r2);
                return;
              }
              self2->sb_generation_ = r2->sb_best.generation;
              self2->manifest_generation_ = r2->sb_best.manifest_generation;
              self2->data_offset_ = r2->sb_best.data_offset;
              self2->data_length_ = r2->sb_best.data_length;

              // Rebuild allocator to match superblock geometry.
              const uint32_t bs2 = self2->backend_->block_size();
              const uint64_t ss = r2->sb_best.segment_size == 0
                                      ? r2->sb_best.data_length
                                      : r2->sb_best.segment_size;
              self2->alloc_ = std::make_shared<segment_allocator>(
                  self2->data_offset_, self2->data_length_, ss, bs2);

              // Read preferred manifest slot (and fallback other).
              const uint64_t mgen = r2->sb_best.manifest_generation;
              const uint64_t moff = manifest_slot_offset(mgen);
              self2->backend_->read(
                  moff, r2->buf, kManifestSlotBytes,
                  [](void* a3, int err3) {
                    auto* r3 = static_cast<recover_req*>(a3);
                    osd_store* self3 = r3->self;
                    if (err3 == 0) {
                      r3->mf_ok = decode_manifest(
                          static_cast<uint8_t*>(r3->buf),
                          static_cast<size_t>(kManifestSlotBytes), &r3->mf);
                    }
                    if (!r3->mf_ok && r3->sb_best.manifest_generation > 0) {
                      // try other slot
                      const uint64_t other = manifest_slot_offset(
                          r3->sb_best.manifest_generation + 1);
                      self3->backend_->read(
                          other, r3->buf, kManifestSlotBytes,
                          [](void* a4, int err4) {
                            auto* r4 = static_cast<recover_req*>(a4);
                            if (err4 == 0) {
                              r4->mf_ok = decode_manifest(
                                  static_cast<uint8_t*>(r4->buf),
                                  static_cast<size_t>(kManifestSlotBytes),
                                  &r4->mf);
                            }
                            r4->self->recover_after_meta(r4);
                          },
                          r3);
                      return;
                    }
                    self3->recover_after_meta(r3);
                  },
                  r2);
            },
            r);
      },
      req);
}

void osd_store::apply_manifest_allocator(const manifest& m) {
  if (alloc_ == nullptr || m.segment_states.empty()) {
    return;
  }
  if (alloc_->import_states(m.segment_states.data(),
                            static_cast<uint32_t>(m.segment_states.size()),
                            m.open_segment_id, m.open_cursor)) {
    SPDK_NOTICELOG(
        "aos recover imported segment table n=%zu open=%u cursor=%lu free=%u "
        "sealed=%u\n",
        m.segment_states.size(), m.open_segment_id, m.open_cursor,
        alloc_->free_count(), alloc_->sealed_count());
  }
}

void osd_store::recover_after_meta(recover_req* req) {
  if (req->mf_ok) {
    manifest_generation_ = req->mf.generation;
    apply_manifest_epochs(req->mf);
    recover_scan_from_ = req->mf.recover_scan_from;
    if (!req->mf.segment_states.empty()) {
      apply_manifest_allocator(req->mf);
      bool any_used = false;
      for (uint8_t s : req->mf.segment_states) {
        if (s != static_cast<uint8_t>(segment_state::free)) {
          any_used = true;
          break;
        }
      }
      // Skip free segments only when a checkpoint covers the prefix
      // (recover_scan_from > data_offset). Otherwise object_name records
      // on a just-freed segment are still required to rebuild the map.
      req->have_seg_table =
          any_used && req->mf.recover_scan_from > data_offset_;
    }
  }
  const uint64_t data_end = data_offset_ + data_length_;
  uint64_t scan_end =
      std::min(backend_ ? backend_->capacity() : data_end, data_end);
  uint64_t scan_from = data_offset_;
  if (req->mf_ok && req->mf.data_high_water > data_offset_) {
    const uint64_t slack = alloc_ && alloc_->segment_size() > 0
                               ? std::min(alloc_->segment_size(),
                                          static_cast<uint64_t>(1024 * 1024))
                               : 1024ull * 1024;
    scan_end = std::min(scan_end, req->mf.data_high_water + slack);
  }
  if (req->mf_ok && req->mf.recover_scan_from >= data_offset_ &&
      req->mf.recover_scan_from <= scan_end) {
    scan_from = req->mf.recover_scan_from;
  }
  req->scan_end = std::max(scan_end, data_offset_);
  backend_->set_write_pointer(req->scan_end);

  req->pos = scan_from;
  req->last_good = scan_from;
  req->last_valid_end = scan_from;
  req->pending.clear();
  req->phase = 3;

  if (req->mf_ok) {
    for (const auto& c : req->mf.object_checkpoints) {
      if (c.phys != 0) {
        req->ckpt_queue.push_back(c.phys);
      }
    }
    for (const auto& c : req->mf.log_checkpoints) {
      if (c.phys != 0) {
        req->ckpt_queue.push_back(c.phys);
      }
    }
    if (req->mf.kv_checkpoint_phys != 0) {
      req->ckpt_queue.push_back(req->mf.kv_checkpoint_phys);
    }
  }

  SPDK_NOTICELOG(
      "aos recover scan start data_off=%lu data_len=%lu scan_from=%lu "
      "scan_end=%lu cap=%lu wp=%lu bs=%u mf_hw=%lu ckpts=%zu segs=%d\n",
      data_offset_, data_length_, scan_from, req->scan_end,
      backend_ ? backend_->capacity() : 0,
      backend_ ? backend_->write_pointer() : 0,
      backend_ ? backend_->block_size() : 0,
      req->mf_ok ? req->mf.data_high_water : 0, req->ckpt_queue.size(),
      req->have_seg_table ? 1 : 0);
  if (!req->ckpt_queue.empty()) {
    recover_load_checkpoints(req);
    return;
  }
  recover_process(req);
}

void osd_store::recover_load_checkpoints(recover_req* req) {
  if (req->ckpt_idx >= req->ckpt_queue.size()) {
    recover_process(req);
    return;
  }
  const uint64_t phys = req->ckpt_queue[req->ckpt_idx];
  const uint32_t bs = backend_->block_size();
  uint64_t aligned = phys - (phys % bs);
  uint64_t to_read = std::min(req->buf_cap, backend_->capacity() - aligned);
  to_read = (to_read / bs) * bs;
  if (to_read < kRecordHeaderLenV1) {
    ++req->ckpt_idx;
    recover_load_checkpoints(req);
    return;
  }
  backend_->read(
      aligned, req->buf, to_read,
      [req, phys, aligned](void* /*a*/, int err) {
        osd_store* self = req->self;
        if (err != 0) {
          SPDK_NOTICELOG("aos recover ckpt read phys=%lu err=%d, skip\n", phys,
                         err);
          ++req->ckpt_idx;
          self->recover_load_checkpoints(req);
          return;
        }
        const auto* p = static_cast<const uint8_t*>(req->buf);
        const size_t skip = static_cast<size_t>(phys - aligned);
        data_record rec;
        size_t consumed = 0;
        const decode_status st =
            decode_record(p + skip, static_cast<size_t>(req->buf_cap) - skip,
                          &rec, &consumed);
        if (st != decode_status::ok) {
          SPDK_NOTICELOG("aos recover ckpt decode phys=%lu st=%d, skip\n", phys,
                         static_cast<int>(st));
          ++req->ckpt_idx;
          self->recover_load_checkpoints(req);
          return;
        }
        if (rec.type == record_type::checkpoint_marker) {
          checkpoint_snapshot snap;
          if (decode_checkpoint_payload(rec.payload, &snap)) {
            pg_store* pg =
                self->ensure_pg(rec.pool_id, rec.pg_id, rec.store_epoch);
            if (pg) {
              pg->apply_checkpoint_snapshot(snap, phys + consumed);
            }
            ++req->applied_data;
          }
        } else if (rec.type == record_type::log_checkpoint) {
          log_store* lg =
              self->get_or_create_log(rec.pool_id, rec.pg_id, rec.store_epoch);
          if (lg) {
            lg->apply_checkpoint_record(rec);
          }
          ++req->applied_log;
        } else if (rec.type == record_type::kv_checkpoint &&
                   self->kv_ != nullptr) {
          self->kv_->apply_record(rec, phys, 0);
          ++req->applied_kv;
        }
        ++req->applied;
        ++req->ckpt_idx;
        self->recover_load_checkpoints(req);
      },
      req);
}

void osd_store::recover_process(recover_req* req) {
  const uint32_t bs = backend_->block_size();
  const uint64_t seg_size = alloc_ ? alloc_->segment_size() : 0;
  const uint64_t region_base = alloc_ ? alloc_->region_base() : data_offset_;
  const uint64_t cap = backend_->capacity();
  const uint64_t data_end = data_offset_ + data_length_;

  auto finish_ok = [&]() {
    uint64_t wp = req->last_valid_end;
    if (req->mf_ok && req->mf.data_high_water > wp) {
      wp = req->mf.data_high_water;
    }
    backend_->set_write_pointer(wp);
    if (alloc_ && !req->have_seg_table) {
      alloc_->set_open_cursor_abs(req->last_valid_end);
    } else if (alloc_ && req->have_seg_table) {
      alloc_->set_open_cursor_abs(req->last_valid_end);
    }
    // Install recovery: prefer activated staging as serving; drop incomplete.
    for (const auto& kv : pgs_) {
      pg_store* s = kv.second.get();
      if (s != nullptr && s->install_activated()) {
        set_serving_epoch(std::get<0>(kv.first), std::get<1>(kv.first),
                          std::get<2>(kv.first));
      }
    }
    drop_unsealed_staging();
    retarget_serving_to_recovered();

    SPDK_NOTICELOG(
        "aos recover scan done last_valid_end=%lu applied=%u data=%u log=%u "
        "kv=%u skipped=%u pgs=%zu first_decode=%d first_magic=0x%x kv_keys=%s\n",
        req->last_valid_end, req->applied, req->applied_data, req->applied_log,
        req->applied_kv, req->skipped, pgs_.size(), req->first_decode_st,
        req->first_magic, req->kv_keys[0] ? req->kv_keys : "-");

    // If data scan advanced past the stale manifest high_water, snapshot a
    // fresh manifest so the next recover can bound the scan sooner.
    const uint64_t mf_hw =
        req->mf_ok ? req->mf.data_high_water : 0;
    const uint64_t last_good = req->last_valid_end;
    const bool need_hw_snap = last_good > mf_hw;

    meta_free_buf(req->buf, mode_);
    auto cb = std::move(req->cb);
    void* a = req->arg;
    osd_store* self = req->self;
    delete req;

    if (need_hw_snap && self != nullptr) {
      struct hw_snap {
        op_complete cb;
        void* arg{nullptr};
      };
      auto* snap = new (std::nothrow) hw_snap();
      if (snap == nullptr) {
        if (cb) {
          cb(a, 0);
        }
        return;
      }
      snap->cb = std::move(cb);
      snap->arg = a;
      self->persist_manifest(
          [](void* arg, int /*err*/) {
            auto* s = static_cast<hw_snap*>(arg);
            auto c = std::move(s->cb);
            void* u = s->arg;
            delete s;
            // Recover already rebuilt memory state; high_water snap failure
            // is non-fatal (next boot still full-scans).
            if (c) {
              c(u, 0);
            }
          },
          snap);
      return;
    }
    if (cb) {
      cb(a, 0);
    }
  };

  auto skip_to_next_segment = [&]() -> bool {
    if (seg_size == 0 || alloc_ == nullptr) {
      return false;
    }
    if (req->last_good < region_base) {
      return false;
    }
    const uint64_t rel = req->last_good - region_base;
    uint64_t target_rel;
    if (rel % seg_size == 0) {
      target_rel = rel + seg_size;
    } else {
      target_rel = ((rel / seg_size) + 1) * seg_size;
    }
    const uint64_t target = region_base + target_rel;
    if (target >= data_end || target >= backend_->write_pointer()) {
      return false;
    }
    // Advance decode/read cursors only. last_valid_end stays so empty
    // prefix/hole segments are not sealed (empty-disk ENOSPC).
    req->last_good = target;
    req->pos = target;
    req->pending.clear();
    return true;
  };

  // Decode failure: resync one block later inside the current segment
  // (padding / 0x55 DMA junk). A long zero streak is an unwritten tail
  // or GC hole — jump to the next segment without sealing the unused
  // range (empty-disk ENOSPC).
  constexpr uint32_t kZeroSkipBytes = 16 * 1024;
  while (!req->pending.empty()) {
    data_record rec;
    size_t consumed = 0;
    const decode_status st = decode_record(
        req->pending.data(), req->pending.size(), &rec, &consumed);
    if (req->first_decode_st < 0) {
      req->first_decode_st = static_cast<int>(st);
      if (!req->pending.empty()) {
        req->first_magic = static_cast<uint32_t>(req->pending[0]) |
                           (static_cast<uint32_t>(req->pending.size() > 1
                                                      ? req->pending[1]
                                                      : 0)
                            << 8) |
                           (static_cast<uint32_t>(req->pending.size() > 2
                                                      ? req->pending[2]
                                                      : 0)
                            << 16) |
                           (static_cast<uint32_t>(req->pending.size() > 3
                                                      ? req->pending[3]
                                                      : 0)
                            << 24);
      }
    }
    if (st == decode_status::need_more) {
      break;
    }
    if (st != decode_status::ok) {
      uint64_t seg_end = data_end;
      if (seg_size > 0 && req->last_good >= region_base) {
        const uint64_t rel = req->last_good - region_base;
        seg_end = region_base + ((rel / seg_size) + 1) * seg_size;
      }
      // Torn payload: header CRC passed, consumed is the full record.
      // Skip the whole aligned record so we do not walk through its body.
      if (st == decode_status::bad_payload_checksum && consumed > 0 &&
          bs > 0) {
        const uint64_t skip =
            ((static_cast<uint64_t>(consumed) + bs - 1) / bs) * bs;
        if (skip >= bs && req->pending.size() >= skip &&
            req->last_good + skip <= seg_end) {
          if (req->skipped < 8) {
            SPDK_NOTICELOG(
                "aos recover skip torn rec off=%lu st=%d consumed=%zu skip=%lu\n",
                req->last_good, static_cast<int>(st), consumed, skip);
          }
          req->last_good += skip;
          req->pending.erase(req->pending.begin(),
                             req->pending.begin() +
                                 static_cast<std::ptrdiff_t>(skip));
          ++req->skipped;
          req->zero_streak = 0;
          continue;
        }
      }
      if (bs > 0 && req->pending.size() >= bs &&
          req->last_good + bs <= seg_end) {
        bool block_zero = true;
        for (uint32_t i = 0; i < bs; ++i) {
          if (req->pending[static_cast<size_t>(i)] != 0) {
            block_zero = false;
            break;
          }
        }
        if (block_zero) {
          req->zero_streak += bs;
          if (req->zero_streak >= kZeroSkipBytes) {
            req->zero_streak = 0;
            if (skip_to_next_segment()) {
              recover_process(req);
              return;
            }
            finish_ok();
            return;
          }
        } else {
          req->zero_streak = 0;
        }
        if (req->skipped < 8) {
          SPDK_NOTICELOG(
              "aos recover skip block off=%lu st=%d magic=0x%x\n",
              req->last_good, static_cast<int>(st),
              req->pending.empty()
                  ? 0
                  : (static_cast<uint32_t>(req->pending[0]) |
                     (static_cast<uint32_t>(
                          req->pending.size() > 3 ? req->pending[3] : 0)
                      << 24)));
        }
        req->last_good += bs;
        req->pending.erase(req->pending.begin(),
                           req->pending.begin() +
                               static_cast<std::ptrdiff_t>(bs));
        ++req->skipped;
        continue;
      }
      req->zero_streak = 0;
      if (skip_to_next_segment()) {
        recover_process(req);
        return;
      }
      finish_ok();
      return;
    }
    req->zero_streak = 0;
    const uint64_t aligned =
        ((static_cast<uint64_t>(consumed) + bs - 1) / bs) * bs;
    if (req->pending.size() < aligned) {
      break;
    }
    if (seg_size > 0 && req->last_good >= region_base) {
      const uint64_t rel = req->last_good - region_base;
      const uint64_t seg_end = region_base + ((rel / seg_size) + 1) * seg_size;
      if (req->last_good + aligned > seg_end) {
        if (skip_to_next_segment()) {
          recover_process(req);
          return;
        }
        finish_ok();
        return;
      }
    }

    const uint64_t rec_phys = req->last_good;
    if (req->applied == 0) {
      SPDK_NOTICELOG(
          "aos recover first rec off=%lu type=%u pool=%lu pg=%lu epoch=%lu "
          "payload=%zu\n",
          rec_phys, static_cast<unsigned>(rec.type), rec.pool_id, rec.pg_id,
          rec.store_epoch, rec.payload.size());
    }
    if (rec.type == record_type::raft_log_entry) {
      log_store* lg =
          get_or_create_log(rec.pool_id, rec.pg_id, rec.store_epoch);
      if (lg != nullptr) {
        lg->apply_record(rec, rec_phys);
      }
      ++req->applied_log;
    } else if (rec.type == record_type::kv_put ||
               rec.type == record_type::kv_delete ||
               rec.type == record_type::kv_checkpoint) {
      int kv_rc = 0;
      if (kv_ != nullptr) {
        kv_rc = kv_->apply_record(rec, rec_phys, aligned);
      }
      if (req->applied_kv < 8) {
        SPDK_NOTICELOG(
            "aos recover kv rec off=%lu type=%u rc=%d payload=%zu\n", rec_phys,
            static_cast<unsigned>(rec.type), kv_rc, rec.payload.size());
      }
      ++req->applied_kv;
      if (rec.type == record_type::kv_put && rec.payload.size() >= 8 &&
          req->kv_keys_n < 8) {
        const auto* p =
            reinterpret_cast<const uint8_t*>(rec.payload.data());
        const uint32_t klen = static_cast<uint32_t>(p[0]) |
                              (static_cast<uint32_t>(p[1]) << 8) |
                              (static_cast<uint32_t>(p[2]) << 16) |
                              (static_cast<uint32_t>(p[3]) << 24);
        if (klen > 0 && 4ull + klen <= rec.payload.size()) {
          const size_t used = std::strlen(req->kv_keys);
          if (used + 1 + klen < sizeof(req->kv_keys)) {
            if (used > 0) {
              req->kv_keys[used] = ',';
            }
            std::memcpy(req->kv_keys + (used > 0 ? used + 1 : 0),
                        rec.payload.data() + 4, klen);
            req->kv_keys_n++;
          }
        }
      }
    } else if (rec.type == record_type::log_checkpoint) {
      log_store* lg =
          get_or_create_log(rec.pool_id, rec.pg_id, rec.store_epoch);
      if (lg != nullptr) {
        lg->apply_checkpoint_record(rec);
      }
      ++req->applied_log;
    } else if (rec.type != record_type::segment_seal) {
      pg_store* pg = ensure_pg(rec.pool_id, rec.pg_id, rec.store_epoch);
      if (rec.type == record_type::checkpoint_marker) {
        checkpoint_snapshot snap;
        if (decode_checkpoint_payload(rec.payload, &snap)) {
          pg->apply_checkpoint_snapshot(snap, rec_phys + aligned);
        }
      } else {
        pg->apply_record(rec, rec_phys);
      }
      ++req->applied_data;
    }
    ++req->applied;
    if (alloc_) {
      alloc_->note_written(rec_phys, aligned);
    }
    req->last_good += aligned;
    req->last_valid_end = req->last_good;
    req->pending.erase(req->pending.begin(),
                       req->pending.begin() +
                           static_cast<std::ptrdiff_t>(aligned));
  }

  if (req->have_seg_table && alloc_ != nullptr) {
    const uint64_t bound = std::min(
        {cap, data_end,
         req->scan_end != 0 ? req->scan_end : backend_->write_pointer()});
    while (req->pos < bound) {
      const uint32_t id = alloc_->segment_id_of(req->pos);
      if (id == segment_allocator::kInvalidSeg || !alloc_->is_free(id)) {
        break;
      }
      const uint64_t next =
          alloc_->segment_base(id) + alloc_->segment_size();
      req->pos = next;
      req->last_good = next;
      req->pending.clear();
      ++req->skipped;
    }
  }

  const uint64_t scan_limit = std::min(
      {cap, data_end,
       req->scan_end != 0 ? req->scan_end : backend_->write_pointer()});
  if (req->pos >= scan_limit) {
    finish_ok();
    return;
  }

  uint64_t to_read = std::min(req->buf_cap, scan_limit - req->pos);
  to_read = (to_read / bs) * bs;
  if (to_read == 0) {
    finish_ok();
    return;
  }

  const uint64_t read_off = req->pos;
  backend_->read(
      read_off, req->buf, to_read,
      [req, to_read, read_off](void* /*a*/, int rerr) {
        osd_store* self = req->self;
        if (rerr != 0) {
          if (rerr == -ERANGE) {
            self->backend_->set_write_pointer(req->last_valid_end);
            if (self->alloc_) {
              self->alloc_->set_open_cursor_abs(req->last_valid_end);
            }
            meta_free_buf(req->buf, self->mode_);
            auto cb = std::move(req->cb);
            void* user = req->arg;
            delete req;
            if (cb) {
              cb(user, 0);
            }
            return;
          }
          meta_free_buf(req->buf, self->mode_);
          auto cb = std::move(req->cb);
          void* user = req->arg;
          delete req;
          if (cb) {
            cb(user, rerr);
          }
          return;
        }
        const auto* p = static_cast<const uint8_t*>(req->buf);
        req->pending.insert(req->pending.end(), p, p + to_read);
        req->pos = read_off + to_read;
        self->recover_process(req);
      },
      req);
}

} // namespace fastblock::aos
