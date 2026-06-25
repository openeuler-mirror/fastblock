/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/**
 * @file test_utils.cc
 * @brief Unit tests for utility functions (itos, units, md5)
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include "utils/itos.h"
#include "utils/units.h"
#include "utils/md5.h"
#include "utils/varint.h"

#include <string>
#include <cstring>

// ============================================================================
// Test Suite: itos (Integer to String Conversion)
// ============================================================================

FB_SUITE_SETUP(itos) {
    // Setup code here
}

FB_SUITE_TEARDOWN(itos) {
    // Teardown code here
}

FB_TEST(itos, zero) {
    std::string result = itos(0);
    FB_ASSERT_EQ(result, "0");
}

FB_TEST(itos, positive_small) {
    std::string result = itos(42);
    FB_ASSERT_EQ(result, "42");
}

FB_TEST(itos, positive_large) {
    std::string result = itos(1234567890);
    FB_ASSERT_EQ(result, "1234567890");
}

FB_TEST(itos, negative_small) {
    std::string result = itos(-42);
    FB_ASSERT_EQ(result, "-42");
}

FB_TEST(itos, negative_large) {
    std::string result = itos(-1234567890);
    FB_ASSERT_EQ(result, "-1234567890");
}

FB_TEST(itos, max_int64) {
    int64_t max_val = 9223372036854775807LL;
    std::string result = itos(max_val);
    FB_ASSERT_EQ(result, "9223372036854775807");
}

FB_TEST(itos, min_int64) {
    int64_t min_val = -9223372036854775807LL;
    std::string result = itos(min_val);
    FB_ASSERT_EQ(result, "-9223372036854775807");
}

// ============================================================================
// Test Suite: units (Size Constants)
// ============================================================================

FB_SUITE_SETUP(units) {
    // Setup code here
}

FB_SUITE_TEARDOWN(units) {
    // Teardown code here
}

FB_TEST(units, base_constants) {
    FB_ASSERT_EQ(B, 1);
    FB_ASSERT_EQ(KB, 1024);
    FB_ASSERT_EQ(MB, 1024 * 1024);
    FB_ASSERT_EQ(GB, 1024 * 1024 * 1024);
}

FB_TEST(units, literal_kb) {
    size_t val = 4_KB;
    FB_ASSERT_EQ(val, 4096);
}

FB_TEST(units, literal_mb) {
    size_t val = 2_MB;
    FB_ASSERT_EQ(val, 2 * 1024 * 1024);
}

FB_TEST(units, literal_gb) {
    size_t val = 1_GB;
    FB_ASSERT_EQ(val, 1024 * 1024 * 1024);
}

FB_TEST(units, arithmetic) {
    size_t val = 1_MB + 512_KB;
    FB_ASSERT_EQ(val, 1024 * 1024 + 512 * 1024);
}

// ============================================================================
// Test Suite: md5 (MD5 Hash)
// ============================================================================

FB_SUITE_SETUP(md5) {
    // Setup code here
}

FB_SUITE_TEARDOWN(md5) {
    // Teardown code here
}

FB_TEST(md5, empty_string) {
    char data[] = "";
    std::string hash = utils::md5(data, 0);
    // MD5 of empty string: d41d8cd98f00b204e9800998ecf8427e
    FB_ASSERT_EQ(hash.length(), 16);
}

FB_TEST(md5, single_char) {
    char data[] = "a";
    std::string hash = utils::md5(data, 1);
    // MD5("a") = 0cc175b9c0f1b6a831c399e269772661
    FB_ASSERT_EQ(hash.length(), 16);
}

FB_TEST(md5, short_string) {
    char data[] = "hello";
    std::string hash = utils::md5(data, 5);
    FB_ASSERT_EQ(hash.length(), 16);
}

FB_TEST(md5, consistent_hash) {
    char data[] = "test data";
    std::string hash1 = utils::md5(data, strlen(data));
    std::string hash2 = utils::md5(data, strlen(data));
    FB_ASSERT_EQ(hash1, hash2);
}

FB_TEST(md5, different_inputs) {
    char data1[] = "input1";
    char data2[] = "input2";
    std::string hash1 = utils::md5(data1, strlen(data1));
    std::string hash2 = utils::md5(data2, strlen(data2));
    FB_ASSERT_TRUE(hash1 != hash2);
}

// ============================================================================
// Test Suite: varint32 (Variable Integer Encoding 32-bit)
// ============================================================================

FB_SUITE_SETUP(varint32) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint32) {
    // Teardown code here
}

FB_TEST(varint32, encode_zero) {
    char buffer[5];
    size_t len = encode_varint32(buffer, 0);
    FB_ASSERT_EQ(len, 1);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[0]), 0);
}

FB_TEST(varint32, encode_one_byte) {
    char buffer[5];
    size_t len = encode_varint32(buffer, 127);
    FB_ASSERT_EQ(len, 1);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[0]), 127);
}

FB_TEST(varint32, encode_two_bytes) {
    char buffer[5];
    size_t len = encode_varint32(buffer, 128);
    FB_ASSERT_EQ(len, 2);
}

FB_TEST(varint32, encode_max_uint32) {
    char buffer[5];
    uint32_t max_val = 4294967295U;
    size_t len = encode_varint32(buffer, max_val);
    FB_ASSERT_TRUE(len <= 5);
}

FB_TEST(varint32, roundtrip_zero) {
    char buffer[5];
    size_t len = encode_varint32(buffer, 0);
    auto [value, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(value, 0);
    FB_ASSERT_EQ(decoded_len, 1);
}

FB_TEST(varint32, roundtrip_small) {
    char buffer[5];
    uint32_t original = 42;
    size_t len = encode_varint32(buffer, original);
    auto [value, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(value, original);
    FB_ASSERT_EQ(decoded_len, len);
}

FB_TEST(varint32, roundtrip_large) {
    char buffer[5];
    uint32_t original = 12345678;
    size_t len = encode_varint32(buffer, original);
    auto [value, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(value, original);
    FB_ASSERT_EQ(decoded_len, len);
}

FB_TEST(varint32, roundtrip_max) {
    char buffer[5];
    uint32_t original = 4294967295U;
    size_t len = encode_varint32(buffer, original);
    auto [value, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(value, original);
}

// ============================================================================
// Test Suite: varint64 (Variable Integer Encoding 64-bit)
// ============================================================================

FB_SUITE_SETUP(varint64) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint64) {
    // Teardown code here
}

FB_TEST(varint64, encode_zero) {
    char buffer[10];
    size_t len = encode_varint64(buffer, 0);
    FB_ASSERT_EQ(len, 1);
}

FB_TEST(varint64, encode_one_byte) {
    char buffer[10];
    size_t len = encode_varint64(buffer, 127);
    FB_ASSERT_EQ(len, 1);
}

FB_TEST(varint64, encode_max_uint64) {
    char buffer[10];
    uint64_t max_val = 18446744073709551615ULL;
    size_t len = encode_varint64(buffer, max_val);
    FB_ASSERT_TRUE(len <= 10);
}

FB_TEST(varint64, roundtrip_zero) {
    char buffer[10];
    size_t len = encode_varint64(buffer, 0);
    auto [value, decoded_len] = decode_varint64(buffer, len);
    FB_ASSERT_EQ(value, 0);
}

FB_TEST(varint64, roundtrip_small) {
    char buffer[10];
    uint64_t original = 42;
    size_t len = encode_varint64(buffer, original);
    auto [value, decoded_len] = decode_varint64(buffer, len);
    FB_ASSERT_EQ(value, original);
}

FB_TEST(varint64, roundtrip_large) {
    char buffer[10];
    uint64_t original = 12345678901234ULL;
    size_t len = encode_varint64(buffer, original);
    auto [value, decoded_len] = decode_varint64(buffer, len);
    FB_ASSERT_EQ(value, original);
}

FB_TEST(varint64, roundtrip_max) {
    char buffer[10];
    uint64_t original = 18446744073709551615ULL;
    size_t len = encode_varint64(buffer, original);
    auto [value, decoded_len] = decode_varint64(buffer, len);
    FB_ASSERT_EQ(value, original);
}

// ============================================================================
// Test Suite: fixed32 (Fixed-size Integer Encoding 32-bit)
// ============================================================================

FB_SUITE_SETUP(fixed32) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fixed32) {
    // Teardown code here
}

FB_TEST(fixed32, encode_zero) {
    char buffer[4];
    encode_fixed32(buffer, 0);
    uint32_t value = decode_fixed32(buffer);
    FB_ASSERT_EQ(value, 0);
}

FB_TEST(fixed32, encode_one) {
    char buffer[4];
    encode_fixed32(buffer, 1);
    uint32_t value = decode_fixed32(buffer);
    FB_ASSERT_EQ(value, 1);
}

FB_TEST(fixed32, encode_max) {
    char buffer[4];
    uint32_t original = 4294967295U;
    encode_fixed32(buffer, original);
    uint32_t value = decode_fixed32(buffer);
    FB_ASSERT_EQ(value, original);
}

FB_TEST(fixed32, encode_arbitrary) {
    char buffer[4];
    uint32_t original = 0x12345678;
    encode_fixed32(buffer, original);
    uint32_t value = decode_fixed32(buffer);
    FB_ASSERT_EQ(value, original);
}

// ============================================================================
// Test Suite: fixed64 (Fixed-size Integer Encoding 64-bit)
// ============================================================================

FB_SUITE_SETUP(fixed64) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fixed64) {
    // Teardown code here
}

FB_TEST(fixed64, encode_zero) {
    char buffer[8];
    encode_fixed64(buffer, 0);
    uint64_t value = decode_fixed64(buffer);
    FB_ASSERT_EQ(value, 0);
}

FB_TEST(fixed64, encode_one) {
    char buffer[8];
    encode_fixed64(buffer, 1);
    uint64_t value = decode_fixed64(buffer);
    FB_ASSERT_EQ(value, 1);
}

FB_TEST(fixed64, encode_max) {
    char buffer[8];
    uint64_t original = 18446744073709551615ULL;
    encode_fixed64(buffer, original);
    uint64_t value = decode_fixed64(buffer);
    FB_ASSERT_EQ(value, original);
}

FB_TEST(fixed64, encode_arbitrary) {
    char buffer[8];
    uint64_t original = 0x123456789ABCDEF0ULL;
    encode_fixed64(buffer, original);
    uint64_t value = decode_fixed64(buffer);
    FB_ASSERT_EQ(value, original);
}

FB_TEST(fixed64, encode_split) {
    char buffer1[4];
    char buffer2[4];
    uint64_t original = 0x123456789ABCDEF0ULL;
    encode_fixed64(buffer1, 3, buffer2, original);
    uint64_t value = decode_fixed64(buffer1, 3, buffer2);
    FB_ASSERT_EQ(value, original);
}

// ============================================================================
// Test Suite: itos_edge_cases (Edge Cases for Integer to String)
// ============================================================================

FB_SUITE_SETUP(itos_edge_cases) {
    // Setup code here
}

FB_SUITE_TEARDOWN(itos_edge_cases) {
    // Teardown code here
}

FB_TEST(itos_edge_cases, one) {
    std::string result = itos(1);
    FB_ASSERT_EQ(result, "1");
}

FB_TEST(itos_edge_cases, negative_one) {
    std::string result = itos(-1);
    FB_ASSERT_EQ(result, "-1");
}

FB_TEST(itos_edge_cases, power_of_two) {
    std::string result = itos(1024);
    FB_ASSERT_EQ(result, "1024");
}

FB_TEST(itos_edge_cases, power_of_ten) {
    std::string result = itos(1000000);
    FB_ASSERT_EQ(result, "1000000");
}

FB_TEST(itos_edge_cases, int32_max) {
    int32_t max_val = 2147483647;
    std::string result = itos(max_val);
    FB_ASSERT_EQ(result, "2147483647");
}

FB_TEST(itos_edge_cases, int32_min) {
    int32_t min_val = -2147483648;
    std::string result = itos(min_val);
    FB_ASSERT_EQ(result, "-2147483648");
}

// ============================================================================
// Test Suite: units_combinations (Units Combination Tests)
// ============================================================================

FB_SUITE_SETUP(units_combinations) {
    // Setup code here
}

FB_SUITE_TEARDOWN(units_combinations) {
    // Teardown code here
}

FB_TEST(units_combinations, kb_plus_kb) {
    size_t val = 1_KB + 1_KB;
    FB_ASSERT_EQ(val, 2048);
}

FB_TEST(units_combinations, mb_plus_kb) {
    size_t val = 1_MB + 1_KB;
    FB_ASSERT_EQ(val, 1024 * 1024 + 1024);
}

FB_TEST(units_combinations, gb_plus_mb) {
    size_t val = 1_GB + 1_MB;
    FB_ASSERT_EQ(val, 1024 * 1024 * 1024 + 1024 * 1024);
}

FB_TEST(units_combinations, gb_minus_mb) {
    size_t val = 1_GB - 1_MB;
    FB_ASSERT_EQ(val, 1024 * 1024 * 1024 - 1024 * 1024);
}

FB_TEST(units_combinations, mb_times_int) {
    size_t val = 4_MB;
    FB_ASSERT_EQ(val, 4 * 1024 * 1024);
}

FB_TEST(units_combinations, complex_expression) {
    size_t val = 2_GB - 512_MB + 128_KB;
    FB_ASSERT_EQ(val, 2ULL * 1024 * 1024 * 1024 - 512ULL * 1024 * 1024 + 128 * 1024);
}

// ============================================================================
// Test Suite: varint32_boundary (Varint32 Boundary Tests)
// ============================================================================

FB_SUITE_SETUP(varint32_boundary) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint32_boundary) {
    // Teardown code here
}

FB_TEST(varint32_boundary, encode_127_boundary) {
    char buffer[5];
    size_t len = encode_varint32(buffer, 127);
    FB_ASSERT_EQ(len, 1);
}

FB_TEST(varint32_boundary, encode_128_boundary) {
    char buffer[5];
    size_t len = encode_varint32(buffer, 128);
    FB_ASSERT_EQ(len, 2);
}

FB_TEST(varint32_boundary, encode_16383_boundary) {
    char buffer[5];
    size_t len = encode_varint32(buffer, 16383);
    FB_ASSERT_EQ(len, 2);
}

FB_TEST(varint32_boundary, encode_16384_boundary) {
    char buffer[5];
    size_t len = encode_varint32(buffer, 16384);
    FB_ASSERT_EQ(len, 3);
}

FB_TEST(varint32_boundary, roundtrip_127) {
    char buffer[5];
    uint32_t original = 127;
    size_t len = encode_varint32(buffer, original);
    auto [value, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(value, original);
    FB_ASSERT_EQ(decoded_len, 1);
}

FB_TEST(varint32_boundary, roundtrip_128) {
    char buffer[5];
    uint32_t original = 128;
    size_t len = encode_varint32(buffer, original);
    auto [value, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(value, original);
    FB_ASSERT_EQ(decoded_len, 2);
}

FB_TEST(varint32_boundary, roundtrip_16383) {
    char buffer[5];
    uint32_t original = 16383;
    size_t len = encode_varint32(buffer, original);
    auto [value, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(value, original);
    FB_ASSERT_EQ(decoded_len, 2);
}

FB_TEST(varint32_boundary, roundtrip_16384) {
    char buffer[5];
    uint32_t original = 16384;
    size_t len = encode_varint32(buffer, original);
    auto [value, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(value, original);
    FB_ASSERT_EQ(decoded_len, 3);
}

// ============================================================================
// Test Suite: varint64_boundary (Varint64 Boundary Tests)
// ============================================================================

FB_SUITE_SETUP(varint64_boundary) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint64_boundary) {
    // Teardown code here
}

FB_TEST(varint64_boundary, encode_127_boundary) {
    char buffer[10];
    size_t len = encode_varint64(buffer, 127);
    FB_ASSERT_EQ(len, 1);
}

FB_TEST(varint64_boundary, encode_128_boundary) {
    char buffer[10];
    size_t len = encode_varint64(buffer, 128);
    FB_ASSERT_EQ(len, 2);
}

FB_TEST(varint64_boundary, encode_16383_boundary) {
    char buffer[10];
    size_t len = encode_varint64(buffer, 16383);
    FB_ASSERT_EQ(len, 2);
}

FB_TEST(varint64_boundary, encode_16384_boundary) {
    char buffer[10];
    size_t len = encode_varint64(buffer, 16384);
    FB_ASSERT_EQ(len, 3);
}

FB_TEST(varint64_boundary, roundtrip_127) {
    char buffer[10];
    uint64_t original = 127;
    size_t len = encode_varint64(buffer, original);
    auto [value, decoded_len] = decode_varint64(buffer, len);
    FB_ASSERT_EQ(value, original);
    FB_ASSERT_EQ(decoded_len, 1);
}

FB_TEST(varint64_boundary, roundtrip_128) {
    char buffer[10];
    uint64_t original = 128;
    size_t len = encode_varint64(buffer, original);
    auto [value, decoded_len] = decode_varint64(buffer, len);
    FB_ASSERT_EQ(value, original);
    FB_ASSERT_EQ(decoded_len, 2);
}

FB_TEST(varint64_boundary, roundtrip_large_value) {
    char buffer[10];
    uint64_t original = 1099511627775ULL; // 2^40 - 1
    size_t len = encode_varint64(buffer, original);
    auto [value, decoded_len] = decode_varint64(buffer, len);
    FB_ASSERT_EQ(value, original);
}

// ============================================================================
// Test Suite: md5_properties (MD5 Hash Properties Tests)
// ============================================================================

FB_SUITE_SETUP(md5_properties) {
    // Setup code here
}

FB_SUITE_TEARDOWN(md5_properties) {
    // Teardown code here
}

FB_TEST(md5_properties, fixed_length) {
    char data1[] = "a";
    char data2[] = "abcdefghijklmnopqrstuvwxyz";
    char data3[] = "";

    std::string hash1 = utils::md5(data1, strlen(data1));
    std::string hash2 = utils::md5(data2, strlen(data2));
    std::string hash3 = utils::md5(data3, 0);

    // All MD5 hashes should be 16 bytes (128 bits)
    FB_ASSERT_EQ(hash1.length(), 16);
    FB_ASSERT_EQ(hash2.length(), 16);
    FB_ASSERT_EQ(hash3.length(), 16);
}

FB_TEST(md5_properties, deterministic) {
    char data[] = "deterministic test";
    std::string hash1 = utils::md5(data, strlen(data));
    std::string hash2 = utils::md5(data, strlen(data));
    std::string hash3 = utils::md5(data, strlen(data));

    FB_ASSERT_EQ(hash1, hash2);
    FB_ASSERT_EQ(hash2, hash3);
}

FB_TEST(md5_properties, similar_inputs_different_output) {
    char data1[] = "test1";
    char data2[] = "test2";

    std::string hash1 = utils::md5(data1, strlen(data1));
    std::string hash2 = utils::md5(data2, strlen(data2));

    // Similar inputs should produce different hashes
    FB_ASSERT_TRUE(hash1 != hash2);
}

FB_TEST(md5_properties, long_string) {
    std::string long_data(10000, 'a');
    std::string hash = utils::md5(const_cast<char*>(long_data.c_str()), long_data.size());

    // MD5 should work with long strings and still produce 16-byte hash
    FB_ASSERT_EQ(hash.length(), 16);
}

FB_TEST(md5_properties, binary_data) {
    unsigned char binary_data[] = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0xFD};
    std::string hash = utils::md5(reinterpret_cast<char*>(binary_data), sizeof(binary_data));

    FB_ASSERT_EQ(hash.length(), 16);
}

// ============================================================================
// Test Suite: fixed_encoding_properties (Fixed Encoding Properties Tests)
// ============================================================================

FB_SUITE_SETUP(fixed_encoding_properties) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fixed_encoding_properties) {
    // Teardown code here
}

FB_TEST(fixed_encoding_properties, fixed32_endian_swap) {
    char buffer[4];
    uint32_t original = 0x12345678;
    encode_fixed32(buffer, original);
    uint32_t decoded = decode_fixed32(buffer);
    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(fixed_encoding_properties, fixed64_endian_swap) {
    char buffer[8];
    uint64_t original = 0x0123456789ABCDEFULL;
    encode_fixed64(buffer, original);
    uint64_t decoded = decode_fixed64(buffer);
    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(fixed_encoding_properties, fixed32_all_bytes) {
    char buffer[4];
    uint32_t original = 0x01020304;
    encode_fixed32(buffer, original);

    // Check that each byte is encoded correctly (little-endian)
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[0]), 0x04);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[1]), 0x03);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[2]), 0x02);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[3]), 0x01);
}

FB_TEST(fixed_encoding_properties, fixed64_all_bytes) {
    char buffer[8];
    uint64_t original = 0x0102030405060708ULL;
    encode_fixed64(buffer, original);

    // Check that each byte is encoded correctly (little-endian)
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[0]), 0x08);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[1]), 0x07);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[2]), 0x06);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[3]), 0x05);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[4]), 0x04);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[5]), 0x03);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[6]), 0x02);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[7]), 0x01);
}

FB_TEST(fixed_encoding_properties, fixed32_consistency) {
    char buffer1[4];
    char buffer2[4];
    uint32_t value = 0xDEADBEEF;

    encode_fixed32(buffer1, value);
    encode_fixed32(buffer2, value);

    FB_ASSERT_EQ(memcmp(buffer1, buffer2, 4), 0);
}

FB_TEST(fixed_encoding_properties, fixed64_consistency) {
    char buffer1[8];
    char buffer2[8];
    uint64_t value = 0xDEADBEEFCAFEBABEULL;

    encode_fixed64(buffer1, value);
    encode_fixed64(buffer2, value);

    FB_ASSERT_EQ(memcmp(buffer1, buffer2, 8), 0);
}

// ============================================================================
// Test Suite: varint_length (Varint Encoding Length Tests)
// ============================================================================

FB_SUITE_SETUP(varint_length) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint_length) {
    // Teardown code here
}

FB_TEST(varint_length, varint32_single_byte) {
    char buffer[5];
    // Values 0-127 use 1 byte
    FB_ASSERT_EQ(encode_varint32(buffer, 0), 1);
    FB_ASSERT_EQ(encode_varint32(buffer, 1), 1);
    FB_ASSERT_EQ(encode_varint32(buffer, 127), 1);
}

FB_TEST(varint_length, varint32_two_bytes) {
    char buffer[5];
    // Values 128-16383 use 2 bytes
    FB_ASSERT_EQ(encode_varint32(buffer, 128), 2);
    FB_ASSERT_EQ(encode_varint32(buffer, 255), 2);
    FB_ASSERT_EQ(encode_varint32(buffer, 16383), 2);
}

FB_TEST(varint_length, varint32_three_bytes) {
    char buffer[5];
    // Values 16384-2097151 use 3 bytes
    FB_ASSERT_EQ(encode_varint32(buffer, 16384), 3);
    FB_ASSERT_EQ(encode_varint32(buffer, 65535), 3);
}

FB_TEST(varint_length, varint64_single_byte) {
    char buffer[10];
    // Values 0-127 use 1 byte
    FB_ASSERT_EQ(encode_varint64(buffer, 0), 1);
    FB_ASSERT_EQ(encode_varint64(buffer, 127), 1);
}

FB_TEST(varint_length, varint64_two_bytes) {
    char buffer[10];
    // Values 128-16383 use 2 bytes
    FB_ASSERT_EQ(encode_varint64(buffer, 128), 2);
    FB_ASSERT_EQ(encode_varint64(buffer, 16383), 2);
}

FB_TEST(varint_length, varint64_max_bytes) {
    char buffer[10];
    uint64_t max_val = 18446744073709551615ULL;
    size_t len = encode_varint64(buffer, max_val);
    // Maximum 10 bytes for 64-bit varint
    FB_ASSERT_TRUE(len <= 10);
}

// ============================================================================
// Test Suite: itos_types (Integer Types Tests)
// ============================================================================

FB_SUITE_SETUP(itos_types) {
    // Setup code here
}

FB_SUITE_TEARDOWN(itos_types) {
    // Teardown code here
}

FB_TEST(itos_types, int8) {
    int8_t val = 127;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "127");
}

FB_TEST(itos_types, int8_negative) {
    int8_t val = -128;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "-128");
}

FB_TEST(itos_types, uint8) {
    uint8_t val = 255;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "255");
}

FB_TEST(itos_types, int16) {
    int16_t val = 32767;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "32767");
}

FB_TEST(itos_types, int16_negative) {
    int16_t val = -32768;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "-32768");
}

FB_TEST(itos_types, uint16) {
    uint16_t val = 65535;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "65535");
}

FB_TEST(itos_types, int32) {
    int32_t val = 2147483647;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "2147483647");
}

FB_TEST(itos_types, uint32) {
    uint32_t val = 4294967295U;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "4294967295");
}

// ============================================================================
// Test Suite: varint_roundtrip (Varint Roundtrip Stress Tests)
// ============================================================================

FB_SUITE_SETUP(varint_roundtrip) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint_roundtrip) {
    // Teardown code here
}

FB_TEST(varint_roundtrip, varint32_ascending) {
    char buffer[5];
    for (uint32_t i = 0; i < 1000; i++) {
        size_t len = encode_varint32(buffer, i);
        auto [value, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(value, i);
    }
}

FB_TEST(varint_roundtrip, varint32_powers_of_two) {
    char buffer[5];
    for (int i = 0; i < 32; i++) {
        uint32_t val = 1U << i;
        size_t len = encode_varint32(buffer, val);
        auto [value, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(value, val);
    }
}

FB_TEST(varint_roundtrip, varint64_ascending) {
    char buffer[10];
    for (uint64_t i = 0; i < 1000; i++) {
        size_t len = encode_varint64(buffer, i);
        auto [value, decoded_len] = decode_varint64(buffer, len);
        FB_ASSERT_EQ(value, i);
    }
}

FB_TEST(varint_roundtrip, varint64_powers_of_two) {
    char buffer[10];
    for (int i = 0; i < 64; i++) {
        uint64_t val = 1ULL << i;
        size_t len = encode_varint64(buffer, val);
        auto [value, decoded_len] = decode_varint64(buffer, len);
        FB_ASSERT_EQ(value, val);
    }
}

FB_TEST(varint_roundtrip, varint32_random_values) {
    char buffer[5];
    uint32_t values[] = {100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

    for (uint32_t val : values) {
        size_t len = encode_varint32(buffer, val);
        auto [value, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(value, val);
    }
}

// ============================================================================
// Test Suite: fixed_roundtrip (Fixed Encoding Roundtrip Tests)
// ============================================================================

FB_SUITE_SETUP(fixed_roundtrip) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fixed_roundtrip) {
    // Teardown code here
}

FB_TEST(fixed_roundtrip, fixed32_multiple_values) {
    char buffer[4];
    uint32_t values[] = {0, 1, 127, 128, 255, 256, 65535, 65536, 16777215, 16777216, 4294967295};

    for (uint32_t val : values) {
        encode_fixed32(buffer, val);
        uint32_t decoded = decode_fixed32(buffer);
        FB_ASSERT_EQ(decoded, val);
    }
}

FB_TEST(fixed_roundtrip, fixed64_multiple_values) {
    char buffer[8];
    uint64_t values[] = {0, 1, 127, 128, 255, 256, 65535, 65536,
                         16777215, 16777216, 4294967295, 4294967296,
                         18446744073709551615ULL};

    for (uint64_t val : values) {
        encode_fixed64(buffer, val);
        uint64_t decoded = decode_fixed64(buffer);
        FB_ASSERT_EQ(decoded, val);
    }
}

FB_TEST(fixed_roundtrip, fixed32_powers_of_two) {
    char buffer[4];
    for (int i = 0; i < 32; i++) {
        uint32_t val = 1U << i;
        encode_fixed32(buffer, val);
        uint32_t decoded = decode_fixed32(buffer);
        FB_ASSERT_EQ(decoded, val);
    }
}

FB_TEST(fixed_roundtrip, fixed64_powers_of_two) {
    char buffer[8];
    for (int i = 0; i < 64; i++) {
        uint64_t val = 1ULL << i;
        encode_fixed64(buffer, val);
        uint64_t decoded = decode_fixed64(buffer);
        FB_ASSERT_EQ(decoded, val);
    }
}

// ============================================================================
// Test Suite: encoding_comparison (Encoding Method Comparison Tests)
// ============================================================================

FB_SUITE_SETUP(encoding_comparison) {
    // Setup code here
}

FB_SUITE_TEARDOWN(encoding_comparison) {
    // Teardown code here
}

FB_TEST(encoding_comparison, varint_vs_fixed_small) {
    char varint_buf[10];
    char fixed_buf[8];

    uint32_t small_val = 100;
    size_t varint_len = encode_varint32(varint_buf, small_val);

    // For small values, varint should be smaller than fixed
    FB_ASSERT_TRUE(varint_len <= 4);
}

FB_TEST(encoding_comparison, varint_vs_fixed_medium) {
    char varint_buf[10];
    char fixed_buf[8];

    uint32_t medium_val = 100000;
    size_t varint_len = encode_varint32(varint_buf, medium_val);

    // For medium values, varint might be 1-3 bytes
    FB_ASSERT_TRUE(varint_len >= 1 && varint_len <= 5);
}

FB_TEST(encoding_comparison, varint64_vs_fixed64_small) {
    char varint_buf[10];
    char fixed_buf[8];

    uint64_t small_val = 100;
    size_t varint_len = encode_varint64(varint_buf, small_val);

    // For small values, varint should be smaller than fixed
    FB_ASSERT_TRUE(varint_len <= 8);
}

FB_TEST(encoding_comparison, fixed_size_constant) {
    char buf4[4];
    char buf8[8];

    // Fixed encoding always uses constant size
    encode_fixed32(buf4, 0);
    encode_fixed32(buf4, 4294967295U);

    encode_fixed64(buf8, 0);
    encode_fixed64(buf8, 18446744073709551615ULL);

    // Fixed encoding always produces 4 or 8 bytes
    FB_ASSERT_TRUE(true);  // If we got here, encoding succeeded
}

// ============================================================================
// Test Suite: string_conversion (String Conversion Edge Cases)
// ============================================================================

FB_SUITE_SETUP(string_conversion) {
    // Setup code here
}

FB_SUITE_TEARDOWN(string_conversion) {
    // Teardown code here
}

FB_TEST(string_conversion, itos_single_digit) {
    for (int i = 0; i <= 9; i++) {
        std::string result = itos(i);
        FB_ASSERT_EQ(result.length(), 1);
    }
}

FB_TEST(string_conversion, itos_double_digit) {
    for (int i = 10; i <= 99; i++) {
        std::string result = itos(i);
        FB_ASSERT_EQ(result.length(), 2);
    }
}

FB_TEST(string_conversion, itos_triple_digit) {
    for (int i = 100; i <= 999; i++) {
        std::string result = itos(i);
        FB_ASSERT_EQ(result.length(), 3);
    }
}

FB_TEST(string_conversion, itos_consistency) {
    for (int i = 0; i < 100; i++) {
        std::string result1 = itos(i);
        std::string result2 = itos(i);
        FB_ASSERT_EQ(result1, result2);
    }
}

// ============================================================================
// Test Suite: varint_encoding_efficiency (Varint Encoding Efficiency Tests)
// ============================================================================

FB_SUITE_SETUP(varint_encoding_efficiency) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint_encoding_efficiency) {
    // Teardown code here
}

FB_TEST(varint_encoding_efficiency, small_values_efficient) {
    char buffer[10];
    // Values 0-127 should use only 1 byte
    for (uint32_t i = 0; i <= 127; i++) {
        size_t len = encode_varint32(buffer, i);
        FB_ASSERT_EQ(len, 1);
    }
}

FB_TEST(varint_encoding_efficiency, medium_values_efficiency) {
    char buffer[10];
    // Values 128-16383 should use 2 bytes
    FB_ASSERT_EQ(encode_varint32(buffer, 128), 2);
    FB_ASSERT_EQ(encode_varint32(buffer, 16383), 2);
}

FB_TEST(varint_encoding_efficiency, large_values_efficiency) {
    char buffer[10];
    // Values 16384-2097151 should use 3 bytes
    FB_ASSERT_EQ(encode_varint32(buffer, 16384), 3);
    FB_ASSERT_EQ(encode_varint32(buffer, 2097151), 3);
}

FB_TEST(varint_encoding_efficiency, space_savings) {
    char varint_buf[10];
    char fixed_buf[8];

    // For small values, varint saves space compared to fixed
    uint32_t small = 127;
    size_t varint_len = encode_varint32(varint_buf, small);
    size_t fixed_len = 4;

    FB_ASSERT_TRUE(varint_len < fixed_len);
}

// ============================================================================
// Test Suite: md5_collision (MD5 Collision Resistance Tests)
// ============================================================================

FB_SUITE_SETUP(md5_collision) {
    // Setup code here
}

FB_SUITE_TEARDOWN(md5_collision) {
    // Teardown code here
}

FB_TEST(md5_collision, sequential_strings) {
    std::string prev_hash;
    for (int i = 0; i < 100; i++) {
        std::string data = "test" + std::to_string(i);
        std::string hash = utils::md5(const_cast<char*>(data.c_str()), data.size());

        // Each hash should be different
        if (!prev_hash.empty()) {
            FB_ASSERT_TRUE(hash != prev_hash);
        }
        prev_hash = hash;
    }
}

FB_TEST(md5_collision, similar_strings) {
    std::string data1 = "string1";
    std::string data2 = "string2";

    std::string hash1 = utils::md5(const_cast<char*>(data1.c_str()), data1.size());
    std::string hash2 = utils::md5(const_cast<char*>(data2.c_str()), data2.size());

    // Similar strings should have different hashes
    FB_ASSERT_TRUE(hash1 != hash2);
}

FB_TEST(md5_collision, single_char_diff) {
    std::string data1 = "hello";
    std::string data2 = "hello!";

    std::string hash1 = utils::md5(const_cast<char*>(data1.c_str()), data1.size());
    std::string hash2 = utils::md5(const_cast<char*>(data2.c_str()), data2.size());

    // Single character difference should produce different hash
    FB_ASSERT_TRUE(hash1 != hash2);
}

FB_TEST(md5_collision, case_difference) {
    std::string data1 = "Hello";
    std::string data2 = "hello";

    std::string hash1 = utils::md5(const_cast<char*>(data1.c_str()), data1.size());
    std::string hash2 = utils::md5(const_cast<char*>(data2.c_str()), data2.size());

    // Case difference should produce different hash
    FB_ASSERT_TRUE(hash1 != hash2);
}

// ============================================================================
// Test Suite: encoding_decode_edge_cases (Encoding Decode Edge Cases)
// ============================================================================

FB_SUITE_SETUP(encoding_decode_edge_cases) {
    // Setup code here
}

FB_SUITE_TEARDOWN(encoding_decode_edge_cases) {
    // Teardown code here
}

FB_TEST(encoding_decode_edge_cases, varint32_min_buffer) {
    char buffer[5];
    uint32_t value = 1;
    size_t len = encode_varint32(buffer, value);
    FB_ASSERT_TRUE(len >= 1);

    auto [decoded, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(encoding_decode_edge_cases, varint64_min_buffer) {
    char buffer[10];
    uint64_t value = 1;
    size_t len = encode_varint64(buffer, value);
    FB_ASSERT_TRUE(len >= 1);

    auto [decoded, decoded_len] = decode_varint64(buffer, len);
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(encoding_decode_edge_cases, fixed32_min_value) {
    char buffer[4];
    encode_fixed32(buffer, 0);
    uint32_t decoded = decode_fixed32(buffer);
    FB_ASSERT_EQ(decoded, 0);
}

FB_TEST(encoding_decode_edge_cases, fixed64_min_value) {
    char buffer[8];
    encode_fixed64(buffer, 0);
    uint64_t decoded = decode_fixed64(buffer);
    FB_ASSERT_EQ(decoded, 0);
}

FB_TEST(encoding_decode_edge_cases, fixed32_all_ones) {
    char buffer[4];
    encode_fixed32(buffer, 0xFFFFFFFF);
    uint32_t decoded = decode_fixed32(buffer);
    FB_ASSERT_EQ(decoded, 0xFFFFFFFF);
}

FB_TEST(encoding_decode_edge_cases, fixed64_all_ones) {
    char buffer[8];
    encode_fixed64(buffer, 0xFFFFFFFFFFFFFFFFULL);
    uint64_t decoded = decode_fixed64(buffer);
    FB_ASSERT_EQ(decoded, 0xFFFFFFFFFFFFFFFFULL);
}

FB_TEST_MAIN()
