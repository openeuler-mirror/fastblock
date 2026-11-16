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

#include "device_backend.h"
#include "segment.h"
#include "segment_allocator.h"
#include "types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Forward declare at global scope so the member type is ::spdk_poller*, not
// an injected fastblock::aos::spdk_poller.
struct spdk_poller;

namespace fastblock::aos {

/**
 * Shard-level KV engine on AOS (design v2 §7).
 *
 * Semantics match legacy kvstore for raft:
 *   put/remove — stage in op_log (sync API)
 *   get — memory table only (committed state)
 *   poller — commit WAL; occasional full-table checkpoint record
 *
 * Records:
 *   kv_put / kv_delete — single op WAL
 *   kv_checkpoint — full table snapshot (replaces need to replay older WAL for recover)
 *
 * Uses reserved pg_id namespace: pool=0, pg=0, epoch=1 for all KV records.
 */
class kv_engine {
public:
  using op_complete = device_backend::op_complete;

  kv_engine(device_backend* backend, std::shared_ptr<segment_allocator> alloc,
            buffer_mode mode = buffer_mode::heap);

  kv_engine(const kv_engine&) = delete;
  kv_engine& operator=(const kv_engine&) = delete;
  ~kv_engine();

  void start();
  void stop(op_complete cb, void* arg);

  void put(std::string key, std::optional<std::string> value);
  void remove(std::string key);
  std::optional<std::string> get(const std::string& key) const;

  void commit(op_complete cb, void* arg);

  /** Apply one decoded kv record during device recover. */
  int apply_record(const data_record& rec);

  /** Rebuild from contiguous image (tests). */
  uint64_t rebuild_from_image(const uint8_t* image, size_t len);

  size_t table_size() const { return table_.size(); }
  size_t pending_ops() const { return op_log_.size(); }

  static constexpr uint64_t kKvPoolId = 0;
  static constexpr uint64_t kKvPgId = 0;
  static constexpr uint64_t kKvEpoch = 1;

private:
  struct op {
    std::string key;
    std::optional<std::string> value; // nullopt = delete
  };

  void apply_op(const std::string& key, const std::optional<std::string>& value);
  void* alloc_buf(uint64_t n);
  void free_buf(void* p);
  void write_record(data_record rec, op_complete cb, void* arg);
  void commit_one(op o, op_complete cb, void* arg);
  void maybe_checkpoint(op_complete cb, void* arg);
  static std::string encode_put_payload(const std::string& key,
                                        const std::string& value);
  static std::string encode_del_payload(const std::string& key);
  static std::string encode_checkpoint_payload(
      const std::map<std::string, std::string>& table);
  static bool decode_put_payload(const std::string& p, std::string* key,
                                 std::string* value);
  static bool decode_del_payload(const std::string& p, std::string* key);
  static bool decode_checkpoint_payload(const std::string& p,
                                        std::map<std::string, std::string>* table);

  static int worker_poll(void* arg);
  bool maybe_work();

  device_backend* backend_{nullptr};
  buffer_mode mode_{buffer_mode::heap};
  std::shared_ptr<segment_allocator> alloc_;
  segment writer_;

  std::map<std::string, std::string> table_;
  std::vector<op> op_log_;
  bool working_{false};
  struct spdk_poller* poller_{nullptr};

  // After checkpoint, older WAL is obsolete for recover if we only load
  // latest checkpoint then subsequent puts — recover applies all in order.
  uint64_t ops_since_checkpoint_{0};
  static constexpr uint64_t kCheckpointEvery = 64;
};

} // namespace fastblock::aos
