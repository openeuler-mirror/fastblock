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

#include "layout.h"
#include "types.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fastblock::aos {

struct superblock {
  uint64_t generation{0};
  uint64_t segment_size{0};
  uint64_t data_offset{kMetaSize};
  uint64_t data_length{0};
  uint64_t manifest_generation{0};
};

struct manifest_pg_entry {
  uint64_t pool_id{0};
  uint64_t pg_id{0};
  uint64_t active_epoch{0}; // max store_epoch ever allocated (survives delete)
};

// One durable checkpoint record location (object or log).
struct manifest_ckpt {
  uint64_t pool_id{0};
  uint64_t pg_id{0};
  uint64_t store_epoch{0};
  uint64_t phys{0}; // absolute start of the checkpoint record
};

// flags bit 0: v2 tail present (segment table + checkpoint pointers).
inline constexpr uint16_t kManifestFlagV2 = 0x1;

struct manifest {
  uint64_t generation{0};
  uint64_t data_high_water{0}; // absolute end of last data write
  uint32_t open_segment_id{0xFFFFFFFFu};
  uint64_t open_cursor{0};
  std::vector<manifest_pg_entry> pgs;
  // v2: recover skips [data_offset, recover_scan_from) after loading ckpts.
  uint64_t recover_scan_from{0};
  uint64_t kv_checkpoint_phys{0};
  std::vector<manifest_ckpt> object_checkpoints;
  std::vector<manifest_ckpt> log_checkpoints;
  std::vector<uint8_t> segment_states; // one byte per segment_state
};

// Encode into a full superblock-sized buffer (zero-padded). CRC at end of header.
std::vector<uint8_t> encode_superblock(const superblock& sb);
bool decode_superblock(const uint8_t* buf, size_t len, superblock* out);

// Encode/decode manifest payload (fits in one slot). CRC trailing.
std::vector<uint8_t> encode_manifest(const manifest& m);
bool decode_manifest(const uint8_t* buf, size_t len, manifest* out);

// Pick better of two superblocks (higher generation, valid CRC).
bool prefer_superblock(const superblock& a, bool a_ok, const superblock& b,
                       bool b_ok, superblock* best);

} // namespace fastblock::aos
