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

#include <cstdint>
#include <string>

namespace fastblock::aos {

// Deprecated dual-engine modes (knife-1: object is AOS-only). Kept for parse helpers.
enum class object_engine_mode : uint8_t {
  blob = 0,
  aos = 1,
  shadow = 2,
};

// Object IO policy
inline constexpr uint64_t kAlign = 4096;
inline constexpr uint64_t kMaxObjectSize = 4ull * 1024 * 1024;

inline constexpr uint32_t kRecordMagic = 0x534f4146u; // 'FAOS' LE
// v2: platform record types include log/kv (design v2 §8)
inline constexpr uint16_t kFormatVersion = 2;

enum class record_type : uint8_t {
  data = 1,
  delete_tombstone = 2,
  gc_rewrite = 3,
  object_name = 4,
  segment_seal = 5,
  range_tombstone = 6,
  checkpoint_marker = 7,
  raft_snapshot_pin = 8,
  raft_snapshot_unpin = 9,
  image_snapshot_pin = 10,
  image_snapshot_unpin = 11,
  install_seal = 12,
  install_activate = 13,
  // design v2 §6 log_engine
  // payload: term,u64 index,u64 type,u64 meta_len,u32 meta, data
  // object_id unused; raft_apply_index = log index; image_snap_seq = term (dup)
  raft_log_entry = 14,
  // design v2 §7 kv_engine
  kv_put = 15,        // object_id unused; payload key\0value or len-prefixed
  kv_delete = 16,     // payload key
  kv_checkpoint = 17, // payload full table snapshot
};

// Follower install-snapshot state machine (design §10.4).
enum class ingest_state : uint8_t {
  idle = 0,       // serving active generation only
  ingesting = 1,  // staging epoch receiving chunks
  sealed = 2,     // staging sealed, not yet switched
  catchup = 3,    // active switched; post-snap appends allowed
};

struct pg_id {
  uint64_t pool_id{0};
  uint64_t pg_id{0};
  uint64_t store_epoch{0};
};

enum class buffer_mode {
  heap = 0,     // unit tests / mem backend
  spdk_dma = 1, // production SPDK reactor
};

inline bool is_aligned(uint64_t offset, uint64_t len) {
  return (offset % kAlign == 0) && (len % kAlign == 0);
}

inline bool valid_object_range(uint64_t offset, uint64_t len) {
  if (len == 0) {
    return false;
  }
  if (!is_aligned(offset, len)) {
    return false;
  }
  if (offset >= kMaxObjectSize) {
    return false;
  }
  if (len > kMaxObjectSize - offset) {
    return false;
  }
  return true;
}

} // namespace fastblock::aos
