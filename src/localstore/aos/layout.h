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

namespace fastblock::aos {

// Fixed metadata layout at the front of the AOS device (design §4.1).
// All sizes are multiples of 4KiB.
//
// +------------------+ 0
// | superblock A     | 4KiB
// +------------------+ 4KiB
// | superblock B     | 4KiB
// +------------------+ 8KiB
// | manifest slot 0  | 128KiB
// +------------------+
// | manifest slot 1  | 128KiB
// +------------------+ kMetaSize
// | data segments... |
// +------------------+

inline constexpr uint64_t kSuperblockBytes = 4096;
inline constexpr uint64_t kSuperblockAOffset = 0;
inline constexpr uint64_t kSuperblockBOffset = kSuperblockBytes;
inline constexpr uint64_t kManifestSlotBytes = 128ull * 1024;
inline constexpr uint64_t kManifestOffset = kSuperblockBOffset + kSuperblockBytes;
inline constexpr uint64_t kManifestSlots = 2;
inline constexpr uint64_t kMetaSize =
    kManifestOffset + kManifestSlotBytes * kManifestSlots;

inline constexpr uint32_t kSuperblockMagic = 0x42534f41u; // 'AOSB' LE
inline constexpr uint32_t kManifestMagic = 0x4d534f41u;   // 'AOSM' LE
inline constexpr uint16_t kLayoutVersion = 1;

inline uint64_t manifest_slot_offset(uint64_t generation) {
  return kManifestOffset +
         (generation % kManifestSlots) * kManifestSlotBytes;
}

} // namespace fastblock::aos
