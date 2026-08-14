/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "superblock.h"
#include "crc32.h"

#include <cstring>

namespace fastblock::aos {
namespace {

void put_u16(std::vector<uint8_t>& o, uint16_t v) {
  o.push_back(static_cast<uint8_t>(v & 0xff));
  o.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}
void put_u32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}
void put_u64(std::vector<uint8_t>& o, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
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

// Fixed header before CRC: magic(4)+ver(2)+flags(2)+5*u64 = 4+2+2+40 = 48
// + crc(4) = 52
constexpr size_t kSbHeaderBytes = 52;

} // namespace

std::vector<uint8_t> encode_superblock(const superblock& sb) {
  std::vector<uint8_t> o;
  o.reserve(static_cast<size_t>(kSuperblockBytes));
  put_u32(o, kSuperblockMagic);
  put_u16(o, kLayoutVersion);
  put_u16(o, 0);
  put_u64(o, sb.generation);
  put_u64(o, sb.segment_size);
  put_u64(o, sb.data_offset);
  put_u64(o, sb.data_length);
  put_u64(o, sb.manifest_generation);
  const uint32_t crc = crc32(o.data(), o.size());
  put_u32(o, crc);
  o.resize(static_cast<size_t>(kSuperblockBytes), 0);
  return o;
}

bool decode_superblock(const uint8_t* buf, size_t len, superblock* out) {
  if (out == nullptr || buf == nullptr || len < kSbHeaderBytes) {
    return false;
  }
  if (get_u32(buf) != kSuperblockMagic) {
    return false;
  }
  if (get_u16(buf + 4) != kLayoutVersion) {
    return false;
  }
  const uint32_t expect = get_u32(buf + 48);
  const uint32_t actual = crc32(buf, 48);
  if (expect != actual) {
    return false;
  }
  out->generation = get_u64(buf + 8);
  out->segment_size = get_u64(buf + 16);
  out->data_offset = get_u64(buf + 24);
  out->data_length = get_u64(buf + 32);
  out->manifest_generation = get_u64(buf + 40);
  return true;
}

bool prefer_superblock(const superblock& a, bool a_ok, const superblock& b,
                       bool b_ok, superblock* best) {
  if (!a_ok && !b_ok) {
    return false;
  }
  if (a_ok && !b_ok) {
    *best = a;
    return true;
  }
  if (!a_ok && b_ok) {
    *best = b;
    return true;
  }
  *best = (a.generation >= b.generation) ? a : b;
  return true;
}

std::vector<uint8_t> encode_manifest(const manifest& m) {
  const bool v2 = m.recover_scan_from != 0 || m.kv_checkpoint_phys != 0 ||
                  !m.object_checkpoints.empty() || !m.log_checkpoints.empty() ||
                  !m.segment_states.empty();
  std::vector<uint8_t> o;
  o.reserve(96 + m.pgs.size() * 24 + m.segment_states.size());
  put_u32(o, kManifestMagic);
  put_u16(o, kLayoutVersion);
  put_u16(o, v2 ? kManifestFlagV2 : 0);
  put_u64(o, m.generation);
  put_u64(o, m.data_high_water);
  put_u32(o, m.open_segment_id);
  put_u32(o, 0); // pad
  put_u64(o, m.open_cursor);
  put_u32(o, static_cast<uint32_t>(m.pgs.size()));
  put_u32(o, 0); // reserved
  for (const auto& e : m.pgs) {
    put_u64(o, e.pool_id);
    put_u64(o, e.pg_id);
    put_u64(o, e.active_epoch);
  }
  if (v2) {
    put_u64(o, m.recover_scan_from);
    put_u64(o, m.kv_checkpoint_phys);
    put_u32(o, static_cast<uint32_t>(m.object_checkpoints.size()));
    put_u32(o, static_cast<uint32_t>(m.log_checkpoints.size()));
    put_u32(o, static_cast<uint32_t>(m.segment_states.size()));
    put_u32(o, 0);
    for (const auto& c : m.object_checkpoints) {
      put_u64(o, c.pool_id);
      put_u64(o, c.pg_id);
      put_u64(o, c.store_epoch);
      put_u64(o, c.phys);
    }
    for (const auto& c : m.log_checkpoints) {
      put_u64(o, c.pool_id);
      put_u64(o, c.pg_id);
      put_u64(o, c.store_epoch);
      put_u64(o, c.phys);
    }
    o.insert(o.end(), m.segment_states.begin(), m.segment_states.end());
    while ((o.size() % 4) != 0) {
      o.push_back(0);
    }
  }
  const uint32_t crc = crc32(o.data(), o.size());
  put_u32(o, crc);
  return o;
}

bool decode_manifest(const uint8_t* buf, size_t len, manifest* out) {
  if (out == nullptr || buf == nullptr || len < 48) {
    return false;
  }
  if (get_u32(buf) != kManifestMagic) {
    return false;
  }
  if (get_u16(buf + 4) != kLayoutVersion) {
    return false;
  }
  const uint16_t flags = get_u16(buf + 6);
  size_t pos = 8;
  out->generation = get_u64(buf + pos);
  pos += 8;
  out->data_high_water = get_u64(buf + pos);
  pos += 8;
  out->open_segment_id = get_u32(buf + pos);
  pos += 4;
  pos += 4; // pad
  out->open_cursor = get_u64(buf + pos);
  pos += 8;
  const uint32_t n = get_u32(buf + pos);
  pos += 4;
  pos += 4; // reserved
  if (len < pos + static_cast<size_t>(n) * 24 + 4) {
    return false;
  }
  out->pgs.clear();
  out->pgs.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    manifest_pg_entry e;
    e.pool_id = get_u64(buf + pos);
    e.pg_id = get_u64(buf + pos + 8);
    e.active_epoch = get_u64(buf + pos + 16);
    pos += 24;
    out->pgs.push_back(e);
  }
  out->recover_scan_from = 0;
  out->kv_checkpoint_phys = 0;
  out->object_checkpoints.clear();
  out->log_checkpoints.clear();
  out->segment_states.clear();

  if ((flags & kManifestFlagV2) != 0) {
    if (len < pos + 8 + 8 + 16 + 4) {
      return false;
    }
    out->recover_scan_from = get_u64(buf + pos);
    pos += 8;
    out->kv_checkpoint_phys = get_u64(buf + pos);
    pos += 8;
    const uint32_t nobj = get_u32(buf + pos);
    pos += 4;
    const uint32_t nlog = get_u32(buf + pos);
    pos += 4;
    const uint32_t nseg = get_u32(buf + pos);
    pos += 4;
    pos += 4;
    const size_t ckpt_bytes =
        (static_cast<size_t>(nobj) + nlog) * 32 + nseg;
    const size_t pad = (4 - (ckpt_bytes % 4)) % 4;
    if (len < pos + ckpt_bytes + pad + 4) {
      return false;
    }
    out->object_checkpoints.reserve(nobj);
    for (uint32_t i = 0; i < nobj; ++i) {
      manifest_ckpt c;
      c.pool_id = get_u64(buf + pos);
      c.pg_id = get_u64(buf + pos + 8);
      c.store_epoch = get_u64(buf + pos + 16);
      c.phys = get_u64(buf + pos + 24);
      pos += 32;
      out->object_checkpoints.push_back(c);
    }
    out->log_checkpoints.reserve(nlog);
    for (uint32_t i = 0; i < nlog; ++i) {
      manifest_ckpt c;
      c.pool_id = get_u64(buf + pos);
      c.pg_id = get_u64(buf + pos + 8);
      c.store_epoch = get_u64(buf + pos + 16);
      c.phys = get_u64(buf + pos + 24);
      pos += 32;
      out->log_checkpoints.push_back(c);
    }
    out->segment_states.assign(buf + pos, buf + pos + nseg);
    pos += nseg + pad;
  }

  if (len < pos + 4) {
    return false;
  }
  const uint32_t expect = get_u32(buf + pos);
  const uint32_t actual = crc32(buf, pos);
  if (expect != actual) {
    return false;
  }
  return true;
}

} // namespace fastblock::aos
