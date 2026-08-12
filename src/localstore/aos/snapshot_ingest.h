/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#pragma once

#include "osd_store.h"
#include "types.h"

#include <cstdint>
#include <string>

namespace fastblock::aos {

/**
 * Follower install-snapshot ingest state machine (design §10.4 / M5).
 *
 *   idle -> ingesting(snap_index) -> sealed -> catchup -> idle
 *
 * Staging uses a new store_epoch (osd_store::create_pg). Active reads continue
 * on the previous epoch until activate(). Unsealed staging is discarded on
 * abort() or osd_store::drop_unsealed_staging() after recover.
 *
 * Reactor-thread only; callbacks complete async (mem backend is inline).
 */
class snapshot_ingest {
public:
  using op_complete = device_backend::op_complete;

  snapshot_ingest(osd_store* osd, uint64_t pool_id, uint64_t pg_id);

  ingest_state state() const { return state_; }
  uint64_t snapshot_index() const { return snap_index_; }
  uint64_t snapshot_term() const { return snap_term_; }
  uint64_t active_epoch() const { return active_epoch_; }
  uint64_t staging_epoch() const { return staging_epoch_; }

  pg_store* active_pg() const;
  pg_store* staging_pg() const;

  /**
   * idle -> ingesting. Creates staging store_epoch = latest+1.
   * snap_index must be > 0.
   */
  void begin(uint64_t snap_index, uint64_t snap_term, op_complete cb,
             void* arg);

  /**
   * Append one object extent into staging. Optional CRC32 check (0 = skip).
   * raft_apply_index on write = snap_index. Only valid in ingesting.
   */
  void apply_chunk(const std::string& object_name, uint64_t offset,
                   const void* data, uint64_t len, uint32_t expected_crc32,
                   op_complete cb, void* arg);

  /** ingesting -> sealed. Durable install_seal on staging. */
  void seal(op_complete cb, void* arg);

  /**
   * sealed -> catchup. Marks staging active (serving), durable activate,
   * drops old active epoch from memory. Post-snap appends go to new active.
   */
  void activate(op_complete cb, void* arg);

  /** catchup -> idle. Install complete. */
  void finish_catchup(op_complete cb, void* arg);

  /**
   * Abort ingesting/sealed: drop staging, restore serving to prior active.
   * No-op success if already idle.
   */
  void abort(op_complete cb, void* arg);

private:
  osd_store* osd_{nullptr};
  uint64_t pool_id_{0};
  uint64_t pg_id_{0};
  ingest_state state_{ingest_state::idle};
  uint64_t snap_index_{0};
  uint64_t snap_term_{0};
  uint64_t active_epoch_{0};
  uint64_t staging_epoch_{0};
  uint64_t chunk_seq_{0}; // monotonic for idempotency of chunk writes
};

} // namespace fastblock::aos
