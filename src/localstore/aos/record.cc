/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "record.h"
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
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

} // namespace

std::vector<uint8_t> encode_record(const data_record& rec) {
  std::vector<uint8_t> out;
  out.reserve(kRecordHeaderLenV1 + rec.payload.size());

  put_u32(out, kRecordMagic);
  put_u16(out, kFormatVersion);
  out.push_back(static_cast<uint8_t>(rec.type));
  out.push_back(rec.flags);
  put_u16(out, kRecordHeaderLenV1);
  put_u32(out, static_cast<uint32_t>(rec.payload.size()));
  put_u64(out, rec.pool_id);
  put_u64(out, rec.pg_id);
  put_u64(out, rec.store_epoch);
  put_u64(out, rec.object_id);
  put_u64(out, rec.object_offset);
  put_u64(out, rec.raft_apply_index);
  put_u64(out, rec.image_snap_seq);

  // header_checksum covers everything written so far (no checksum fields yet)
  const uint32_t header_crc = crc32(out.data(), out.size());
  put_u32(out, header_crc);

  const uint32_t payload_crc =
      rec.payload.empty()
          ? 0
          : crc32(rec.payload.data(), rec.payload.size());
  put_u32(out, payload_crc);

  if (!rec.payload.empty()) {
    out.insert(out.end(), rec.payload.begin(), rec.payload.end());
  }
  return out;
}

decode_status decode_record(const uint8_t* buf, size_t len, data_record* out,
                            size_t* consumed) {
  if (consumed) {
    *consumed = 0;
  }
  if (len < kRecordHeaderLenV1) {
    return decode_status::need_more;
  }

  if (get_u32(buf) != kRecordMagic) {
    return decode_status::bad_magic;
  }
  if (get_u16(buf + 4) != kFormatVersion) {
    return decode_status::bad_version;
  }

  const auto type = static_cast<record_type>(buf[6]);
  const uint8_t flags = buf[7];
  const uint16_t header_len = get_u16(buf + 8);
  const uint32_t payload_len = get_u32(buf + 10);

  if (header_len != kRecordHeaderLenV1) {
    return decode_status::bad_header_len;
  }
  if (len < static_cast<size_t>(header_len) + payload_len) {
    return decode_status::need_more;
  }

  // header_checksum is last 8 bytes of fixed header: [header_crc][payload_crc]
  // Fields before checksums: header_len - 8
  const size_t pre_crc_len = static_cast<size_t>(header_len) - 8;
  const uint32_t expect_header_crc = get_u32(buf + pre_crc_len);
  const uint32_t actual_header_crc = crc32(buf, pre_crc_len);
  if (expect_header_crc != actual_header_crc) {
    return decode_status::bad_header_checksum;
  }

  const uint32_t expect_payload_crc = get_u32(buf + pre_crc_len + 4);
  const uint8_t* payload = buf + header_len;
  if (payload_len == 0) {
    if (expect_payload_crc != 0) {
      return decode_status::bad_payload_checksum;
    }
  } else {
    const uint32_t actual_payload_crc = crc32(payload, payload_len);
    if (expect_payload_crc != actual_payload_crc) {
      return decode_status::bad_payload_checksum;
    }
  }

  if (type != record_type::data && type != record_type::delete_tombstone &&
      type != record_type::gc_rewrite && type != record_type::object_name &&
      type != record_type::segment_seal &&
      type != record_type::range_tombstone &&
      type != record_type::checkpoint_marker &&
      type != record_type::raft_snapshot_pin &&
      type != record_type::raft_snapshot_unpin) {
    return decode_status::bad_type;
  }

  if (out) {
    out->type = type;
    out->flags = flags;
    out->pool_id = get_u64(buf + 14);
    out->pg_id = get_u64(buf + 22);
    out->store_epoch = get_u64(buf + 30);
    out->object_id = get_u64(buf + 38);
    out->object_offset = get_u64(buf + 46);
    out->raft_apply_index = get_u64(buf + 54);
    out->image_snap_seq = get_u64(buf + 62);
    out->payload.assign(reinterpret_cast<const char*>(payload), payload_len);
  }

  if (consumed) {
    *consumed = static_cast<size_t>(header_len) + payload_len;
  }
  return decode_status::ok;
}

} // namespace fastblock::aos
