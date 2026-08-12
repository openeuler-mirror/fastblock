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
#include <cstddef>

namespace fastblock::aos {

// CRC-32/ISO-HDLC (poly 0xEDB88320), sufficient for M1 torn-write checks.
inline uint32_t crc32(const void* data, size_t len, uint32_t seed = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    ready = true;
  }

  uint32_t c = seed ^ 0xFFFFFFFFu;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

} // namespace fastblock::aos
