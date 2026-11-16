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

#include "extent_map.h"
#include "types.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fastblock::aos {

// Payload magic inside checkpoint_marker record payload: 'ACKP' LE
inline constexpr uint32_t kCheckpointPayloadMagic = 0x504b4341u;
inline constexpr uint16_t kCheckpointPayloadVersion = 1;

struct checkpoint_extent {
  uint64_t logical_offset{0};
  uint64_t length{0};
  uint64_t raft_create_index{0};
  uint64_t image_create_seq{0};
  uint64_t phys_segment{0};
  uint64_t phys_offset{0};
};

// applied op: (offset, len) -> raft_apply_index
using applied_op = std::pair<std::pair<uint64_t, uint64_t>, uint64_t>;

struct checkpoint_object {
  uint64_t object_id{0};
  std::string name;
  bool deleted{false};
  uint64_t delete_raft_index{0};
  std::vector<checkpoint_extent> extents;
  std::vector<applied_op> applied_writes;
  std::vector<applied_op> applied_discards;
};

/**
 * In-memory PG ObjectMap snapshot for checkpoint_marker payload.
 * Not the sole source of truth — records after the marker still replay.
 */
struct checkpoint_snapshot {
  uint64_t generation{0};
  uint64_t pool_id{0};
  uint64_t pg_id{0};
  uint64_t store_epoch{0};
  uint64_t next_object_id{1};
  std::vector<checkpoint_object> objects;
};

// Encode / decode payload only (record header is separate).
std::string encode_checkpoint_payload(const checkpoint_snapshot& snap);
bool decode_checkpoint_payload(const std::string& payload,
                               checkpoint_snapshot* out);

} // namespace fastblock::aos
