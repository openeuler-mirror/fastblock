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
#include <string>
#include <vector>

namespace fastblock::aos {

// Fixed header layout (little-endian), design §6.2.
// Total fixed header bytes before optional payload.
//
//  magic            u32
//  format_version   u16
//  record_type      u8
//  flags            u8
//  header_len       u16
//  payload_len      u32
//  pool_id          u64
//  pg_id            u64
//  store_epoch      u64
//  object_id        u64
//  object_offset    u64
//  raft_apply_index u64
//  image_snap_seq   u64
//  header_checksum  u32  (covers all fields above except itself)
//  payload_checksum u32  (covers payload; 0 if payload_len==0)
//
// header_len == sizeof fixed header for version 1 data records.
// 4+2+1+1+2+4 + 7*8 + 4+4 = 78
inline constexpr uint16_t kRecordHeaderLenV1 = 78;

struct data_record {
  record_type type{record_type::data};
  uint8_t flags{0};
  uint64_t pool_id{0};
  uint64_t pg_id{0};
  uint64_t store_epoch{0};
  uint64_t object_id{0};
  uint64_t object_offset{0};
  uint64_t raft_apply_index{0};
  uint64_t image_snap_seq{0};
  std::string payload;
};

enum class decode_status {
  ok = 0,
  need_more,          // buffer shorter than one full record
  bad_magic,
  bad_version,
  bad_header_len,
  bad_header_checksum,
  bad_payload_checksum,
  bad_type,
};

// Encode a data/tombstone-style record. Returns full bytes (header+payload).
std::vector<uint8_t> encode_record(const data_record& rec);

// Try decode one record from buf[0..len). On ok, *consumed is total bytes.
decode_status decode_record(const uint8_t* buf, size_t len, data_record* out,
                            size_t* consumed);

} // namespace fastblock::aos
