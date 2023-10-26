/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

// The encoding/decoding algorithm is borrowed from leveldb
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <concepts>

#include <spdk/env.h>
#include <spdk/log.h>

inline size_t 
encode_varint32(char* dst, uint32_t v) {
  size_t bytes = 0;
  uint8_t* buffer = reinterpret_cast<uint8_t*>(dst);
  while (v >= 0x80) {
    buffer[bytes++] = v | 0x80;
    v >>= 7;
  }
  buffer[bytes++] = static_cast<uint8_t>(v);
  return bytes;
}

inline size_t 
encode_varint64(char* dst, uint64_t v) {
  size_t bytes = 0;
  uint8_t* buffer = reinterpret_cast<uint8_t*>(dst);
  while (v >= 0x80) {
    buffer[bytes++] = v | 0x80;
    v >>= 7;
  }
  buffer[bytes++] = static_cast<uint8_t>(v);
  return bytes;
}

inline std::pair<uint32_t, size_t>
decode_varint32(char* src, size_t len) {
  size_t bytes = 0;
  uint32_t result = 0;
  int shift = 0;
  for(size_t i = 0; i < 5 && i < len; i++) {
    uint32_t byte = static_cast<uint8_t>(src[bytes++]);
    if (byte & 0x80) {
      result |= ((byte & 0x7F) << shift);
    } else {
      result |= byte << shift;
      break;
    }
    shift += 7;
  }
  return {result, bytes};
}


inline std::pair<uint64_t, size_t>
decode_varint64(char* src, size_t len) {
  size_t bytes = 0;
  uint64_t result = 0;
  int shift = 0;
  for(size_t i = 0; i < 10 && i < len; i++) {
    uint64_t byte = static_cast<uint8_t>(src[bytes++]);
    if (byte & 0x80) {
      result |= ((byte & 0x7F) << shift);
    } else {
      result |= byte << shift;
      break;
    }
    shift += 7;
  }
  return {result, bytes};
}

/***********************************************************/
/*
inline void 
encode_fixed32(char* buffer, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        buffer[i] = static_cast<char>(value & 0xFF);
        value >>= 8;
    }
}

inline void 
encode_fixed64(char* buffer, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        buffer[i] = static_cast<char>(value & 0xFF);
        value >>= 8;
    }
}

inline uint32_t
decode_fixed32(const char* buffer) {
    uint32_t result = 0;
    for (int i = 3; i >= 0; i--) {
        result <<= 8;
        result |= static_cast<unsigned char>(buffer[i]);
    }
    return result;
}

inline uint64_t
decode_fixed64(const char* buffer) {
    uint64_t result = 0;
    for (int i = 7; i >= 0; i--) {
        result <<= 8;
        result |= static_cast<unsigned char>(buffer[i]);
    }
    return result;
}
*/
/***********************************************************/

inline void
encode_fixed32(char* dst, uint32_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
}

inline void 
encode_fixed64(char* dst, uint64_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
  buffer[4] = static_cast<uint8_t>(value >> 32);
  buffer[5] = static_cast<uint8_t>(value >> 40);
  buffer[6] = static_cast<uint8_t>(value >> 48);
  buffer[7] = static_cast<uint8_t>(value >> 56);
}

inline uint32_t
decode_fixed32(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  uint32_t result = (static_cast<uint32_t>(buffer[0])) |
                    (static_cast<uint32_t>(buffer[1]) << 8) |
                    (static_cast<uint32_t>(buffer[2]) << 16) |
                    (static_cast<uint32_t>(buffer[3]) << 24);
  return result;
}

inline uint64_t
decode_fixed64(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  uint64_t result = (static_cast<uint64_t>(buffer[0])) |
                    (static_cast<uint64_t>(buffer[1]) << 8) |
                    (static_cast<uint64_t>(buffer[2]) << 16) |
                    (static_cast<uint64_t>(buffer[3]) << 24) |
                    (static_cast<uint64_t>(buffer[4]) << 32) |
                    (static_cast<uint64_t>(buffer[5]) << 40) |
                    (static_cast<uint64_t>(buffer[6]) << 48) |
                    (static_cast<uint64_t>(buffer[7]) << 56);
  return result;
}

/**
 * 新增对不连续地址的序列化。
 */
inline void 
encode_fixed64(char* dst1, size_t len1, char* dst2, uint64_t value) {
    char buffer[8];
    encode_fixed64(buffer, value);
    memcpy(dst1, buffer, len1);
    memcpy(dst2, buffer + len1, 8 - len1);
}

inline uint64_t
decode_fixed64(const char* ptr1, size_t len1, const char* ptr2) {
    char buffer[8];
    memcpy(buffer, ptr1, len1);
    memcpy(buffer + len1, ptr2, 8 - len1);
    return decode_fixed64(buffer);
}