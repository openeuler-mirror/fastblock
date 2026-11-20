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

/**
 * Unique per-offset payload used by image_verify.
 *
 * This is the opposite of block_bench's constant 0x55 fill: every 4KiB
 * (or io_size) block encodes seed + image offset + a header CRC and a
 * deterministic body. A swap, hole, stale write, or replica split shows
 * up as a decode / memcmp failure instead of a false green.
 *
 * Layout (little-endian):
 *   [ 0,  8)  magic  "VERIFYA1"
 *   [ 8, 16)  seed
 *   [16, 24)  image_offset
 *   [24, 28)  length
 *   [28, 32)  header CRC-32 (bytes [0, 28))
 *   [32,  N)  fill(seed, offset, i)
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace image_verify {

inline constexpr char kMagic[8] = {'V', 'E', 'R', 'I', 'F', 'Y', 'A', '1'};
inline constexpr size_t kHeaderSize = 32;

enum class pattern_error {
  ok = 0,
  too_short,
  bad_magic,
  bad_header_crc,
  seed_mismatch,
  offset_mismatch,
  length_mismatch,
  body_mismatch,
};

inline uint32_t pattern_crc32(const void* data, size_t len) {
  static const auto table = []() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();

  uint32_t c = 0xFFFFFFFFu;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

inline uint8_t fill_byte(uint64_t seed, uint64_t offset, size_t i) {
  const uint8_t s = static_cast<uint8_t>(seed >> ((i % 8) * 8));
  const uint8_t o = static_cast<uint8_t>(offset >> (((i + 3) % 8) * 8));
  return static_cast<uint8_t>(s ^ o ^ static_cast<uint8_t>(i) ^
                             static_cast<uint8_t>(i * 131u));
}

inline void write_le64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
}

inline void write_le32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
}

inline uint64_t read_le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

inline uint32_t read_le32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(p[i]) << (8 * i);
  }
  return v;
}

inline std::string make_payload(uint64_t seed, uint64_t offset, size_t length) {
  std::string buf(length, '\0');
  auto* p = reinterpret_cast<uint8_t*>(buf.data());
  if (length >= kHeaderSize) {
    std::memcpy(p, kMagic, 8);
    write_le64(p + 8, seed);
    write_le64(p + 16, offset);
    write_le32(p + 24, static_cast<uint32_t>(length));
    write_le32(p + 28, pattern_crc32(p, 28));
    for (size_t i = kHeaderSize; i < length; ++i) {
      p[i] = fill_byte(seed, offset, i);
    }
  } else {
    for (size_t i = 0; i < length; ++i) {
      p[i] = fill_byte(seed, offset, i);
    }
  }
  return buf;
}

struct decoded_payload {
  uint64_t seed{0};
  uint64_t offset{0};
  uint32_t length{0};
};

inline pattern_error decode_payload(const void* data, size_t size,
                                    decoded_payload* out) {
  if (size < kHeaderSize) {
    return pattern_error::too_short;
  }
  const auto* p = static_cast<const uint8_t*>(data);
  if (std::memcmp(p, kMagic, 8) != 0) {
    return pattern_error::bad_magic;
  }
  if (pattern_crc32(p, 28) != read_le32(p + 28)) {
    return pattern_error::bad_header_crc;
  }
  decoded_payload d;
  d.seed = read_le64(p + 8);
  d.offset = read_le64(p + 16);
  d.length = read_le32(p + 24);
  if (out != nullptr) {
    *out = d;
  }
  if (d.length != size) {
    return pattern_error::length_mismatch;
  }
  for (size_t i = kHeaderSize; i < size; ++i) {
    if (p[i] != fill_byte(d.seed, d.offset, i)) {
      return pattern_error::body_mismatch;
    }
  }
  return pattern_error::ok;
}

inline pattern_error check_payload(const void* data, size_t size, uint64_t seed,
                                   uint64_t offset) {
  decoded_payload d;
  const auto st = decode_payload(data, size, &d);
  if (st != pattern_error::ok) {
    return st;
  }
  if (d.seed != seed) {
    return pattern_error::seed_mismatch;
  }
  if (d.offset != offset) {
    return pattern_error::offset_mismatch;
  }
  return pattern_error::ok;
}

inline const char* pattern_error_name(pattern_error e) {
  switch (e) {
  case pattern_error::ok:
    return "ok";
  case pattern_error::too_short:
    return "too_short";
  case pattern_error::bad_magic:
    return "bad_magic";
  case pattern_error::bad_header_crc:
    return "bad_header_crc";
  case pattern_error::seed_mismatch:
    return "seed_mismatch";
  case pattern_error::offset_mismatch:
    return "offset_mismatch";
  case pattern_error::length_mismatch:
    return "length_mismatch";
  case pattern_error::body_mismatch:
    return "body_mismatch";
  }
  return "unknown";
}

} // namespace image_verify
