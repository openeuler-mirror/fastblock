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
#include "record.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace fastblock::aos {
namespace {
constexpr uint64_t kRecoverChunk = 256 * 1024;
}

struct osd_store::recover_req {
  osd_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  uint64_t pos{0};
  uint64_t last_good{0};
  void* buf{nullptr};
  uint64_t buf_cap{0};
  std::vector<uint8_t> pending;
};

osd_store::osd_store(device_backend* backend, buffer_mode mode,
                     uint64_t segment_size)
    : backend_(backend), mode_(mode) {
  const uint64_t cap = backend ? backend->capacity() : 0;
  const uint32_t bs = backend ? backend->block_size() : 4096;
  const uint64_t ss = segment_size == 0 ? cap : segment_size;
  alloc_ = std::make_shared<segment_allocator>(cap, ss, bs);
}

uint64_t osd_store::latest_epoch(uint64_t pool_id, uint64_t pg_id) const {
  auto it = latest_epoch_.find({pool_id, pg_id});
  if (it == latest_epoch_.end()) {
    return 0;
  }
  return it->second;
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

pg_store* osd_store::ensure_pg(uint64_t pool_id, uint64_t pgid,
                               uint64_t epoch) {
  auto key = pg_key{pool_id, pgid, epoch};
  auto it = pgs_.find(key);
  if (it != pgs_.end()) {
    return it->second.get();
  }
  const fastblock::aos::pg_id pid{pool_id, pgid, epoch};
  auto store =
      std::make_unique<pg_store>(backend_, pid, alloc_, mode_);
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
  latest_epoch_.clear();
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
  cb(arg, 0);
}

void osd_store::delete_pg(uint64_t pool_id, uint64_t pg_id, op_complete cb,
                          void* arg) {
  if (cb == nullptr) {
    return;
  }
  // Erase all epochs for this PG.
  for (auto it = pgs_.begin(); it != pgs_.end();) {
    if (std::get<0>(it->first) == pool_id &&
        std::get<1>(it->first) == pg_id) {
      it = pgs_.erase(it);
    } else {
      ++it;
    }
  }
  // Keep latest_epoch so recreate gets a higher epoch (design §4.2).
  cb(arg, 0);
}

void osd_store::recover(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr) {
    cb(arg, -EINVAL);
    return;
  }

  clear_all_pgs();
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
  req->pos = 0;
  req->last_good = 0;

  const uint32_t bs = backend_->block_size();
  const uint64_t chunk = std::max<uint64_t>(kRecoverChunk, bs);
  req->buf_cap = chunk;

  // Use heap buffer for recover staging (osd_store is mode-aware via pg).
  void* buf = nullptr;
  if (mode_ == buffer_mode::spdk_dma) {
    // Lazy: still heap for M2 skeleton; spdk path uses pg_store DMA for IO.
    // recover reads via backend which accepts any buffer for mem; for spdk
    // need DMA — allocate with posix for now only if heap mode tests.
  }
  if (posix_memalign(&buf, bs, static_cast<size_t>(chunk)) != 0) {
    delete req;
    cb(arg, -ENOMEM);
    return;
  }
  std::memset(buf, 0, static_cast<size_t>(chunk));
  req->buf = buf;

  recover_process(req);
}

void osd_store::recover_process(recover_req* req) {
  const uint32_t bs = backend_->block_size();
  const uint64_t seg_size = alloc_ ? alloc_->segment_size() : 0;
  const uint64_t cap = backend_->capacity();

  auto finish_ok = [&]() {
    backend_->set_write_pointer(req->last_good);
    if (alloc_) {
      alloc_->set_open_cursor_abs(req->last_good);
    }
    free(req->buf);
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    if (cb) {
      cb(a, 0);
    }
  };

  auto skip_to_next_segment = [&]() -> bool {
    if (seg_size == 0 || seg_size >= cap) {
      return false;
    }
    uint64_t target;
    if (req->last_good % seg_size == 0) {
      target = req->last_good + seg_size;
    } else {
      target = ((req->last_good / seg_size) + 1) * seg_size;
    }
    if (target >= cap || target >= backend_->write_pointer()) {
      return false;
    }
    req->last_good = target;
    req->pos = target;
    req->pending.clear();
    return true;
  };

  while (!req->pending.empty()) {
    data_record rec;
    size_t consumed = 0;
    const decode_status st = decode_record(
        req->pending.data(), req->pending.size(), &rec, &consumed);
    if (st == decode_status::need_more) {
      break;
    }
    if (st != decode_status::ok) {
      if (skip_to_next_segment()) {
        recover_process(req);
        return;
      }
      finish_ok();
      return;
    }
    const uint64_t aligned =
        ((static_cast<uint64_t>(consumed) + bs - 1) / bs) * bs;
    if (req->pending.size() < aligned) {
      break;
    }
    if (seg_size > 0 && seg_size < cap) {
      const uint64_t seg_end =
          ((req->last_good / seg_size) + 1) * seg_size;
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
    if (rec.type != record_type::segment_seal) {
      pg_store* pg =
          ensure_pg(rec.pool_id, rec.pg_id, rec.store_epoch);
      if (rec.type == record_type::checkpoint_marker) {
        checkpoint_snapshot snap;
        if (decode_checkpoint_payload(rec.payload, &snap)) {
          pg->apply_checkpoint_snapshot(snap, rec_phys + aligned);
        }
      } else {
        pg->apply_record(rec, rec_phys);
      }
    }
    if (alloc_) {
      alloc_->note_written(rec_phys, aligned);
    }
    req->last_good += aligned;
    req->pending.erase(req->pending.begin(),
                       req->pending.begin() +
                           static_cast<std::ptrdiff_t>(aligned));
  }

  if (req->pos >= cap) {
    finish_ok();
    return;
  }

  uint64_t to_read = std::min(req->buf_cap, cap - req->pos);
  to_read = (to_read / bs) * bs;
  if (to_read == 0) {
    finish_ok();
    return;
  }
  if (req->pos + to_read > backend_->write_pointer() &&
      backend_->write_pointer() > req->pos) {
    to_read = backend_->write_pointer() - req->pos;
    to_read = (to_read / bs) * bs;
    if (to_read == 0) {
      finish_ok();
      return;
    }
  }

  const uint64_t read_off = req->pos;
  backend_->read(
      read_off, req->buf, to_read,
      [req, to_read, read_off](void* /*a*/, int rerr) {
        osd_store* self = req->self;
        if (rerr != 0) {
          if (rerr == -ERANGE) {
            self->backend_->set_write_pointer(req->last_good);
            if (self->alloc_) {
              self->alloc_->set_open_cursor_abs(req->last_good);
            }
            free(req->buf);
            auto cb = std::move(req->cb);
            void* user = req->arg;
            delete req;
            if (cb) {
              cb(user, 0);
            }
            return;
          }
          free(req->buf);
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
