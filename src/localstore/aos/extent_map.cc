/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "extent_map.h"

#include <cassert>

namespace fastblock::aos {

void extent_map::put(const extent& e) {
  assert(valid_object_range(e.logical_offset, e.length));
  discard(e.logical_offset, e.length);
  extents_[e.logical_offset] = e;
}

void extent_map::discard(uint64_t offset, uint64_t len) {
  assert(valid_object_range(offset, len));
  const uint64_t end = offset + len;

  // Find first extent that may overlap: largest key <= offset, or begin.
  auto it = extents_.upper_bound(offset);
  if (it != extents_.begin()) {
    --it;
  }

  std::vector<extent> to_add;
  while (it != extents_.end()) {
    extent cur = it->second;
    const uint64_t cur_end = cur.logical_offset + cur.length;
    if (cur.logical_offset >= end) {
      break;
    }
    if (cur_end <= offset) {
      ++it;
      continue;
    }

    // Overlap: remove cur and keep left/right remnants.
    auto erase_it = it++;
    extents_.erase(erase_it);

    if (cur.logical_offset < offset) {
      extent left = cur;
      left.length = offset - cur.logical_offset;
      to_add.push_back(left);
    }
    if (cur_end > end) {
      extent right = cur;
      const uint64_t cut = end - cur.logical_offset;
      right.logical_offset = end;
      right.length = cur_end - end;
      // Physical pointer advances for right remnant only if we track payload
      // alignment in file backend; for M1 map logic, keep phys_offset as-is
      // (logical-only tests). File backend will set phys on put.
      right.phys_offset = cur.phys_offset + cut;
      to_add.push_back(right);
    }
  }

  for (const auto& e : to_add) {
    extents_[e.logical_offset] = e;
  }
}

std::vector<extent> extent_map::query(uint64_t offset, uint64_t len) const {
  assert(valid_object_range(offset, len));
  const uint64_t end = offset + len;
  std::vector<extent> out;

  auto it = extents_.upper_bound(offset);
  if (it != extents_.begin()) {
    --it;
  }

  for (; it != extents_.end(); ++it) {
    const extent& cur = it->second;
    const uint64_t cur_end = cur.logical_offset + cur.length;
    if (cur.logical_offset >= end) {
      break;
    }
    if (cur_end <= offset) {
      continue;
    }
    out.push_back(cur);
  }
  return out;
}

std::vector<extent> extent_map::all() const {
  std::vector<extent> out;
  out.reserve(extents_.size());
  for (const auto& kv : extents_) {
    out.push_back(kv.second);
  }
  return out;
}

} // namespace fastblock::aos
