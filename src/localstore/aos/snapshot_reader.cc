/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "snapshot_reader.h"

#include <cerrno>
#include <new>

namespace fastblock::aos {

pg_snapshot_reader::pg_snapshot_reader(pg_store* store, uint64_t raft_index)
    : store_(store), raft_index_(raft_index) {}

pg_snapshot_reader::~pg_snapshot_reader() {
  // Best-effort: if caller forgot close, pin may leak until store destroyed.
  // Production path must always call close().
}

void pg_snapshot_reader::build_plan() {
  plan_.clear();
  cursor_ = 0;
  if (store_ == nullptr) {
    return;
  }
  store_->for_each_snapshot_object(
      raft_index_, /*image_read_seq=*/0,
      [this](const std::string& name, const extent_map& view) {
        for (const auto& e : view.all()) {
          if (e.length == 0) {
            continue;
          }
          plan_item it;
          it.name = name;
          it.offset = e.logical_offset;
          it.len = e.length;
          plan_.push_back(std::move(it));
        }
      });
}

void pg_snapshot_reader::open(pg_store* store, uint64_t raft_snapshot_index,
                              uint64_t raft_term, open_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (store == nullptr || raft_snapshot_index == 0) {
    cb(arg, -EINVAL, nullptr);
    return;
  }

  auto* reader = new (std::nothrow) pg_snapshot_reader(store, raft_snapshot_index);
  if (reader == nullptr) {
    cb(arg, -ENOMEM, nullptr);
    return;
  }

  store->create_raft_snapshot(
      raft_snapshot_index, raft_term,
      [](void* a, int err) {
        auto* pack = static_cast<std::pair<pg_snapshot_reader*,
                                           std::pair<open_complete, void*>>*>(a);
        pg_snapshot_reader* r = pack->first;
        open_complete c = pack->second.first;
        void* user = pack->second.second;
        delete pack;
        if (err != 0) {
          delete r;
          c(user, err, nullptr);
          return;
        }
        r->build_plan();
        c(user, 0, r);
      },
      new std::pair<pg_snapshot_reader*, std::pair<open_complete, void*>>(
          reader, std::make_pair(cb, arg)));
}

void pg_snapshot_reader::next_chunk(void* buf, uint64_t buf_cap,
                                    chunk_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (closed_ || store_ == nullptr || busy_) {
    cb(arg, -EINVAL, true, nullptr, 0, 0);
    return;
  }
  if (cursor_ >= plan_.size()) {
    cb(arg, 0, true, nullptr, 0, 0);
    return;
  }
  const plan_item& item = plan_[cursor_];
  if (buf == nullptr || buf_cap < item.len) {
    cb(arg, -ENOSPC, false, item.name.c_str(), item.offset, item.len);
    return;
  }

  busy_ = true;
  struct ctx {
    pg_snapshot_reader* self;
    chunk_complete cb;
    void* arg;
    std::string name;
    uint64_t offset;
    uint64_t len;
    void* buf;
  };
  auto* c = new (std::nothrow) ctx();
  if (c == nullptr) {
    busy_ = false;
    cb(arg, -ENOMEM, false, nullptr, 0, 0);
    return;
  }
  c->self = this;
  c->cb = cb;
  c->arg = arg;
  c->name = item.name;
  c->offset = item.offset;
  c->len = item.len;
  c->buf = buf;

  store_->read(
      item.name, item.offset, buf, item.len, raft_index_,
      [](void* a, int err) {
        auto* c2 = static_cast<ctx*>(a);
        pg_snapshot_reader* self = c2->self;
        chunk_complete cb2 = c2->cb;
        void* user = c2->arg;
        const std::string name = c2->name;
        const uint64_t off = c2->offset;
        const uint64_t len = c2->len;
        delete c2;
        self->busy_ = false;
        if (err == 0) {
          ++self->cursor_;
        }
        cb2(user, err, false, name.c_str(), off, len);
      },
      c);
}

void pg_snapshot_reader::close(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (closed_ || store_ == nullptr) {
    closed_ = true;
    cb(arg, 0);
    return;
  }
  closed_ = true;
  store_->release_raft_snapshot(raft_index_, std::move(cb), arg);
}

} // namespace fastblock::aos
