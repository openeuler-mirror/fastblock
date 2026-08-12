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

// M1 IO policy (design §7.4)
inline constexpr uint64_t kAlign = 4096;
inline constexpr uint64_t kMaxObjectSize = 4ull * 1024 * 1024;

inline constexpr uint32_t kRecordMagic = 0x534f4146u; // 'FAOS' LE
inline constexpr uint16_t kFormatVersion = 1;

enum class record_type : uint8_t {
  data = 1,
  delete_tombstone = 2,
  gc_rewrite = 3,
  object_name = 4,
  segment_seal = 5,
  // range tombstone: object_offset + payload u64 LE length (design §7.3)
  range_tombstone = 6,
  // per-PG ObjectMap snapshot (design §12.1); payload is checkpoint_snapshot
  checkpoint_marker = 7,
  // metadata_delta intentionally unused in M1
};

struct pg_id {
  uint64_t pool_id{0};
  uint64_t pg_id{0};
  uint64_t store_epoch{0};
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
