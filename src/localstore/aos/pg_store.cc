/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "pg_store.h"

#include <spdk/env.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace fastblock::aos {

namespace {
constexpr uint64_t kRecoverChunk = 256 * 1024;
}

struct pg_store::write_req {
  pg_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  std::string name;
  uint64_t offset{0};
  uint64_t len{0};
  uint64_t raft_apply_index{0};
  uint64_t image_snap_seq{0};
  std::vector<uint8_t> payload;
  uint64_t object_id{0};
  bool need_name_record{false};
  uint64_t data_phys{0};
};

struct pg_store::remove_req {
  pg_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  std::string name;
  uint64_t raft_apply_index{0};
  uint64_t image_snap_seq{0};
  uint64_t object_id{0};
};

struct pg_store::discard_req {
  pg_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  std::string name;
  uint64_t offset{0};
  uint64_t len{0};
  uint64_t raft_apply_index{0};
  uint64_t image_snap_seq{0};
  uint64_t object_id{0};
};

struct pg_store::checkpoint_req {
  pg_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  uint64_t generation{0};
  uint64_t phys{0};
  uint64_t aligned_len{0};
};

struct pg_store::read_req {
  pg_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  std::string name;
  uint64_t offset{0};
  uint64_t len{0};
  uint8_t* user_buf{nullptr};
  std::vector<extent> extents;
  size_t extent_idx{0};
  void* stage{nullptr};
  uint64_t stage_cap{0};
  uint64_t copy_len{0};
  uint64_t pad{0};
  uint64_t user_skip{0};
};

struct pg_store::recover_req {
  enum class phase {
    find_checkpoint = 0,
    replay = 1,
  };

  pg_store* self{nullptr};
  op_complete cb;
  void* arg{nullptr};
  phase ph{phase::find_checkpoint};
  uint64_t pos{0};
  uint64_t last_good{0};
  void* buf{nullptr};
  uint64_t buf_cap{0};
  // Leftover bytes not yet forming a full record across chunk boundaries.
  std::vector<uint8_t> pending;

  // Best complete checkpoint found during find_checkpoint.
  bool have_ckpt{false};
  uint64_t best_ckpt_gen{0};
  uint64_t best_ckpt_end{0};
  std::string best_ckpt_payload;
};

pg_store::pg_store(device_backend* backend, pg_id id, buffer_mode mode,
                   uint64_t segment_size)
    : backend_(backend),
      alloc_(std::make_shared<segment_allocator>(
          backend ? backend->capacity() : 0,
          segment_size == 0 ? (backend ? backend->capacity() : 0)
                            : segment_size,
          backend ? backend->block_size() : 4096)),
      seg_(backend, alloc_.get()), id_(id), mode_(mode) {
  ensure_scratch(kMaxObjectSize + 256 * 1024);
}

pg_store::pg_store(device_backend* backend, pg_id id,
                   std::shared_ptr<segment_allocator> alloc, buffer_mode mode)
    : backend_(backend), alloc_(std::move(alloc)),
      seg_(backend, alloc_.get()), id_(id), mode_(mode) {
  ensure_scratch(kMaxObjectSize + 256 * 1024);
}

pg_store::~pg_store() {
  free_buf(scratch_ptr_);
  scratch_ptr_ = nullptr;
  scratch_cap_ = 0;
}

void* pg_store::alloc_buf(size_t size, size_t align) {
  if (size == 0) {
    return nullptr;
  }
  if (mode_ == buffer_mode::spdk_dma) {
    return spdk_dma_zmalloc(size, align, nullptr);
  }
  void* p = nullptr;
  if (posix_memalign(&p, align, size) != 0) {
    return nullptr;
  }
  std::memset(p, 0, size);
  return p;
}

void pg_store::free_buf(void* p) {
  if (p == nullptr) {
    return;
  }
  if (mode_ == buffer_mode::spdk_dma) {
    spdk_dma_free(p);
  } else {
    free(p);
  }
}

void pg_store::ensure_scratch(uint64_t need) {
  if (scratch_cap_ >= need && scratch_ptr_ != nullptr) {
    return;
  }
  free_buf(scratch_ptr_);
  scratch_ptr_ = nullptr;
  scratch_cap_ = 0;
  const size_t align =
      backend_ ? std::max<size_t>(backend_->block_size(), 4096) : 4096;
  scratch_ptr_ = alloc_buf(static_cast<size_t>(need), align);
  if (scratch_ptr_ != nullptr) {
    scratch_cap_ = need;
  }
}

void pg_store::enqueue_or_run(std::function<void()> op) {
  if (busy_) {
    queue_.push_back(std::move(op));
    return;
  }
  busy_ = true;
  op();
}

void pg_store::pump_queue() {
  if (busy_ || queue_.empty()) {
    return;
  }
  auto op = std::move(queue_.front());
  queue_.pop_front();
  busy_ = true;
  op();
}

void pg_store::finish_op(op_complete cb, void* arg, int err) {
  busy_ = false;
  if (cb) {
    cb(arg, err);
  }
  pump_queue();
}

uint64_t pg_store::alloc_object_id() { return next_object_id_++; }

void pg_store::clear_maps() {
  name_to_id_.clear();
  objects_.clear();
  next_object_id_ = 1;
}

checkpoint_snapshot pg_store::build_snapshot(uint64_t generation) const {
  checkpoint_snapshot snap;
  snap.generation = generation;
  snap.pool_id = id_.pool_id;
  snap.pg_id = id_.pg_id;
  snap.store_epoch = id_.store_epoch;
  snap.next_object_id = next_object_id_;
  snap.objects.reserve(objects_.size());
  for (const auto& kv : objects_) {
    checkpoint_object o;
    o.object_id = kv.first;
    o.name = kv.second.name;
    o.deleted = kv.second.deleted;
    o.delete_raft_index = kv.second.delete_raft_index;
    for (const auto& e : kv.second.head.all()) {
      checkpoint_extent ce;
      ce.logical_offset = e.logical_offset;
      ce.length = e.length;
      ce.raft_create_index = e.raft_create_index;
      ce.image_create_seq = e.image_create_seq;
      ce.phys_segment = e.phys_segment;
      ce.phys_offset = e.phys_offset;
      o.extents.push_back(ce);
    }
    for (const auto& w : kv.second.applied_writes) {
      o.applied_writes.push_back(w);
    }
    for (const auto& d : kv.second.applied_discards) {
      o.applied_discards.push_back(d);
    }
    snap.objects.push_back(std::move(o));
  }
  return snap;
}

int pg_store::apply_checkpoint_snapshot(const checkpoint_snapshot& snap,
                                        uint64_t checkpoint_end_phys) {
  if (snap.pool_id != id_.pool_id || snap.pg_id != id_.pg_id ||
      snap.store_epoch != id_.store_epoch) {
    return 0;
  }
  clear_maps();
  next_object_id_ = snap.next_object_id;
  checkpoint_generation_ = snap.generation;
  last_checkpoint_end_ = checkpoint_end_phys;

  for (const auto& o : snap.objects) {
    object_state st;
    st.name = o.name;
    st.deleted = o.deleted;
    st.delete_raft_index = o.delete_raft_index;
    if (!o.name.empty()) {
      name_to_id_[o.name] = o.object_id;
    }
    for (const auto& ce : o.extents) {
      if (ce.length == 0 || !valid_object_range(ce.logical_offset, ce.length)) {
        continue;
      }
      extent e;
      e.logical_offset = ce.logical_offset;
      e.length = ce.length;
      e.raft_create_index = ce.raft_create_index;
      e.image_create_seq = ce.image_create_seq;
      e.phys_segment = ce.phys_segment;
      e.phys_offset = ce.phys_offset;
      st.head.put(e);
    }
    for (const auto& w : o.applied_writes) {
      st.applied_writes[w.first] = w.second;
    }
    for (const auto& d : o.applied_discards) {
      st.applied_discards[d.first] = d.second;
    }
    objects_[o.object_id] = std::move(st);
  }
  return 0;
}

pg_store::object_state* pg_store::find_object(const std::string& name) {
  auto it = name_to_id_.find(name);
  if (it == name_to_id_.end()) {
    return nullptr;
  }
  auto oit = objects_.find(it->second);
  if (oit == objects_.end()) {
    return nullptr;
  }
  return &oit->second;
}

pg_store::object_state* pg_store::find_or_alloc_object(const std::string& name,
                                                       bool* is_new) {
  if (auto* o = find_object(name)) {
    if (is_new) {
      *is_new = false;
    }
    return o;
  }
  const uint64_t oid = alloc_object_id();
  name_to_id_[name] = oid;
  object_state st;
  st.name = name;
  objects_[oid] = std::move(st);
  if (is_new) {
    *is_new = true;
  }
  return &objects_[oid];
}

void pg_store::write(const std::string& object_name, uint64_t offset,
                     const void* data, uint64_t len, uint64_t raft_apply_index,
                     uint64_t image_snap_seq, op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr || data == nullptr || object_name.empty()) {
    cb(arg, -EINVAL);
    return;
  }
  if (!valid_object_range(offset, len)) {
    cb(arg, -EINVAL);
    return;
  }

  // Copy payload now; caller buffer need not outlive callback.
  std::vector<uint8_t> payload(static_cast<const uint8_t*>(data),
                               static_cast<const uint8_t*>(data) + len);

  enqueue_or_run([this, object_name, offset, len, raft_apply_index,
                  image_snap_seq, cb = std::move(cb), arg,
                  payload = std::move(payload)]() mutable {
    bool is_new = false;
    object_state* obj = find_or_alloc_object(object_name, &is_new);
    if (obj->deleted) {
      obj->deleted = false;
      obj->head.clear();
      obj->applied_writes.clear();
      obj->applied_discards.clear();
    }

    const auto key = std::make_pair(offset, len);
    auto ait = obj->applied_writes.find(key);
    if (ait != obj->applied_writes.end() && ait->second == raft_apply_index) {
      finish_op(std::move(cb), arg, 0);
      return;
    }

    auto* req = new (std::nothrow) write_req();
    if (req == nullptr) {
      finish_op(std::move(cb), arg, -ENOMEM);
      return;
    }
    req->self = this;
    req->cb = std::move(cb);
    req->arg = arg;
    req->name = object_name;
    req->offset = offset;
    req->len = len;
    req->raft_apply_index = raft_apply_index;
    req->image_snap_seq = image_snap_seq;
    req->payload = std::move(payload);
    req->object_id = name_to_id_[object_name];
    req->need_name_record = is_new;

    if (req->need_name_record) {
      data_record name_rec;
      name_rec.type = record_type::object_name;
      name_rec.pool_id = id_.pool_id;
      name_rec.pg_id = id_.pg_id;
      name_rec.store_epoch = id_.store_epoch;
      name_rec.object_id = req->object_id;
      name_rec.raft_apply_index = raft_apply_index;
      name_rec.image_snap_seq = image_snap_seq;
      name_rec.payload = object_name;

      const uint64_t need =
          seg_.align_up(kRecordHeaderLenV1 + name_rec.payload.size());
      ensure_scratch(need);
      if (scratch_ptr_ == nullptr) {
        auto c = std::move(req->cb);
        void* a = req->arg;
        delete req;
        finish_op(std::move(c), a, -ENOMEM);
        return;
      }
      seg_.append_record(name_rec, scratch_ptr_, scratch_cap_, write_after_name,
                         req);
      return;
    }
    write_after_name(req, 0, 0);
  });
}

void pg_store::write_after_name(void* arg, uint64_t /*phys*/, int err) {
  auto* req = static_cast<write_req*>(arg);
  pg_store* self = req->self;
  if (err != 0) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, err);
    return;
  }

  data_record rec;
  rec.type = record_type::data;
  rec.pool_id = self->id_.pool_id;
  rec.pg_id = self->id_.pg_id;
  rec.store_epoch = self->id_.store_epoch;
  rec.object_id = req->object_id;
  rec.object_offset = req->offset;
  rec.raft_apply_index = req->raft_apply_index;
  rec.image_snap_seq = req->image_snap_seq;
  rec.payload.assign(req->payload.begin(), req->payload.end());

  const uint64_t need =
      self->seg_.align_up(kRecordHeaderLenV1 + rec.payload.size());
  self->ensure_scratch(need);
  if (self->scratch_ptr_ == nullptr) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, -ENOMEM);
    return;
  }
  self->seg_.append_record(rec, self->scratch_ptr_, self->scratch_cap_,
                           write_after_data, req);
}

void pg_store::write_after_data(void* arg, uint64_t phys, int err) {
  auto* req = static_cast<write_req*>(arg);
  pg_store* self = req->self;
  if (err != 0) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, err);
    return;
  }
  req->data_phys = phys;
  self->seg_.flush(write_after_flush, req);
}

void pg_store::write_after_flush(void* arg, int err) {
  auto* req = static_cast<write_req*>(arg);
  pg_store* self = req->self;
  if (err != 0) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, err);
    return;
  }

  auto oit = self->objects_.find(req->object_id);
  if (oit == self->objects_.end()) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, -ENOENT);
    return;
  }
  object_state& obj = oit->second;
  extent e;
  e.logical_offset = req->offset;
  e.length = req->len;
  e.raft_create_index = req->raft_apply_index;
  e.image_create_seq = req->image_snap_seq;
  e.phys_segment = 0;
  e.phys_offset = req->data_phys + kRecordHeaderLenV1;
  obj.head.put(e);
  obj.applied_writes[{req->offset, req->len}] = req->raft_apply_index;

  auto cb = std::move(req->cb);
  void* a = req->arg;
  delete req;
  self->finish_op(std::move(cb), a, 0);
}

void pg_store::remove(const std::string& object_name, uint64_t raft_apply_index,
                      uint64_t image_snap_seq, op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr || object_name.empty()) {
    cb(arg, -EINVAL);
    return;
  }

  enqueue_or_run([this, object_name, raft_apply_index, image_snap_seq,
                  cb = std::move(cb), arg]() mutable {
    object_state* obj = find_object(object_name);
    if (obj == nullptr || obj->deleted) {
      finish_op(std::move(cb), arg, 0);
      return;
    }

    auto* req = new (std::nothrow) remove_req();
    if (req == nullptr) {
      finish_op(std::move(cb), arg, -ENOMEM);
      return;
    }
    req->self = this;
    req->cb = std::move(cb);
    req->arg = arg;
    req->name = object_name;
    req->raft_apply_index = raft_apply_index;
    req->image_snap_seq = image_snap_seq;
    req->object_id = name_to_id_[object_name];

    data_record rec;
    rec.type = record_type::delete_tombstone;
    rec.pool_id = id_.pool_id;
    rec.pg_id = id_.pg_id;
    rec.store_epoch = id_.store_epoch;
    rec.object_id = req->object_id;
    rec.raft_apply_index = raft_apply_index;
    rec.image_snap_seq = image_snap_seq;

    const uint64_t need = seg_.align_up(kRecordHeaderLenV1);
    ensure_scratch(need);
    if (scratch_ptr_ == nullptr) {
      auto c = std::move(req->cb);
      void* a = req->arg;
      delete req;
      finish_op(std::move(c), a, -ENOMEM);
      return;
    }
    seg_.append_record(rec, scratch_ptr_, scratch_cap_, remove_after_tomb, req);
  });
}

void pg_store::remove_after_tomb(void* arg, uint64_t /*phys*/, int err) {
  auto* req = static_cast<remove_req*>(arg);
  pg_store* self = req->self;
  if (err != 0) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, err);
    return;
  }
  self->seg_.flush(remove_after_flush, req);
}

void pg_store::remove_after_flush(void* arg, int err) {
  auto* req = static_cast<remove_req*>(arg);
  pg_store* self = req->self;
  if (err == 0) {
    auto oit = self->objects_.find(req->object_id);
    if (oit != self->objects_.end()) {
      oit->second.deleted = true;
      oit->second.delete_raft_index = req->raft_apply_index;
      oit->second.head.clear();
      oit->second.applied_writes.clear();
      oit->second.applied_discards.clear();
    }
  }
  auto cb = std::move(req->cb);
  void* a = req->arg;
  delete req;
  self->finish_op(std::move(cb), a, err);
}

void pg_store::discard(const std::string& object_name, uint64_t offset,
                       uint64_t len, uint64_t raft_apply_index,
                       uint64_t image_snap_seq, op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr || object_name.empty()) {
    cb(arg, -EINVAL);
    return;
  }
  if (!valid_object_range(offset, len)) {
    cb(arg, -EINVAL);
    return;
  }

  enqueue_or_run([this, object_name, offset, len, raft_apply_index,
                  image_snap_seq, cb = std::move(cb), arg]() mutable {
    object_state* obj = find_object(object_name);
    if (obj == nullptr || obj->deleted) {
      // No live object: discard is a no-op success (same as remove missing).
      finish_op(std::move(cb), arg, 0);
      return;
    }

    const auto key = std::make_pair(offset, len);
    auto dit = obj->applied_discards.find(key);
    if (dit != obj->applied_discards.end() &&
        dit->second == raft_apply_index) {
      finish_op(std::move(cb), arg, 0);
      return;
    }

    auto* req = new (std::nothrow) discard_req();
    if (req == nullptr) {
      finish_op(std::move(cb), arg, -ENOMEM);
      return;
    }
    req->self = this;
    req->cb = std::move(cb);
    req->arg = arg;
    req->name = object_name;
    req->offset = offset;
    req->len = len;
    req->raft_apply_index = raft_apply_index;
    req->image_snap_seq = image_snap_seq;
    req->object_id = name_to_id_[object_name];

    // Encode length as little-endian u64 payload (design §7.3 range tombstone).
    data_record rec;
    rec.type = record_type::range_tombstone;
    rec.pool_id = id_.pool_id;
    rec.pg_id = id_.pg_id;
    rec.store_epoch = id_.store_epoch;
    rec.object_id = req->object_id;
    rec.object_offset = offset;
    rec.raft_apply_index = raft_apply_index;
    rec.image_snap_seq = image_snap_seq;
    rec.payload.resize(8);
    for (int i = 0; i < 8; ++i) {
      rec.payload[static_cast<size_t>(i)] =
          static_cast<char>((len >> (8 * i)) & 0xff);
    }

    const uint64_t need =
        seg_.align_up(kRecordHeaderLenV1 + rec.payload.size());
    ensure_scratch(need);
    if (scratch_ptr_ == nullptr) {
      auto c = std::move(req->cb);
      void* a = req->arg;
      delete req;
      finish_op(std::move(c), a, -ENOMEM);
      return;
    }
    seg_.append_record(rec, scratch_ptr_, scratch_cap_, discard_after_tomb,
                       req);
  });
}

void pg_store::discard_after_tomb(void* arg, uint64_t /*phys*/, int err) {
  auto* req = static_cast<discard_req*>(arg);
  pg_store* self = req->self;
  if (err != 0) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, err);
    return;
  }
  self->seg_.flush(discard_after_flush, req);
}

void pg_store::discard_after_flush(void* arg, int err) {
  auto* req = static_cast<discard_req*>(arg);
  pg_store* self = req->self;
  if (err == 0) {
    auto oit = self->objects_.find(req->object_id);
    if (oit != self->objects_.end() && !oit->second.deleted) {
      oit->second.head.discard(req->offset, req->len);
      oit->second.applied_discards[{req->offset, req->len}] =
          req->raft_apply_index;
    }
  }
  auto cb = std::move(req->cb);
  void* a = req->arg;
  delete req;
  self->finish_op(std::move(cb), a, err);
}

void pg_store::checkpoint(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr) {
    cb(arg, -EINVAL);
    return;
  }

  enqueue_or_run([this, cb = std::move(cb), arg]() mutable {
    const uint64_t gen = checkpoint_generation_ + 1;
    checkpoint_snapshot snap = build_snapshot(gen);
    const std::string payload = encode_checkpoint_payload(snap);

    auto* req = new (std::nothrow) checkpoint_req();
    if (req == nullptr) {
      finish_op(std::move(cb), arg, -ENOMEM);
      return;
    }
    req->self = this;
    req->cb = std::move(cb);
    req->arg = arg;
    req->generation = gen;

    data_record rec;
    rec.type = record_type::checkpoint_marker;
    rec.pool_id = id_.pool_id;
    rec.pg_id = id_.pg_id;
    rec.store_epoch = id_.store_epoch;
    rec.object_id = 0;
    rec.object_offset = gen;
    rec.raft_apply_index = 0;
    rec.image_snap_seq = 0;
    rec.payload = payload;

    const uint64_t need =
        seg_.align_up(kRecordHeaderLenV1 + rec.payload.size());
    ensure_scratch(need);
    if (scratch_ptr_ == nullptr) {
      auto c = std::move(req->cb);
      void* a = req->arg;
      delete req;
      finish_op(std::move(c), a, -ENOMEM);
      return;
    }
    req->aligned_len = need;
    seg_.append_record(rec, scratch_ptr_, scratch_cap_, checkpoint_after_append,
                       req);
  });
}

void pg_store::checkpoint_after_append(void* arg, uint64_t phys, int err) {
  auto* req = static_cast<checkpoint_req*>(arg);
  pg_store* self = req->self;
  if (err != 0) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, err);
    return;
  }
  req->phys = phys;
  self->seg_.flush(checkpoint_after_flush, req);
}

void pg_store::checkpoint_after_flush(void* arg, int err) {
  auto* req = static_cast<checkpoint_req*>(arg);
  pg_store* self = req->self;
  if (err == 0) {
    self->checkpoint_generation_ = req->generation;
    self->last_checkpoint_end_ = req->phys + req->aligned_len;
  }
  auto cb = std::move(req->cb);
  void* a = req->arg;
  delete req;
  self->finish_op(std::move(cb), a, err);
}

void pg_store::flush(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr) {
    cb(arg, -EINVAL);
    return;
  }
  enqueue_or_run([this, cb = std::move(cb), arg]() mutable {
    seg_.flush(
        [](void* a, int err) {
          auto* pack = static_cast<std::pair<pg_store*, std::pair<op_complete, void*>>*>(a);
          pg_store* self = pack->first;
          op_complete c = std::move(pack->second.first);
          void* user = pack->second.second;
          delete pack;
          self->finish_op(std::move(c), user, err);
        },
        new std::pair<pg_store*, std::pair<op_complete, void*>>(
            this, std::make_pair(std::move(cb), arg)));
  });
}

void pg_store::read(const std::string& object_name, uint64_t offset, void* buf,
                    uint64_t len, op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr || buf == nullptr || object_name.empty()) {
    cb(arg, -EINVAL);
    return;
  }
  if (!valid_object_range(offset, len)) {
    cb(arg, -EINVAL);
    return;
  }

  enqueue_or_run([this, object_name, offset, buf, len, cb = std::move(cb),
                  arg]() mutable {
    object_state* obj = find_object(object_name);
    std::memset(buf, 0, static_cast<size_t>(len));
    if (obj == nullptr || obj->deleted) {
      finish_op(std::move(cb), arg, 0);
      return;
    }

    auto extents = obj->head.query(offset, len);
    if (extents.empty()) {
      finish_op(std::move(cb), arg, 0);
      return;
    }

    auto* req = new (std::nothrow) read_req();
    if (req == nullptr) {
      finish_op(std::move(cb), arg, -ENOMEM);
      return;
    }
    req->self = this;
    req->cb = std::move(cb);
    req->arg = arg;
    req->name = object_name;
    req->offset = offset;
    req->len = len;
    req->user_buf = static_cast<uint8_t*>(buf);
    req->extents = std::move(extents);
    req->extent_idx = 0;
    read_extent_done(req, 0);
  });
}

void pg_store::read_extent_done(void* arg, int err) {
  auto* req = static_cast<read_req*>(arg);
  pg_store* self = req->self;

  if (err != 0) {
    self->free_buf(req->stage);
    req->stage = nullptr;
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, err);
    return;
  }

  if (req->extent_idx >= req->extents.size()) {
    self->free_buf(req->stage);
    req->stage = nullptr;
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, 0);
    return;
  }

  // If previous stage had data to copy — already copied in read callback.

  const extent& e = req->extents[req->extent_idx];
  const uint64_t req_end = req->offset + req->len;
  const uint64_t e_end = e.logical_offset + e.length;
  const uint64_t lo = std::max(req->offset, e.logical_offset);
  const uint64_t hi = std::min(req_end, e_end);
  if (lo >= hi) {
    ++req->extent_idx;
    read_extent_done(req, 0);
    return;
  }

  const uint64_t copy_len = hi - lo;
  const uint64_t extent_skip = lo - e.logical_offset;
  const uint64_t user_skip = lo - req->offset;
  const uint64_t phys = e.phys_offset + extent_skip;

  const uint32_t bs = self->backend_->block_size();
  const uint64_t aligned_off = phys - (phys % bs);
  const uint64_t pad = phys - aligned_off;
  const uint64_t aligned_len = ((copy_len + pad + bs - 1) / bs) * bs;

  self->free_buf(req->stage);
  req->stage = self->alloc_buf(static_cast<size_t>(aligned_len), bs);
  req->stage_cap = aligned_len;
  if (req->stage == nullptr) {
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, -ENOMEM);
    return;
  }
  req->copy_len = copy_len;
  req->pad = pad;
  req->user_skip = user_skip;

  self->backend_->read(
      aligned_off, req->stage, aligned_len,
      [](void* a, int rerr) {
        auto* req2 = static_cast<read_req*>(a);
        if (rerr == 0) {
          std::memcpy(req2->user_buf + req2->user_skip,
                      static_cast<uint8_t*>(req2->stage) + req2->pad,
                      static_cast<size_t>(req2->copy_len));
        }
        ++req2->extent_idx;
        read_extent_done(req2, rerr);
      },
      req);
}

void pg_store::recover(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (backend_ == nullptr) {
    cb(arg, -EINVAL);
    return;
  }

  enqueue_or_run([this, cb = std::move(cb), arg]() mutable {
    clear_maps();
    checkpoint_generation_ = 0;
    last_checkpoint_end_ = 0;
    // Only reset allocator when exclusively owned (not shared multi-PG).
    if (alloc_ && alloc_.use_count() == 1) {
      alloc_->reset();
    }

    auto* req = new (std::nothrow) recover_req();
    if (req == nullptr) {
      finish_op(std::move(cb), arg, -ENOMEM);
      return;
    }
    req->self = this;
    req->cb = std::move(cb);
    req->arg = arg;
    req->ph = recover_req::phase::find_checkpoint;
    req->pos = 0;
    req->last_good = 0;
    req->have_ckpt = false;

    const uint32_t bs = backend_->block_size();
    const uint64_t chunk = std::max<uint64_t>(kRecoverChunk, bs);
    req->buf_cap = chunk;
    req->buf = alloc_buf(static_cast<size_t>(chunk), bs);
    if (req->buf == nullptr) {
      auto c = std::move(req->cb);
      void* a = req->arg;
      delete req;
      finish_op(std::move(c), a, -ENOMEM);
      return;
    }

    // Phase 1: find latest complete checkpoint; phase 2: load + replay tail.
    recover_read_done(req, 0);
  });
}

void pg_store::recover_start_replay(recover_req* req, uint64_t from_offset) {
  clear_maps();
  checkpoint_generation_ = 0;
  last_checkpoint_end_ = 0;
  // Keep allocator state from find phase (note_written); do not full reset.

  if (req->have_ckpt) {
    checkpoint_snapshot snap;
    if (decode_checkpoint_payload(req->best_ckpt_payload, &snap)) {
      apply_checkpoint_snapshot(snap, req->best_ckpt_end);
    } else {
      // Should not happen: payload was validated when saved.
      req->have_ckpt = false;
    }
  }

  req->ph = recover_req::phase::replay;
  req->pos = from_offset;
  req->last_good = from_offset;
  req->pending.clear();
  recover_process_buffer(req);
}

void pg_store::recover_read_done(void* arg, int err) {
  auto* req = static_cast<recover_req*>(arg);
  pg_store* self = req->self;

  if (err != 0) {
    self->free_buf(req->buf);
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    self->finish_op(std::move(cb), a, err);
    return;
  }

  // First entry or after a successful chunk process: schedule next read or finish.
  self->recover_process_buffer(req);
}

void pg_store::recover_process_buffer(recover_req* req) {
  const uint64_t cap = backend_->capacity();
  const uint32_t bs = backend_->block_size();

  auto finish_ok = [&]() {
    backend_->set_write_pointer(req->last_good);
    if (alloc_) {
      alloc_->set_open_cursor_abs(req->last_good);
    }
    free_buf(req->buf);
    auto cb = std::move(req->cb);
    void* a = req->arg;
    delete req;
    finish_op(std::move(cb), a, 0);
  };

  auto end_of_find_phase = [&]() {
    // Transition: load best checkpoint (if any) and replay from its end.
    const uint64_t from =
        req->have_ckpt ? req->best_ckpt_end : 0;
    recover_start_replay(req, from);
  };

  const uint64_t seg_size =
      alloc_ ? alloc_->segment_size() : backend_->capacity();
  const uint64_t region_base = alloc_ ? alloc_->region_base() : 0;
  const uint64_t region_end =
      region_base + (alloc_ ? alloc_->capacity() : backend_->capacity());

  // Skip pad hole to next segment boundary (multi-segment layout).
  auto skip_to_next_segment = [&]() -> bool {
    if (seg_size == 0 || alloc_ == nullptr) {
      return false;
    }
    if (seg_size >= alloc_->capacity()) {
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
    if (target >= region_end || target >= backend_->write_pointer()) {
      return false;
    }
    req->last_good = target;
    req->pos = target;
    req->pending.clear();
    return true;
  };

  // Decode from pending buffer first.
  // returns: 0 continue, 1 stop phase (hard end), 2 need more, 3 skip segment
  auto try_decode_pending = [&]() -> int {
    while (!req->pending.empty()) {
      data_record rec;
      size_t consumed = 0;
      const decode_status st = decode_record(
          req->pending.data(), req->pending.size(), &rec, &consumed);
      if (st == decode_status::need_more) {
        return 2;
      }
      if (st != decode_status::ok) {
        // Torn / empty pad at segment tail → try next segment.
        if (skip_to_next_segment()) {
          return 3;
        }
        return 1;
      }
      const uint64_t aligned =
          ((static_cast<uint64_t>(consumed) + bs - 1) / bs) * bs;
      if (req->pending.size() < aligned) {
        return 2;
      }
      // Do not cross segment boundary with a single record.
      if (seg_size > 0 && alloc_ && seg_size < alloc_->capacity() &&
          req->last_good >= region_base) {
        const uint64_t rel = req->last_good - region_base;
        const uint64_t seg_end =
            region_base + ((rel / seg_size) + 1) * seg_size;
        if (req->last_good + aligned > seg_end) {
          if (skip_to_next_segment()) {
            return 3;
          }
          return 1;
        }
      }
      const uint64_t rec_phys = req->last_good;
      if (req->ph == recover_req::phase::find_checkpoint) {
        if (rec.type == record_type::checkpoint_marker &&
            rec.pool_id == id_.pool_id && rec.pg_id == id_.pg_id &&
            rec.store_epoch == id_.store_epoch) {
          checkpoint_snapshot snap;
          if (decode_checkpoint_payload(rec.payload, &snap) &&
              snap.generation >= req->best_ckpt_gen) {
            req->have_ckpt = true;
            req->best_ckpt_gen = snap.generation;
            req->best_ckpt_end = rec_phys + aligned;
            req->best_ckpt_payload = rec.payload;
          }
        }
      } else {
        apply_record(rec, rec_phys);
      }
      if (alloc_) {
        alloc_->note_written(rec_phys, aligned);
      }
      req->last_good += aligned;
      req->pending.erase(req->pending.begin(),
                         req->pending.begin() +
                             static_cast<std::ptrdiff_t>(aligned));
    }
    return 0;
  };

  const int dec = try_decode_pending();
  if (dec == 3) {
    recover_process_buffer(req);
    return;
  }
  if (dec == 1) {
    // Torn: end of this phase
    if (req->ph == recover_req::phase::find_checkpoint) {
      end_of_find_phase();
      return;
    }
    if (alloc_) {
      alloc_->set_open_cursor_abs(req->last_good);
    }
    finish_ok();
    return;
  }
  if (dec == 2) {
    // need more — fall through to read
  }

  if (req->pos >= cap) {
    if (req->ph == recover_req::phase::find_checkpoint) {
      end_of_find_phase();
      return;
    }
    finish_ok();
    return;
  }

  uint64_t to_read = std::min(req->buf_cap, cap - req->pos);
  to_read = (to_read / bs) * bs;
  if (to_read == 0) {
    if (req->ph == recover_req::phase::find_checkpoint) {
      end_of_find_phase();
      return;
    }
    finish_ok();
    return;
  }

  // mem_device_backend only allows read up to write_pointer.
  if (req->pos + to_read > backend_->write_pointer() &&
      backend_->write_pointer() > req->pos) {
    to_read = backend_->write_pointer() - req->pos;
    to_read = (to_read / bs) * bs;
    if (to_read == 0) {
      if (req->ph == recover_req::phase::find_checkpoint) {
        end_of_find_phase();
        return;
      }
      finish_ok();
      return;
    }
  }

  const uint64_t read_off = req->pos;
  backend_->read(
      read_off, req->buf, to_read,
      [req, to_read, read_off](void* /*a*/, int rerr) {
        pg_store* self = req->self;
        if (rerr != 0) {
          if (rerr == -ERANGE) {
            if (req->ph == recover_req::phase::find_checkpoint) {
              self->recover_start_replay(
                  req, req->have_ckpt ? req->best_ckpt_end : 0);
              return;
            }
            self->backend_->set_write_pointer(req->last_good);
            self->free_buf(req->buf);
            auto cb = std::move(req->cb);
            void* user = req->arg;
            delete req;
            self->finish_op(std::move(cb), user, 0);
            return;
          }
          self->free_buf(req->buf);
          auto cb = std::move(req->cb);
          void* user = req->arg;
          delete req;
          self->finish_op(std::move(cb), user, rerr);
          return;
        }
        const auto* p = static_cast<const uint8_t*>(req->buf);
        req->pending.insert(req->pending.end(), p, p + to_read);
        req->pos = read_off + to_read;
        self->recover_process_buffer(req);
      },
      req);
}

int pg_store::apply_record(const data_record& rec, uint64_t phys_offset) {
  if (rec.pool_id != id_.pool_id || rec.pg_id != id_.pg_id ||
      rec.store_epoch != id_.store_epoch) {
    return 0;
  }

  if (rec.type == record_type::checkpoint_marker) {
    checkpoint_snapshot snap;
    if (!decode_checkpoint_payload(rec.payload, &snap)) {
      return -EINVAL;
    }
    // Physical end of this record is unknown here; rebuild_from_image
    // updates last_checkpoint_end after apply. recover uses find phase.
    const uint64_t end_hint = last_checkpoint_end_;
    return apply_checkpoint_snapshot(snap, end_hint);
  }

  if (rec.type == record_type::object_name) {
    const std::string name = rec.payload;
    name_to_id_[name] = rec.object_id;
    if (objects_.find(rec.object_id) == objects_.end()) {
      object_state st;
      st.name = name;
      objects_[rec.object_id] = std::move(st);
    } else {
      objects_[rec.object_id].name = name;
    }
    if (rec.object_id >= next_object_id_) {
      next_object_id_ = rec.object_id + 1;
    }
    return 0;
  }

  if (rec.type == record_type::delete_tombstone) {
    auto oit = objects_.find(rec.object_id);
    if (oit == objects_.end()) {
      object_state st;
      st.deleted = true;
      st.delete_raft_index = rec.raft_apply_index;
      objects_[rec.object_id] = std::move(st);
      return 0;
    }
    oit->second.deleted = true;
    oit->second.delete_raft_index = rec.raft_apply_index;
    oit->second.head.clear();
    oit->second.applied_writes.clear();
    oit->second.applied_discards.clear();
    return 0;
  }

  if (rec.type == record_type::range_tombstone) {
    if (rec.payload.size() != 8) {
      return -EINVAL;
    }
    uint64_t dlen = 0;
    for (int i = 0; i < 8; ++i) {
      dlen |= static_cast<uint64_t>(static_cast<uint8_t>(rec.payload[i]))
              << (8 * i);
    }
    if (!valid_object_range(rec.object_offset, dlen)) {
      return -EINVAL;
    }
    auto oit = objects_.find(rec.object_id);
    if (oit == objects_.end()) {
      // Orphan range tombstone before object_name — ignore for head.
      return 0;
    }
    object_state& obj = oit->second;
    if (obj.deleted) {
      return 0;
    }
    const auto key = std::make_pair(rec.object_offset, dlen);
    auto dit = obj.applied_discards.find(key);
    if (dit != obj.applied_discards.end() &&
        dit->second == rec.raft_apply_index) {
      return 0;
    }
    obj.head.discard(rec.object_offset, dlen);
    obj.applied_discards[key] = rec.raft_apply_index;
    return 0;
  }

  if (rec.type == record_type::data) {
    auto oit = objects_.find(rec.object_id);
    if (oit == objects_.end()) {
      object_state st;
      objects_[rec.object_id] = std::move(st);
      oit = objects_.find(rec.object_id);
    }
    object_state& obj = oit->second;
    obj.deleted = false;
    const uint64_t len = rec.payload.size();
    if (len == 0 || !valid_object_range(rec.object_offset, len)) {
      return -EINVAL;
    }
    const auto key = std::make_pair(rec.object_offset, len);
    auto ait = obj.applied_writes.find(key);
    if (ait != obj.applied_writes.end() &&
        ait->second == rec.raft_apply_index) {
      return 0;
    }
    extent e;
    e.logical_offset = rec.object_offset;
    e.length = len;
    e.raft_create_index = rec.raft_apply_index;
    e.image_create_seq = rec.image_snap_seq;
    e.phys_segment = 0;
    e.phys_offset = phys_offset + kRecordHeaderLenV1;
    obj.head.put(e);
    obj.applied_writes[key] = rec.raft_apply_index;
    return 0;
  }

  return 0;
}

uint64_t pg_store::rebuild_from_image(const uint8_t* image, size_t len) {
  clear_maps();
  checkpoint_generation_ = 0;
  last_checkpoint_end_ = 0;
  if (alloc_ && alloc_.use_count() == 1) {
    alloc_->reset();
  }

  const uint32_t bs = backend_ ? backend_->block_size() : 4096;
  const uint64_t seg_size = alloc_ ? alloc_->segment_size() : 0;
  uint64_t pos = 0;
  uint64_t last_good = 0;
  while (pos < len) {
    data_record rec;
    size_t consumed = 0;
    const decode_status st =
        decode_record(image + static_cast<size_t>(pos),
                      len - static_cast<size_t>(pos), &rec, &consumed);
    if (st == decode_status::need_more || st != decode_status::ok) {
      // Multi-segment: skip pad to next segment and continue.
      if (seg_size > 0 && seg_size < static_cast<uint64_t>(len)) {
        uint64_t target;
        if (pos % seg_size == 0) {
          target = pos + seg_size;
        } else {
          target = ((pos / seg_size) + 1) * seg_size;
        }
        if (target < len) {
          pos = target;
          continue;
        }
      }
      break;
    }
    const uint64_t aligned =
        ((static_cast<uint64_t>(consumed) + bs - 1) / bs) * bs;
    if (pos + aligned > len) {
      break;
    }
    if (rec.type == record_type::checkpoint_marker) {
      checkpoint_snapshot snap;
      if (decode_checkpoint_payload(rec.payload, &snap)) {
        apply_checkpoint_snapshot(snap, pos + aligned);
      }
    } else {
      apply_record(rec, pos);
    }
    if (alloc_) {
      alloc_->note_written(pos, aligned);
    }
    pos += aligned;
    last_good = pos;
  }
  if (backend_) {
    backend_->set_write_pointer(last_good);
  }
  if (alloc_) {
    alloc_->set_open_cursor_abs(last_good);
  }
  return last_good;
}

} // namespace fastblock::aos
