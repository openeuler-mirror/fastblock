/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "checkpoint.h"
#include "crc32.h"

#include <cstring>

namespace fastblock::aos {
namespace {

void put_u16(std::string& o, uint16_t v) {
  o.push_back(static_cast<char>(v & 0xff));
  o.push_back(static_cast<char>((v >> 8) & 0xff));
}

void put_u32(std::string& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    o.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
}

void put_u64(std::string& o, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    o.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
}

bool need(size_t pos, size_t need_n, size_t len) {
  return pos + need_n <= len;
}

uint16_t get_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t get_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

void put_applied(std::string& o, const std::vector<applied_op>& ops) {
  put_u32(o, static_cast<uint32_t>(ops.size()));
  for (const auto& op : ops) {
    put_u64(o, op.first.first);
    put_u64(o, op.first.second);
    put_u64(o, op.second);
  }
}

bool get_applied(const uint8_t* base, size_t len, size_t* pos,
                 std::vector<applied_op>* out) {
  if (!need(*pos, 4, len)) {
    return false;
  }
  const uint32_t n = get_u32(base + *pos);
  *pos += 4;
  out->clear();
  out->reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    if (!need(*pos, 24, len)) {
      return false;
    }
    const uint64_t off = get_u64(base + *pos);
    const uint64_t l = get_u64(base + *pos + 8);
    const uint64_t raft = get_u64(base + *pos + 16);
    *pos += 24;
    out->push_back({{off, l}, raft});
  }
  return true;
}

} // namespace

std::string encode_checkpoint_payload(const checkpoint_snapshot& snap) {
  std::string o;
  o.reserve(256 + snap.objects.size() * 128);

  put_u32(o, kCheckpointPayloadMagic);
  put_u16(o, kCheckpointPayloadVersion);
  put_u16(o, 0); // flags
  put_u64(o, snap.generation);
  put_u64(o, snap.pool_id);
  put_u64(o, snap.pg_id);
  put_u64(o, snap.store_epoch);
  put_u64(o, snap.next_object_id);
  put_u32(o, static_cast<uint32_t>(snap.objects.size()));
  put_u32(o, 0); // reserved

  for (const auto& obj : snap.objects) {
    put_u64(o, obj.object_id);
    put_u32(o, static_cast<uint32_t>(obj.name.size()));
    o.push_back(obj.deleted ? 1 : 0);
    o.push_back(0);
    o.push_back(0);
    o.push_back(0);
    put_u64(o, obj.delete_raft_index);
    o.append(obj.name);
    put_u32(o, static_cast<uint32_t>(obj.extents.size()));
    for (const auto& e : obj.extents) {
      put_u64(o, e.logical_offset);
      put_u64(o, e.length);
      put_u64(o, e.raft_create_index);
      put_u64(o, e.image_create_seq);
      put_u64(o, e.phys_segment);
      put_u64(o, e.phys_offset);
    }
    put_applied(o, obj.applied_writes);
    put_applied(o, obj.applied_discards);
  }

  // Trailing payload CRC over everything so far (detect half-written payload
  // when embedded in a corrupt larger buffer; record CRC also covers this).
  const uint32_t crc =
      crc32(o.data(), o.size());
  put_u32(o, crc);
  return o;
}

bool decode_checkpoint_payload(const std::string& payload,
                               checkpoint_snapshot* out) {
  if (out == nullptr) {
    return false;
  }
  const auto* base = reinterpret_cast<const uint8_t*>(payload.data());
  const size_t len = payload.size();
  // header + crc
  if (len < 4 + 2 + 2 + 8 * 5 + 4 + 4 + 4) {
    return false;
  }

  // Verify trailing CRC
  const uint32_t expect_crc = get_u32(base + len - 4);
  const uint32_t actual_crc = crc32(base, len - 4);
  if (expect_crc != actual_crc) {
    return false;
  }

  size_t pos = 0;
  if (get_u32(base + pos) != kCheckpointPayloadMagic) {
    return false;
  }
  pos += 4;
  if (get_u16(base + pos) != kCheckpointPayloadVersion) {
    return false;
  }
  pos += 2;
  pos += 2; // flags
  out->generation = get_u64(base + pos);
  pos += 8;
  out->pool_id = get_u64(base + pos);
  pos += 8;
  out->pg_id = get_u64(base + pos);
  pos += 8;
  out->store_epoch = get_u64(base + pos);
  pos += 8;
  out->next_object_id = get_u64(base + pos);
  pos += 8;
  const uint32_t nobj = get_u32(base + pos);
  pos += 4;
  pos += 4; // reserved

  out->objects.clear();
  out->objects.reserve(nobj);
  for (uint32_t i = 0; i < nobj; ++i) {
    if (!need(pos, 8 + 4 + 4 + 8, len - 4)) {
      return false;
    }
    checkpoint_object obj;
    obj.object_id = get_u64(base + pos);
    pos += 8;
    const uint32_t name_len = get_u32(base + pos);
    pos += 4;
    obj.deleted = base[pos] != 0;
    pos += 4; // deleted + pad
    obj.delete_raft_index = get_u64(base + pos);
    pos += 8;
    if (!need(pos, name_len, len - 4)) {
      return false;
    }
    obj.name.assign(reinterpret_cast<const char*>(base + pos), name_len);
    pos += name_len;
    if (!need(pos, 4, len - 4)) {
      return false;
    }
    const uint32_t next = get_u32(base + pos);
    pos += 4;
    obj.extents.reserve(next);
    for (uint32_t j = 0; j < next; ++j) {
      if (!need(pos, 48, len - 4)) {
        return false;
      }
      checkpoint_extent e;
      e.logical_offset = get_u64(base + pos);
      e.length = get_u64(base + pos + 8);
      e.raft_create_index = get_u64(base + pos + 16);
      e.image_create_seq = get_u64(base + pos + 24);
      e.phys_segment = get_u64(base + pos + 32);
      e.phys_offset = get_u64(base + pos + 40);
      pos += 48;
      obj.extents.push_back(e);
    }
    if (!get_applied(base, len - 4, &pos, &obj.applied_writes)) {
      return false;
    }
    if (!get_applied(base, len - 4, &pos, &obj.applied_discards)) {
      return false;
    }
    out->objects.push_back(std::move(obj));
  }

  // pos should land on trailing CRC
  if (pos != len - 4) {
    return false;
  }
  return true;
}

} // namespace fastblock::aos
