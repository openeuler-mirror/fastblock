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

/** 
 *  再返回一个size常量，是为了接口和 encode_varint32 统一，
 *  减少以后varint和fix切换时的代码改动。
 *  由于是inline函数，且返回的是编译期常量，所以不会引入额外的开销。
 */ 
inline size_t
encode_fixed32(char* dst, uint32_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
  return sizeof(uint32_t);
}

inline size_t 
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
  return sizeof(uint64_t);
}

inline std::pair<uint32_t, size_t>
decode_fixed32(const char* ptr, size_t) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  uint32_t result = (static_cast<uint32_t>(buffer[0])) |
                    (static_cast<uint32_t>(buffer[1]) << 8) |
                    (static_cast<uint32_t>(buffer[2]) << 16) |
                    (static_cast<uint32_t>(buffer[3]) << 24);
  return {result, sizeof(uint32_t)};
}

//  此处无需担心额外声明一个result变量的开销，会被RVO优化掉
inline std::pair<uint64_t, size_t>
decode_fixed64(const char* ptr, size_t) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  uint64_t result = (static_cast<uint64_t>(buffer[0])) |
                    (static_cast<uint64_t>(buffer[1]) << 8) |
                    (static_cast<uint64_t>(buffer[2]) << 16) |
                    (static_cast<uint64_t>(buffer[3]) << 24) |
                    (static_cast<uint64_t>(buffer[4]) << 32) |
                    (static_cast<uint64_t>(buffer[5]) << 40) |
                    (static_cast<uint64_t>(buffer[6]) << 48) |
                    (static_cast<uint64_t>(buffer[7]) << 56);
  return {result, sizeof(uint64_t)};
}
