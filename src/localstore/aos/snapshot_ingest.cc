/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "snapshot_ingest.h"

#include "crc32.h"

#include <cerrno>
#include <new>

namespace fastblock::aos {
namespace {

struct cb_pack {
  snapshot_ingest* self{nullptr};
  device_backend::op_complete cb;
  void* arg{nullptr};
};

} // namespace

snapshot_ingest::snapshot_ingest(osd_store* osd, uint64_t pool_id,
                                 uint64_t pg_id)
    : osd_(osd), pool_id_(pool_id), pg_id_(pg_id) {
  if (osd_ != nullptr) {
    active_epoch_ = osd_->serving_epoch(pool_id_, pg_id_);
  }
}

pg_store* snapshot_ingest::active_pg() const {
  if (osd_ == nullptr || active_epoch_ == 0) {
    return nullptr;
  }
  return osd_->get_pg(pool_id_, pg_id_, active_epoch_);
}

pg_store* snapshot_ingest::staging_pg() const {
  if (osd_ == nullptr || staging_epoch_ == 0) {
    return nullptr;
  }
  return osd_->get_pg(pool_id_, pg_id_, staging_epoch_);
}

void snapshot_ingest::begin(uint64_t snap_index, uint64_t snap_term,
                            op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (osd_ == nullptr || snap_index == 0) {
    cb(arg, -EINVAL);
    return;
  }
  if (state_ != ingest_state::idle) {
    cb(arg, -EBUSY);
    return;
  }

  active_epoch_ = osd_->serving_epoch(pool_id_, pg_id_);
  if (active_epoch_ == 0) {
    cb(arg, -ENOENT);
    return;
  }

  snap_index_ = snap_index;
  snap_term_ = snap_term;
  chunk_seq_ = 0;

  auto* p = new (std::nothrow) cb_pack{this, std::move(cb), arg};
  if (p == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }

  osd_->create_pg(
      pool_id_, pg_id_,
      [](void* a, int err) {
        auto* pack = static_cast<cb_pack*>(a);
        snapshot_ingest* self = pack->self;
        op_complete c = std::move(pack->cb);
        void* user = pack->arg;
        delete pack;
        if (err != 0) {
          c(user, err);
          return;
        }
        self->staging_epoch_ =
            self->osd_->latest_epoch(self->pool_id_, self->pg_id_);
        // Keep serving on old active during ingest.
        self->osd_->set_serving_epoch(self->pool_id_, self->pg_id_,
                                      self->active_epoch_);
        self->state_ = ingest_state::ingesting;
        c(user, 0);
      },
      p);
}

void snapshot_ingest::apply_chunk(const std::string& object_name,
                                  uint64_t offset, const void* data,
                                  uint64_t len, uint32_t expected_crc32,
                                  op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (state_ != ingest_state::ingesting) {
    cb(arg, -EINVAL);
    return;
  }
  pg_store* st = staging_pg();
  if (st == nullptr || data == nullptr || object_name.empty()) {
    cb(arg, -EINVAL);
    return;
  }
  if (expected_crc32 != 0) {
    const uint32_t got = crc32(data, static_cast<size_t>(len));
    if (got != expected_crc32) {
      cb(arg, -EBADMSG);
      return;
    }
  }
  // Distinct raft indices per chunk for idempotency under snap_index base.
  ++chunk_seq_;
  const uint64_t raft_idx = snap_index_ * 1000000ull + chunk_seq_;
  st->write(object_name, offset, data, len, raft_idx, /*image*/ 0,
            std::move(cb), arg);
}

void snapshot_ingest::seal(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (state_ != ingest_state::ingesting) {
    cb(arg, -EINVAL);
    return;
  }
  pg_store* st = staging_pg();
  if (st == nullptr) {
    cb(arg, -ENOENT);
    return;
  }

  auto* p = new (std::nothrow) cb_pack{this, std::move(cb), arg};
  if (p == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }
  st->mark_install_seal(
      snap_index_, snap_term_,
      [](void* a, int err) {
        auto* pack = static_cast<cb_pack*>(a);
        snapshot_ingest* self = pack->self;
        op_complete c = std::move(pack->cb);
        void* user = pack->arg;
        delete pack;
        if (err == 0) {
          self->state_ = ingest_state::sealed;
        }
        c(user, err);
      },
      p);
}

void snapshot_ingest::activate(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (state_ != ingest_state::sealed) {
    cb(arg, -EINVAL);
    return;
  }
  pg_store* st = staging_pg();
  if (st == nullptr) {
    cb(arg, -ENOENT);
    return;
  }

  auto* p = new (std::nothrow) cb_pack{this, std::move(cb), arg};
  if (p == nullptr) {
    cb(arg, -ENOMEM);
    return;
  }
  st->mark_install_activate(
      snap_index_,
      [](void* a, int err) {
        auto* pack = static_cast<cb_pack*>(a);
        snapshot_ingest* self = pack->self;
        op_complete c = std::move(pack->cb);
        void* user = pack->arg;
        delete pack;
        if (err != 0) {
          c(user, err);
          return;
        }
        // Promote staging to serving; drop old active epoch from memory.
        const uint64_t old = self->active_epoch_;
        self->osd_->set_serving_epoch(self->pool_id_, self->pg_id_,
                                      self->staging_epoch_);
        self->active_epoch_ = self->staging_epoch_;
        if (old != 0 && old != self->staging_epoch_) {
          self->osd_->drop_pg_epoch(self->pool_id_, self->pg_id_, old);
        }
        self->state_ = ingest_state::catchup;
        c(user, 0);
      },
      p);
}

void snapshot_ingest::finish_catchup(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (state_ != ingest_state::catchup) {
    cb(arg, -EINVAL);
    return;
  }
  state_ = ingest_state::idle;
  staging_epoch_ = 0;
  snap_index_ = 0;
  snap_term_ = 0;
  chunk_seq_ = 0;
  cb(arg, 0);
}

void snapshot_ingest::abort(op_complete cb, void* arg) {
  if (cb == nullptr) {
    return;
  }
  if (state_ == ingest_state::idle) {
    cb(arg, 0);
    return;
  }
  if (state_ == ingest_state::catchup) {
    // Already switched; abort is no longer safe without full rollback.
    cb(arg, -EBUSY);
    return;
  }

  if (staging_epoch_ != 0) {
    osd_->drop_pg_epoch(pool_id_, pg_id_, staging_epoch_);
    // Roll latest back to active if it pointed at staging.
    if (osd_->latest_epoch(pool_id_, pg_id_) == staging_epoch_) {
      osd_->set_latest_epoch(pool_id_, pg_id_, active_epoch_);
    }
  }
  osd_->set_serving_epoch(pool_id_, pg_id_, active_epoch_);
  staging_epoch_ = 0;
  snap_index_ = 0;
  snap_term_ = 0;
  chunk_seq_ = 0;
  state_ = ingest_state::idle;
  cb(arg, 0);
}

} // namespace fastblock::aos
