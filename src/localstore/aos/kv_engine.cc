/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "kv_engine.h"
#include "record.h"

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/thread.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <utility>

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

std::string kv_engine::encode_put_payload(const std::string& key,
                                          const std::string& value) {
  std::string o;
  put_u32(o, static_cast<uint32_t>(key.size()));
  o.append(key);
  put_u32(o, static_cast<uint32_t>(value.size()));
  o.append(value);
  return o;
}

std::string kv_engine::encode_del_payload(const std::string& key) {
  std::string o;
  put_u32(o, static_cast<uint32_t>(key.size()));
  o.append(key);
  return o;
}

std::string kv_engine::encode_checkpoint_payload(
    const std::map<std::string, std::string>& table) {
  std::string o;
  put_u64(o, table.size());
  for (const auto& kv : table) {
    put_u32(o, static_cast<uint32_t>(kv.first.size()));
    o.append(kv.first);
    put_u32(o, static_cast<uint32_t>(kv.second.size()));
    o.append(kv.second);
  }
  return o;
}

bool kv_engine::decode_put_payload(const std::string& p, std::string* key,
                                   std::string* value) {
  if (p.size() < 8 || key == nullptr || value == nullptr) {
    return false;
  }
  const char* c = p.data();
  const uint32_t klen = get_u32(c);
  if (p.size() < 4ull + klen + 4) {
    return false;
  }
  key->assign(c + 4, klen);
  const uint32_t vlen = get_u32(c + 4 + klen);
  if (p.size() < 8ull + klen + vlen) {
    return false;
  }
  value->assign(c + 8 + klen, vlen);
  return true;
}

bool kv_engine::decode_del_payload(const std::string& p, std::string* key) {
  if (p.size() < 4 || key == nullptr) {
    return false;
  }
  const uint32_t klen = get_u32(p.data());
  if (p.size() < 4ull + klen) {
    return false;
  }
  key->assign(p.data() + 4, klen);
  return true;
}

bool kv_engine::decode_checkpoint_payload(
    const std::string& p, std::map<std::string, std::string>* table) {
  if (p.size() < 8 || table == nullptr) {
    return false;
  }
  table->clear();
  const char* c = p.data();
  size_t off = 0;
  const uint64_t n = get_u64(c);
  off = 8;
  for (uint64_t i = 0; i < n; ++i) {
    if (off + 4 > p.size()) {
      return false;
    }
    const uint32_t klen = get_u32(c + off);
    off += 4;
    if (off + klen + 4 > p.size()) {
      return false;
    }
    std::string key(c + off, klen);
    off += klen;
    const uint32_t vlen = get_u32(c + off);
    off += 4;
    if (off + vlen > p.size()) {
      return false;
    }
    std::string val(c + off, vlen);
    off += vlen;
    (*table)[std::move(key)] = std::move(val);
  }
  return true;
}

kv_engine::kv_engine(device_backend* backend,
                     std::shared_ptr<segment_allocator> alloc, buffer_mode mode)
    : backend_(backend)
    , mode_(mode)
    , alloc_(std::move(alloc))
    , writer_(backend, alloc_.get()) {}

kv_engine::~kv_engine() {
  if (poller_) {
    spdk_poller_unregister(&poller_);
    poller_ = nullptr;
  }
}

void kv_engine::start() {
  if (poller_ == nullptr) {
    poller_ = SPDK_POLLER_REGISTER(worker_poll, this, 100000); // 100ms
  }
}

void kv_engine::stop(op_complete cb, void* arg) {
  if (poller_) {
    spdk_poller_unregister(&poller_);
    poller_ = nullptr;
  }
  if (cb) {
    cb(arg, 0);
  }
}

void* kv_engine::alloc_buf(uint64_t n) {
  if (mode_ == buffer_mode::spdk_dma) {
    return spdk_dma_zmalloc(static_cast<size_t>(n), 0x1000, nullptr);
  }
  return std::calloc(1, static_cast<size_t>(n));
}

void kv_engine::free_buf(void* p) {
  if (p == nullptr) {
    return;
  }
  if (mode_ == buffer_mode::spdk_dma) {
    spdk_dma_free(p);
  } else {
    std::free(p);
  }
}

void kv_engine::apply_op(const std::string& key,
                         const std::optional<std::string>& value) {
  if (value) {
    table_[key] = *value;
  } else {
    table_.erase(key);
  }
}

void kv_engine::put(std::string key, std::optional<std::string> value) {
  op_log_.push_back(op{std::move(key), std::move(value)});
}

void kv_engine::remove(std::string key) {
  op_log_.push_back(op{std::move(key), std::nullopt});
}

std::optional<std::string> kv_engine::get(const std::string& key) const {
  auto it = table_.find(key);
  if (it == table_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void kv_engine::write_record(data_record rec, op_complete cb, void* arg) {
  rec.pool_id = kKvPoolId;
  rec.pg_id = kKvPgId;
  rec.store_epoch = kKvEpoch;
  auto encoded = encode_record(rec);
  const uint64_t aligned = writer_.align_up(encoded.size());
  void* dma = alloc_buf(aligned);
  if (dma == nullptr) {
    if (cb) {
      cb(arg, -ENOMEM);
    }
    return;
  }
  std::memcpy(dma, encoded.data(), encoded.size());
  struct ctx {
    kv_engine* self;
    void* dma;
    op_complete cb;
    void* arg;
  };
  auto* c = new ctx{this, dma, std::move(cb), arg};
  writer_.append_record(
      rec, dma, aligned,
      [](void* a, uint64_t /*phys*/, int err) {
        auto* c = static_cast<ctx*>(a);
        if (err != 0) {
          c->self->free_buf(c->dma);
          if (c->cb) {
            c->cb(c->arg, err);
          }
          delete c;
          return;
        }
        c->self->writer_.flush(
            [](void* a2, int ferr) {
              auto* c2 = static_cast<ctx*>(a2);
              c2->self->free_buf(c2->dma);
              if (c2->cb) {
                c2->cb(c2->arg, ferr);
              }
              delete c2;
            },
            c);
      },
      c);
}

void kv_engine::commit_one(op o, op_complete cb, void* arg) {
  data_record rec;
  if (o.value) {
    rec.type = record_type::kv_put;
    rec.payload = encode_put_payload(o.key, *o.value);
  } else {
    rec.type = record_type::kv_delete;
    rec.payload = encode_del_payload(o.key);
  }
  write_record(std::move(rec),
               [this, o = std::move(o), cb = std::move(cb)](void* a, int err) {
                 if (err == 0) {
                   apply_op(o.key, o.value);
                   ++ops_since_checkpoint_;
                 }
                 if (cb) {
                   cb(a, err);
                 }
               },
               arg);
}

void kv_engine::commit(op_complete cb, void* arg) {
  if (working_) {
    if (cb) {
      cb(arg, -EBUSY);
    }
    return;
  }
  if (op_log_.empty()) {
    if (cb) {
      cb(arg, 0);
    }
    return;
  }
  working_ = true;
  auto ops = std::exchange(op_log_, {});
  struct cctx {
    kv_engine* self;
    std::vector<op> ops;
    size_t idx{0};
    op_complete cb;
    void* arg;
  };
  auto* c = new cctx{this, std::move(ops), 0, std::move(cb), arg};
  struct holder {
    std::function<void(cctx*)> go;
  };
  auto* h = new holder;
  h->go = [h](cctx* c) {
    if (c->idx >= c->ops.size()) {
      c->self->working_ = false;
      // Opportunistic checkpoint after many ops.
      if (c->self->ops_since_checkpoint_ >= kCheckpointEvery) {
        c->self->maybe_checkpoint(std::move(c->cb), c->arg);
        delete h;
        delete c;
        return;
      }
      if (c->cb) {
        c->cb(c->arg, 0);
      }
      delete h;
      delete c;
      return;
    }
    c->self->commit_one(
        c->ops[c->idx],
        [](void* a, int err) {
          auto* pack = static_cast<std::pair<holder*, cctx*>*>(a);
          holder* hh = pack->first;
          cctx* cc = pack->second;
          delete pack;
          if (err != 0) {
            // re-queue remaining
            for (size_t i = cc->idx; i < cc->ops.size(); ++i) {
              cc->self->op_log_.push_back(std::move(cc->ops[i]));
            }
            cc->self->working_ = false;
            if (cc->cb) {
              cc->cb(cc->arg, err);
            }
            delete hh;
            delete cc;
            return;
          }
          ++cc->idx;
          hh->go(cc);
        },
        new std::pair<holder*, cctx*>(h, c));
  };
  h->go(c);
}

void kv_engine::maybe_checkpoint(op_complete cb, void* arg) {
  working_ = true;
  data_record rec;
  rec.type = record_type::kv_checkpoint;
  rec.payload = encode_checkpoint_payload(table_);
  write_record(std::move(rec),
               [this, cb = std::move(cb)](void* a, int err) {
                 if (err == 0) {
                   ops_since_checkpoint_ = 0;
                 }
                 working_ = false;
                 if (cb) {
                   cb(a, err);
                 }
               },
               arg);
}

int kv_engine::worker_poll(void* arg) {
  auto* self = static_cast<kv_engine*>(arg);
  return self->maybe_work() ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

bool kv_engine::maybe_work() {
  if (working_) {
    return false;
  }
  if (!op_log_.empty()) {
    commit([](void*, int) {}, nullptr);
    return true;
  }
  return false;
}

int kv_engine::apply_record(const data_record& rec) {
  if (rec.pool_id != kKvPoolId || rec.pg_id != kKvPgId) {
    return 0;
  }
  if (rec.type == record_type::kv_checkpoint) {
    std::map<std::string, std::string> t;
    if (!decode_checkpoint_payload(rec.payload, &t)) {
      return -EIO;
    }
    table_ = std::move(t);
    ops_since_checkpoint_ = 0;
    return 0;
  }
  if (rec.type == record_type::kv_put) {
    std::string key;
    std::string value;
    if (!decode_put_payload(rec.payload, &key, &value)) {
      return -EIO;
    }
    apply_op(key, value);
    return 0;
  }
  if (rec.type == record_type::kv_delete) {
    std::string key;
    if (!decode_del_payload(rec.payload, &key)) {
      return -EIO;
    }
    apply_op(key, std::nullopt);
    return 0;
  }
  return 0;
}

uint64_t kv_engine::rebuild_from_image(const uint8_t* image, size_t len) {
  table_.clear();
  op_log_.clear();
  size_t off = 0;
  uint64_t last_good = 0;
  while (off < len) {
    data_record rec;
    size_t consumed = 0;
    auto st = decode_record(image + off, len - off, &rec, &consumed);
    if (st != decode_status::ok) {
      break;
    }
    if (rec.type == record_type::kv_put || rec.type == record_type::kv_delete ||
        rec.type == record_type::kv_checkpoint) {
      if (apply_record(rec) != 0) {
        break;
      }
    }
    last_good = off + consumed;
    if (backend_) {
      off += static_cast<size_t>(writer_.align_up(consumed));
    } else {
      off += consumed;
    }
  }
  return last_good;
}

} // namespace fastblock::aos
