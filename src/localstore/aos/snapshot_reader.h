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

#include "pg_store.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fastblock::aos {

/**
 * Leader-side raft snapshot streaming reader (design §10.3 / M5 skeleton).
 *
 * - open() pins the raft snapshot index (O(1) pin record).
 * - next_chunk() streams object extents at that snapshot view.
 * - close() releases the pin (must be called on cancel/done/stepdown).
 *
 * Reactor-thread only; completions are async via callbacks.
 */
class pg_snapshot_reader {
public:
  using op_complete = device_backend::op_complete;

  using open_complete = void (*)(void* arg, int err,
                                 pg_snapshot_reader* reader);

  /**
   * Chunk callback: err!=0 failure; done=true means stream exhausted
   * (name/offset/len undefined). On success with done=false, [buf,len)
   * holds payload for object_name at logical offset.
   */
  using chunk_complete = void (*)(void* arg, int err, bool done,
                                  const char* object_name, uint64_t offset,
                                  uint64_t len);

  /**
   * Create reader and pin raft snapshot. On success cb receives owned
   * reader pointer; caller must close() then delete (or use unique_ptr
   * with custom deleter calling close — M5 keeps explicit close+delete).
   */
  static void open(pg_store* store, uint64_t raft_snapshot_index,
                   uint64_t raft_term, open_complete cb, void* arg);

  ~pg_snapshot_reader();

  pg_snapshot_reader(const pg_snapshot_reader&) = delete;
  pg_snapshot_reader& operator=(const pg_snapshot_reader&) = delete;

  uint64_t raft_index() const { return raft_index_; }
  pg_store* store() const { return store_; }
  bool closed() const { return closed_; }
  size_t remaining_chunks() const {
    return cursor_ >= plan_.size() ? 0 : plan_.size() - cursor_;
  }
  size_t total_chunks() const { return plan_.size(); }

  /**
   * Read next extent into caller buffer. buf must hold at least extent len
   * (known only after callback — for skeleton, buf_cap must be >= 4MiB max
   * object extent or the callback returns -ENOSPC if too small).
   */
  void next_chunk(void* buf, uint64_t buf_cap, chunk_complete cb, void* arg);

  /** Release pin. Safe to call once; further next_chunk returns -EINVAL. */
  void close(op_complete cb, void* arg);

private:
  struct plan_item {
    std::string name;
    uint64_t offset{0};
    uint64_t len{0};
  };

  pg_snapshot_reader(pg_store* store, uint64_t raft_index);

  void build_plan();

  pg_store* store_{nullptr};
  uint64_t raft_index_{0};
  std::vector<plan_item> plan_;
  size_t cursor_{0};
  bool closed_{false};
  bool busy_{false};
};

} // namespace fastblock::aos
