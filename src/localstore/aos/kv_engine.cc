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
  flush(std::move(cb), arg);
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

void kv_engine::note_durable(record_type type, uint64_t phys, uint64_t length) {
  if (length == 0) {
    return;
  }
  if (type == record_type::kv_checkpoint) {
    last_checkpoint_ = live_pos{phys, length};
    live_wal_.clear();
    return;
  }
  if (type == record_type::kv_put || type == record_type::kv_delete) {
    live_wal_.push_back(live_pos{phys, length});
  }
}

void kv_engine::reset_for_recover() {
  table_.clear();
  op_log_.clear();
  live_wal_.clear();
  last_checkpoint_ = {};
  ops_since_checkpoint_ = 0;
}

void kv_engine::for_each_live_extent(
    const std::function<void(uint64_t phys_offset, uint64_t length)>& fn) const {
  if (!fn) {
    return;
  }
  if (last_checkpoint_.length > 0) {
    fn(last_checkpoint_.phys_offset, last_checkpoint_.length);
  }
  for (const auto& p : live_wal_) {
    if (p.length > 0) {
      fn(p.phys_offset, p.length);
    }
  }
}

void kv_engine::put(std::string key, std::optional<std::string> value) {
  apply_op(key, value);
  op_log_.push_back(op{std::move(key), std::move(value)});
  kick_commit();
}

void kv_engine::remove(std::string key) {
  apply_op(key, std::nullopt);
  op_log_.push_back(op{std::move(key), std::nullopt});
  kick_commit();
}

void kv_engine::kick_commit() {
  if (working_ || op_log_.empty()) {
    return;
  }
  commit([](void*, int) {}, nullptr);
}

void kv_engine::put_durable(std::string key, std::optional<std::string> value,
                            op_complete cb, void* arg) {
  apply_op(key, value);
  op_log_.push_back(op{std::move(key), std::move(value)});
  flush(std::move(cb), arg);
}

void kv_engine::flush(op_complete cb, void* arg) {
  if (cb) {
    flush_waiters_.push_back(flush_waiter{std::move(cb), arg});
  }
  if (!working_ && op_log_.empty()) {
    fire_flush_waiters(0);
    return;
  }
  if (!working_) {
    kick_commit();
  }
}

void kv_engine::fire_flush_waiters(int err) {
  auto waiters = std::move(flush_waiters_);
  flush_waiters_.clear();
  for (auto& w : waiters) {
    if (w.cb) {
      w.cb(w.arg, err);
    }
  }
}

void kv_engine::after_idle() {
  if (working_) {
    return;
  }
  if (!op_log_.empty()) {
    kick_commit();
    return;
  }
  fire_flush_waiters(0);
}

std::optional<std::string> kv_engine::get(const std::string& key) const {
  auto it = table_.find(key);
  if (it == table_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void kv_engine::for_each_key(
    const std::function<void(const std::string& key)>& fn) const {
  if (!fn) {
    return;
  }
  for (const auto& kv : table_) {
    fn(kv.first);
  }
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
    record_type type;
    uint64_t aligned;
    uint64_t phys{0};
  };
  auto* c = new ctx{this, dma, std::move(cb), arg, rec.type, aligned, 0};
  writer_.append_record(
      rec, dma, aligned,
      [](void* a, uint64_t phys, int err) {
        auto* c = static_cast<ctx*>(a);
        if (err != 0) {
          c->self->free_buf(c->dma);
          if (c->cb) {
            c->cb(c->arg, err);
          }
          delete c;
          return;
        }
        c->phys = phys;
        c->self->writer_.flush(
            [](void* a2, int ferr) {
              auto* c2 = static_cast<ctx*>(a2);
              if (ferr == 0) {
                c2->self->note_durable(c2->type, c2->phys, c2->aligned);
              }
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
      auto user_cb = std::move(c->cb);
      void* user_arg = c->arg;
      kv_engine* self = c->self;
      delete h;
      delete c;
      // Opportunistic checkpoint after many ops.
      if (self->ops_since_checkpoint_ >= kCheckpointEvery) {
        self->maybe_checkpoint(std::move(user_cb), user_arg);
        return;
      }
      if (user_cb) {
        user_cb(user_arg, 0);
      }
      self->after_idle();
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
            auto user_cb = std::move(cc->cb);
            void* user_arg = cc->arg;
            kv_engine* self = cc->self;
            delete hh;
            delete cc;
            if (user_cb) {
              user_cb(user_arg, err);
            }
            self->after_idle();
            return;
          }
          ++cc->idx;
          hh->go(cc);
        },
        new std::pair<holder*, cctx*>(h, c));
  };
  h->go(c);
}

void kv_engine::checkpoint(op_complete cb, void* arg) {
  if (working_) {
    if (cb) {
      cb(arg, -EBUSY);
    }
    return;
  }
  maybe_checkpoint(std::move(cb), arg);
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
                 after_idle();
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

int kv_engine::apply_record(const data_record& rec, uint64_t phys_offset,
                            uint64_t length) {
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
  } else if (rec.type == record_type::kv_put) {
    std::string key;
    std::string value;
    if (!decode_put_payload(rec.payload, &key, &value)) {
      return -EIO;
    }
    apply_op(key, value);
  } else if (rec.type == record_type::kv_delete) {
    std::string key;
    if (!decode_del_payload(rec.payload, &key)) {
      return -EIO;
    }
    apply_op(key, std::nullopt);
  } else {
    return 0;
  }
  uint64_t len = length;
  if (len == 0 && phys_offset != 0 && backend_) {
    len = writer_.align_up(encode_record(rec).size());
  }
  note_durable(rec.type, phys_offset, len);
  return 0;
}

uint64_t kv_engine::rebuild_from_image(const uint8_t* image, size_t len) {
  reset_for_recover();
  size_t off = 0;
  uint64_t last_good = 0;
  while (off < len) {
    data_record rec;
    size_t consumed = 0;
    auto st = decode_record(image + off, len - off, &rec, &consumed);
    if (st != decode_status::ok) {
      break;
    }
    const uint64_t aligned =
        backend_ ? writer_.align_up(consumed) : static_cast<uint64_t>(consumed);
    if (rec.type == record_type::kv_put || rec.type == record_type::kv_delete ||
        rec.type == record_type::kv_checkpoint) {
      if (apply_record(rec, off, aligned) != 0) {
        break;
      }
    }
    last_good = off + consumed;
    off += static_cast<size_t>(aligned);
  }
  return last_good;
}

} // namespace fastblock::aos
