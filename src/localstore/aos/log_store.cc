/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "log_store.h"
#include "record.h"

#include <spdk/env.h>

#include <cerrno>
#include <cstring>
#include <new>

namespace fastblock::aos {
namespace {

void put_u32(std::string& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    o.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
}

void put_u64(std::string& o, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    o.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
}

uint32_t get_u32(const char* p) {
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

uint64_t get_u64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  }
  return v;
}

} // namespace

std::string log_store::encode_payload(const log_entry& e) {
  std::string o;
  o.reserve(8 + 8 + 8 + 4 + e.meta.size() + e.data.size());
  put_u64(o, e.term);
  put_u64(o, e.index);
  put_u64(o, e.type);
  put_u32(o, static_cast<uint32_t>(e.meta.size()));
  o.append(e.meta);
  o.append(e.data);
  return o;
}

bool log_store::decode_payload(const std::string& payload, log_entry* out) {
  if (out == nullptr || payload.size() < 8 + 8 + 8 + 4) {
    return false;
  }
  const char* p = payload.data();
  out->term = get_u64(p);
  out->index = get_u64(p + 8);
  out->type = get_u64(p + 16);
  const uint32_t meta_len = get_u32(p + 24);
  if (payload.size() < 28ull + meta_len) {
    return false;
  }
  out->meta.assign(p + 28, meta_len);
  out->data.assign(p + 28 + meta_len, payload.size() - 28ull - meta_len);
  return true;
}

log_store::log_store(device_backend* backend, pg_id id,
                     std::shared_ptr<segment_allocator> alloc, buffer_mode mode)
    : backend_(backend)
    , id_(id)
    , mode_(mode)
    , alloc_(std::move(alloc))
    , writer_(backend, alloc_.get()) {}

void* log_store::alloc_buf(uint64_t n) {
  if (mode_ == buffer_mode::spdk_dma) {
    return spdk_dma_zmalloc(static_cast<size_t>(n), 0x1000, nullptr);
  }
  return std::calloc(1, static_cast<size_t>(n));
}

void log_store::free_buf(void* p) {
  if (p == nullptr) {
    return;
  }
  if (mode_ == buffer_mode::spdk_dma) {
    spdk_dma_free(p);
  } else {
    std::free(p);
  }
}

void log_store::append(const log_entry& entry, op_complete cb, void* arg) {
  if (backend_ == nullptr || !cb) {
    if (cb) {
      cb(arg, -EINVAL);
    }
    return;
  }
  if (entry.index == 0) {
    cb(arg, -EINVAL);
    return;
  }
  if (!index_map_.empty() && entry.index != last_index_ + 1) {
    cb(arg, -EINVAL);
    return;
  }
  if (index_map_.empty() && last_index_ != 0) {
    cb(arg, -EINVAL);
    return;
  }

  data_record rec;
  rec.type = record_type::raft_log_entry;
  rec.pool_id = id_.pool_id;
  rec.pg_id = id_.pg_id;
  rec.store_epoch = id_.store_epoch;
  rec.raft_apply_index = entry.index;
  rec.image_snap_seq = entry.term;
  rec.payload = encode_payload(entry);

  auto encoded = encode_record(rec);
  const uint64_t aligned = writer_.align_up(encoded.size());
  void* dma = alloc_buf(aligned);
  if (dma == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }
  std::memcpy(dma, encoded.data(), encoded.size());

  struct ctx {
    log_store* self;
    log_entry entry;
    void* dma;
    uint64_t aligned;
    op_complete cb;
    void* arg;
    uint64_t phys{0};
  };
  auto* c = new ctx{this, entry, dma, aligned, std::move(cb), arg, 0};

  writer_.append_record(
      rec, dma, aligned,
      [](void* a, uint64_t phys, int err) {
        auto* c = static_cast<ctx*>(a);
        if (err != 0) {
          c->self->free_buf(c->dma);
          c->cb(c->arg, err);
          delete c;
          return;
        }
        c->phys = phys;
        c->self->writer_.flush(
            [](void* a2, int ferr) {
              auto* c2 = static_cast<ctx*>(a2);
              if (ferr == 0) {
                log_store* self = c2->self;
                self->index_map_[c2->entry.index] =
                    idx_pos{c2->phys, c2->aligned, c2->entry.term};
                if (self->index_map_.size() == 1) {
                  self->base_index_ = c2->entry.index;
                }
                self->last_index_ = c2->entry.index;
              }
              c2->self->free_buf(c2->dma);
              c2->cb(c2->arg, ferr);
              delete c2;
            },
            c);
      },
      c);
}

void log_store::read(uint64_t start_index, uint64_t end_index, read_complete cb,
                     void* arg) {
  if (!cb) {
    return;
  }
  if (end_index < start_index || start_index == 0) {
    cb(arg, {}, -EINVAL);
    return;
  }
  for (uint64_t i = start_index; i <= end_index; ++i) {
    if (index_map_.find(i) == index_map_.end()) {
      cb(arg, {}, -ENOENT);
      return;
    }
  }

  // Sequential physical read if contiguous; else per-entry.
  struct rctx {
    log_store* self;
    uint64_t start;
    uint64_t end;
    uint64_t cur;
    std::vector<log_entry> out;
    read_complete cb;
    void* arg;
    void* buf{nullptr};
    uint64_t buf_len{0};
  };
  auto* c = new rctx{this, start_index, end_index, start_index, {}, std::move(cb),
                     arg, nullptr, 0};

  auto finish = [](rctx* c, int err) {
    if (c->buf) {
      c->self->free_buf(c->buf);
    }
    c->cb(c->arg, std::move(c->out), err);
    delete c;
  };

  std::function<void(rctx*)>* step = new std::function<void(rctx*)>;
  *step = [step, finish](rctx* c) {
    if (c->cur > c->end) {
      delete step;
      finish(c, 0);
      return;
    }
    const auto& pos = c->self->index_map_[c->cur];
    if (c->buf && c->buf_len < pos.length) {
      c->self->free_buf(c->buf);
      c->buf = nullptr;
    }
    if (c->buf == nullptr) {
      c->buf = c->self->alloc_buf(pos.length);
      c->buf_len = pos.length;
      if (c->buf == nullptr) {
        delete step;
        finish(c, -ENOMEM);
        return;
      }
    }
    c->self->backend_->read(
        pos.phys_offset, c->buf, pos.length,
        [step, finish, pos](void* a, int err) {
          auto* c = static_cast<rctx*>(a);
          if (err != 0) {
            delete step;
            finish(c, err);
            return;
          }
          data_record rec;
          size_t consumed = 0;
          auto st = decode_record(static_cast<const uint8_t*>(c->buf),
                                  static_cast<size_t>(pos.length), &rec,
                                  &consumed);
          if (st != decode_status::ok ||
              rec.type != record_type::raft_log_entry) {
            delete step;
            finish(c, -EIO);
            return;
          }
          log_entry e;
          if (!decode_payload(rec.payload, &e) || e.index != c->cur) {
            delete step;
            finish(c, -EIO);
            return;
          }
          c->out.push_back(std::move(e));
          ++c->cur;
          (*step)(c);
        },
        c);
  };
  (*step)(c);
}

void log_store::append_batch(std::vector<log_entry> entries, op_complete cb,
                             void* arg) {
  if (!cb) {
    return;
  }
  if (entries.empty()) {
    cb(arg, 0);
    return;
  }
  struct bctx {
    log_store* self;
    std::vector<log_entry> entries;
    size_t idx{0};
    op_complete cb;
    void* arg;
  };
  auto* c = new bctx{this, std::move(entries), 0, std::move(cb), arg};
  struct holder {
    std::function<void(bctx*)> go;
  };
  auto* h = new holder;
  h->go = [h](bctx* c) {
    if (c->idx >= c->entries.size()) {
      c->cb(c->arg, 0);
      delete h;
      delete c;
      return;
    }
    c->self->append(
        c->entries[c->idx],
        [](void* a, int err) {
          auto* pack = static_cast<std::pair<holder*, bctx*>*>(a);
          holder* hh = pack->first;
          bctx* cc = pack->second;
          delete pack;
          if (err != 0) {
            cc->cb(cc->arg, err);
            delete hh;
            delete cc;
            return;
          }
          ++cc->idx;
          hh->go(cc);
        },
        new std::pair<holder*, bctx*>(h, c));
  };
  h->go(c);
}

uint64_t log_store::get_term(uint64_t index) const {
  auto it = index_map_.find(index);
  if (it == index_map_.end()) {
    return 0;
  }
  return it->second.term;
}

void log_store::trim_to(uint64_t trim_index) {
  if (index_map_.empty() || trim_index == 0) {
    return;
  }
  for (auto it = index_map_.begin(); it != index_map_.end();) {
    if (it->first <= trim_index) {
      it = index_map_.erase(it);
    } else {
      break;
    }
  }
  if (index_map_.empty()) {
    base_index_ = 0;
    last_index_ = 0;
  } else {
    base_index_ = index_map_.begin()->first;
    last_index_ = index_map_.rbegin()->first;
  }
}

void log_store::truncate_from(uint64_t from_index, op_complete cb, void* arg) {
  if (from_index == 0) {
    if (cb) {
      cb(arg, -EINVAL);
    }
    return;
  }
  for (auto it = index_map_.lower_bound(from_index); it != index_map_.end();) {
    it = index_map_.erase(it);
  }
  if (index_map_.empty()) {
    base_index_ = 0;
    last_index_ = 0;
  } else {
    base_index_ = index_map_.begin()->first;
    last_index_ = index_map_.rbegin()->first;
  }
  if (cb) {
    cb(arg, 0);
  }
}

int log_store::apply_record(const data_record& rec, uint64_t phys_offset) {
  if (rec.type != record_type::raft_log_entry) {
    return 0;
  }
  if (rec.pool_id != id_.pool_id || rec.pg_id != id_.pg_id ||
      rec.store_epoch != id_.store_epoch) {
    return 0; // other PG
  }
  log_entry e;
  if (!decode_payload(rec.payload, &e)) {
    return -EIO;
  }
  if (e.index == 0) {
    return -EIO;
  }
  // On-disk length: use aligned size from header+payload estimate.
  const auto encoded_len = encode_record(rec).size();
  const uint64_t aligned =
      backend_ ? writer_.align_up(encoded_len) : encoded_len;
  index_map_[e.index] = idx_pos{phys_offset, aligned, e.term};
  if (base_index_ == 0 || e.index < base_index_) {
    base_index_ = e.index;
  }
  if (e.index > last_index_) {
    last_index_ = e.index;
  }
  return 0;
}

uint64_t log_store::rebuild_from_image(const uint8_t* image, size_t len) {
  index_map_.clear();
  base_index_ = 0;
  last_index_ = 0;
  size_t off = 0;
  uint64_t last_good = 0;
  while (off < len) {
    data_record rec;
    size_t consumed = 0;
    auto st = decode_record(image + off, len - off, &rec, &consumed);
    if (st == decode_status::need_more || st == decode_status::bad_magic) {
      break;
    }
    if (st != decode_status::ok) {
      break;
    }
    if (rec.type == record_type::raft_log_entry) {
      if (apply_record(rec, off) != 0) {
        break;
      }
    }
    // Skip other record types (object/kv on shared device image in tests).
    last_good = off + consumed;
    // Align forward if backend known.
    if (backend_) {
      const uint64_t al = writer_.align_up(consumed);
      off += static_cast<size_t>(al);
    } else {
      off += consumed;
    }
  }
  return last_good;
}

void log_store::for_each_live_extent(
    const std::function<void(uint64_t phys_offset, uint64_t length)>& fn) const {
  if (!fn) {
    return;
  }
  for (const auto& kv : index_map_) {
    if (kv.second.length > 0) {
      fn(kv.second.phys_offset, kv.second.length);
    }
  }
}

} // namespace fastblock::aos
