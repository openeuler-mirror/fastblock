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

#include "types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace fastblock::aos {

// M1: head-only extent map (no historical snapshot chain yet).
// Physical location is abstract for unit tests.
struct extent {
  uint64_t logical_offset{0};
  uint64_t length{0};
  uint64_t raft_create_index{0};
  uint64_t image_create_seq{0};
  // File-backend placeholder: segment id + offset inside segment.
  uint64_t phys_segment{0};
  uint64_t phys_offset{0};
};

// Ordered non-overlapping extents by logical_offset.
class extent_map {
public:
  // Insert/overwrite a range. Splits and removes covered extents.
  // Requires valid_object_range(offset, len).
  void put(const extent& e);

  // Range tombstone: remove coverage for [offset, offset+len).
  void discard(uint64_t offset, uint64_t len);

  // Collect extents that intersect [offset, offset+len), ordered.
  // Holes are not represented; caller zero-fills.
  std::vector<extent> query(uint64_t offset, uint64_t len) const;

  // Snapshot all extents in logical_offset order (for checkpoint).
  std::vector<extent> all() const;

  size_t size() const { return extents_.size(); }
  void clear() { extents_.clear(); }

private:
  // key = logical_offset
  std::map<uint64_t, extent> extents_;
};

} // namespace fastblock::aos
