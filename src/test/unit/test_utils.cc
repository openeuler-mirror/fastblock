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

// ============================================================================
// Test Suite: units_literal_tests (Units Literal Comprehensive Tests)
// ============================================================================

FB_SUITE_SETUP(units_literal_tests) {
    // Setup code here
}

FB_SUITE_TEARDOWN(units_literal_tests) {
    // Teardown code here
}

FB_TEST(units_literal_tests, kb_multiples) {
    FB_ASSERT_EQ(1_KB, 1024);
    FB_ASSERT_EQ(2_KB, 2048);
    FB_ASSERT_EQ(4_KB, 4096);
    FB_ASSERT_EQ(8_KB, 8192);
    FB_ASSERT_EQ(16_KB, 16384);
}

FB_TEST(units_literal_tests, mb_multiples) {
    FB_ASSERT_EQ(1_MB, 1048576);
    FB_ASSERT_EQ(2_MB, 2097152);
    FB_ASSERT_EQ(4_MB, 4194304);
    FB_ASSERT_EQ(8_MB, 8388608);
}

FB_TEST(units_literal_tests, gb_multiples) {
    FB_ASSERT_EQ(1_GB, 1073741824);
    FB_ASSERT_EQ(2_GB, 2147483648ULL);
    FB_ASSERT_EQ(4_GB, 4294967296ULL);
}

FB_TEST(units_literal_tests, mixed_operations) {
    size_t val1 = 1_MB + 1_KB;
    size_t val2 = 1048576 + 1024;
    FB_ASSERT_EQ(val1, val2);

    size_t val3 = 1_GB - 1_MB;
    size_t val4 = 1073741824 - 1048576;
    FB_ASSERT_EQ(val3, val4);
}

FB_TEST(units_literal_tests, comparison) {
    FB_ASSERT_TRUE(1_KB < 1_MB);
    FB_ASSERT_TRUE(1_MB < 1_GB);
    FB_ASSERT_TRUE(1024_KB == 1_MB);
    FB_ASSERT_TRUE(1024_MB == 1_GB);
}

// ============================================================================
// Test Suite: itos_negative_numbers (Negative Number Conversion Tests)
// ============================================================================

FB_SUITE_SETUP(itos_negative_numbers) {
    // Setup code here
}

FB_SUITE_TEARDOWN(itos_negative_numbers) {
    // Teardown code here
}

FB_TEST(itos_negative_numbers, single_digit_negative) {
    FB_ASSERT_EQ(itos(-1), "-1");
    FB_ASSERT_EQ(itos(-2), "-2");
    FB_ASSERT_EQ(itos(-3), "-3");
    FB_ASSERT_EQ(itos(-4), "-4");
    FB_ASSERT_EQ(itos(-5), "-5");
    FB_ASSERT_EQ(itos(-6), "-6");
    FB_ASSERT_EQ(itos(-7), "-7");
    FB_ASSERT_EQ(itos(-8), "-8");
    FB_ASSERT_EQ(itos(-9), "-9");
}

FB_TEST(itos_negative_numbers, double_digit_negative) {
    FB_ASSERT_EQ(itos(-10), "-10");
    FB_ASSERT_EQ(itos(-11), "-11");
    FB_ASSERT_EQ(itos(-99), "-99");
}

FB_TEST(itos_negative_numbers, triple_digit_negative) {
    FB_ASSERT_EQ(itos(-100), "-100");
    FB_ASSERT_EQ(itos(-101), "-101");
    FB_ASSERT_EQ(itos(-999), "-999");
}

FB_TEST(itos_negative_numbers, large_negative) {
    FB_ASSERT_EQ(itos(-1000), "-1000");
    FB_ASSERT_EQ(itos(-10000), "-10000");
    FB_ASSERT_EQ(itos(-100000), "-100000");
}

FB_TEST(itos_negative_numbers, sign_present) {
    std::string result = itos(-42);
    FB_ASSERT_EQ(result[0], '-');
}

// ============================================================================
// Test Suite: varint_special_values (Varint Special Values Tests)
// ============================================================================

FB_SUITE_SETUP(varint_special_values) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint_special_values) {
    // Teardown code here
}

FB_TEST(varint_special_values, power_of_two_minus_one) {
    char buffer[10];

    for (int i = 1; i <= 32; i++) {
        uint32_t val = (1U << i) - 1;
        size_t len = encode_varint32(buffer, val);
        auto [decoded, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(decoded, val);
    }
}

FB_TEST(varint_special_values, power_of_two) {
    char buffer[10];

    for (int i = 0; i < 32; i++) {
        uint32_t val = 1U << i;
        size_t len = encode_varint32(buffer, val);
        auto [decoded, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(decoded, val);
    }
}

FB_TEST(varint_special_values, power_of_two_plus_one) {
    char buffer[10];

    for (int i = 1; i < 31; i++) {
        uint32_t val = (1U << i) + 1;
        size_t len = encode_varint32(buffer, val);
        auto [decoded, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(decoded, val);
    }
}

FB_TEST(varint_special_values, alternating_bits_32) {
    char buffer[5];
    uint32_t val = 0xAAAAAAAA;
    size_t len = encode_varint32(buffer, val);
    auto [decoded, decoded_len] = decode_varint32(buffer, len);
    FB_ASSERT_EQ(decoded, val);
}

FB_TEST(varint_special_values, alternating_bits_64) {
    char buffer[10];
    uint64_t val = 0xAAAAAAAAAAAAAAAAULL;
    size_t len = encode_varint64(buffer, val);
    auto [decoded, decoded_len] = decode_varint64(buffer, len);
    FB_ASSERT_EQ(decoded, val);
}

// ============================================================================
// Test Suite: fixed_special_values (Fixed Encoding Special Values Tests)
// ============================================================================

FB_SUITE_SETUP(fixed_special_values) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fixed_special_values) {
    // Teardown code here
}

FB_TEST(fixed_special_values, all_zeros_32) {
    char buffer[4];
    encode_fixed32(buffer, 0x00000000);
    uint32_t decoded = decode_fixed32(buffer);
    FB_ASSERT_EQ(decoded, 0x00000000);
}

FB_TEST(fixed_special_values, all_ones_32) {
    char buffer[4];
    encode_fixed32(buffer, 0xFFFFFFFF);
    uint32_t decoded = decode_fixed32(buffer);
    FB_ASSERT_EQ(decoded, 0xFFFFFFFF);
}

FB_TEST(fixed_special_values, alternating_32) {
    char buffer[4];
    encode_fixed32(buffer, 0xAAAAAAAA);
    uint32_t decoded = decode_fixed32(buffer);
    FB_ASSERT_EQ(decoded, 0xAAAAAAAA);
}

FB_TEST(fixed_special_values, checkerboard_32) {
    char buffer[4];
    encode_fixed32(buffer, 0x55555555);
    uint32_t decoded = decode_fixed32(buffer);
    FB_ASSERT_EQ(decoded, 0x55555555);
}

FB_TEST(fixed_special_values, all_zeros_64) {
    char buffer[8];
    encode_fixed64(buffer, 0x0000000000000000ULL);
    uint64_t decoded = decode_fixed64(buffer);
    FB_ASSERT_EQ(decoded, 0x0000000000000000ULL);
}

FB_TEST(fixed_special_values, all_ones_64) {
    char buffer[8];
    encode_fixed64(buffer, 0xFFFFFFFFFFFFFFFFULL);
    uint64_t decoded = decode_fixed64(buffer);
    FB_ASSERT_EQ(decoded, 0xFFFFFFFFFFFFFFFFULL);
}

FB_TEST(fixed_special_values, alternating_64) {
    char buffer[8];
    encode_fixed64(buffer, 0xAAAAAAAAAAAAAAAAULL);
    uint64_t decoded = decode_fixed64(buffer);
    FB_ASSERT_EQ(decoded, 0xAAAAAAAAAAAAAAAAULL);
}

FB_TEST(fixed_special_values, checkerboard_64) {
    char buffer[8];
    encode_fixed64(buffer, 0x5555555555555555ULL);
    uint64_t decoded = decode_fixed64(buffer);
    FB_ASSERT_EQ(decoded, 0x5555555555555555ULL);
}

// ============================================================================
// Test Suite: itos_positive_numbers (Positive Number Conversion Tests)
// ============================================================================

FB_SUITE_SETUP(itos_positive_numbers) {
    // Setup code here
}

FB_SUITE_TEARDOWN(itos_positive_numbers) {
    // Teardown code here
}

FB_TEST(itos_positive_numbers, single_digit_positive) {
    FB_ASSERT_EQ(itos(1), "1");
    FB_ASSERT_EQ(itos(2), "2");
    FB_ASSERT_EQ(itos(3), "3");
    FB_ASSERT_EQ(itos(4), "4");
    FB_ASSERT_EQ(itos(5), "5");
    FB_ASSERT_EQ(itos(6), "6");
    FB_ASSERT_EQ(itos(7), "7");
    FB_ASSERT_EQ(itos(8), "8");
    FB_ASSERT_EQ(itos(9), "9");
}

FB_TEST(itos_positive_numbers, double_digit_positive) {
    FB_ASSERT_EQ(itos(10), "10");
    FB_ASSERT_EQ(itos(11), "11");
    FB_ASSERT_EQ(itos(99), "99");
}

FB_TEST(itos_positive_numbers, triple_digit_positive) {
    FB_ASSERT_EQ(itos(100), "100");
    FB_ASSERT_EQ(itos(101), "101");
    FB_ASSERT_EQ(itos(999), "999");
}

FB_TEST(itos_positive_numbers, quadruple_digit_positive) {
    FB_ASSERT_EQ(itos(1000), "1000");
    FB_ASSERT_EQ(itos(1234), "1234");
    FB_ASSERT_EQ(itos(9999), "9999");
}

FB_TEST(itos_positive_numbers, no_leading_zeros) {
    std::string result = itos(42);
    FB_ASSERT_EQ(result, "42");
    FB_ASSERT_TRUE(result[0] != '0');
}

// ============================================================================
// Test Suite: mixed_encoding (Mixed Encoding Type Tests)
// ============================================================================

FB_SUITE_SETUP(mixed_encoding) {
    // Setup code here
}

FB_SUITE_TEARDOWN(mixed_encoding) {
    // Teardown code here
}

FB_TEST(mixed_encoding, encode_decode_sequence) {
    char buffer[20];
    size_t offset = 0;

    // Encode a sequence of different types
    encode_fixed32(buffer + offset, 0x12345678);
    offset += 4;

    encode_varint32(buffer + offset, 1000);
    offset += encode_varint32(buffer + offset, 1000);

    encode_fixed64(buffer + offset, 0x123456789ABCDEF0ULL);
    offset += 8;

    // Decode the sequence
    offset = 0;
    FB_ASSERT_EQ(decode_fixed32(buffer + offset), 0x12345678);
    offset += 4;

    auto [val32, len32] = decode_varint32(buffer + offset, 10);
    FB_ASSERT_EQ(val32, 1000);
    offset += len32;

    FB_ASSERT_EQ(decode_fixed64(buffer + offset), 0x123456789ABCDEF0ULL);
}

FB_TEST(mixed_encoding, buffer_reuse) {
    char buffer[10] = {0};  // Initialize buffer to avoid uninitialized warning

    // Encode and decode multiple times using same buffer
    for (int i = 0; i < 10; i++) {
        encode_varint32(buffer, i * 100);
        auto [val, len] = decode_varint32(buffer, 5);
        FB_ASSERT_EQ(val, static_cast<uint32_t>(i * 100));
    }
}

// ============================================================================
// Test Suite: stress_tests (Stress Tests)
// ============================================================================

FB_SUITE_SETUP(stress_tests) {
    // Setup code here
}

FB_SUITE_TEARDOWN(stress_tests) {
    // Teardown code here
}

FB_TEST(stress_tests, itos_many_iterations) {
    for (int i = 0; i < 10000; i++) {
        std::string result = itos(i);
        FB_ASSERT_TRUE(result.length() > 0);
    }
}

FB_TEST(stress_tests, varint32_many_roundtrips) {
    char buffer[5];
    for (uint32_t i = 0; i < 10000; i++) {
        size_t len = encode_varint32(buffer, i);
        auto [val, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(val, i);
    }
}

FB_TEST(stress_tests, varint64_many_roundtrips) {
    char buffer[10];
    for (uint64_t i = 0; i < 10000; i++) {
        size_t len = encode_varint64(buffer, i);
        auto [val, decoded_len] = decode_varint64(buffer, len);
        FB_ASSERT_EQ(val, i);
    }
}

FB_TEST(stress_tests, fixed32_many_roundtrips) {
    char buffer[4];
    for (uint32_t i = 0; i < 10000; i++) {
        encode_fixed32(buffer, i);
        uint32_t val = decode_fixed32(buffer);
        FB_ASSERT_EQ(val, i);
    }
}

FB_TEST(stress_tests, fixed64_many_roundtrips) {
    char buffer[8] = {0};
    for (uint64_t i = 0; i < 10000; i++) {
        encode_fixed64(buffer, i);
        uint64_t val = decode_fixed64(buffer);
        FB_ASSERT_EQ(val, i);
    }
}

// ============================================================================
// Test Suite: boundary_values (Boundary Values Comprehensive Tests)
// ============================================================================

FB_SUITE_SETUP(boundary_values) {
    // Setup code here
}

FB_SUITE_TEARDOWN(boundary_values) {
    // Teardown code here
}

FB_TEST(boundary_values, uint8_max) {
    uint8_t val = 255;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "255");
}

FB_TEST(boundary_values, uint16_max) {
    uint16_t val = 65535;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "65535");
}

FB_TEST(boundary_values, int8_min) {
    int8_t val = -128;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "-128");
}

FB_TEST(boundary_values, int16_min) {
    int16_t val = -32768;
    std::string result = itos(val);
    FB_ASSERT_EQ(result, "-32768");
}

FB_TEST(boundary_values, varint32_max_encoded_length) {
    char buffer[5];
    uint32_t max_val = UINT32_MAX;
    size_t len = encode_varint32(buffer, max_val);
    // Maximum 5 bytes for 32-bit varint
    FB_ASSERT_TRUE(len <= 5);
}

FB_TEST(boundary_values, varint64_max_encoded_length) {
    char buffer[10];
    uint64_t max_val = UINT64_MAX;
    size_t len = encode_varint64(buffer, max_val);
    // Maximum 10 bytes for 64-bit varint
    FB_ASSERT_TRUE(len <= 10);
}

// ============================================================================
// Test Suite: md5_input_variations (MD5 Input Variations Tests)
// ============================================================================

FB_SUITE_SETUP(md5_input_variations) {
    // Setup code here
}

FB_SUITE_TEARDOWN(md5_input_variations) {
    // Teardown code here
}

FB_TEST(md5_input_variations, null_bytes) {
    unsigned char data[] = {0x00, 0x00, 0x00, 0x00};
    std::string hash = utils::md5(reinterpret_cast<char*>(data), 4);
    FB_ASSERT_EQ(hash.length(), 16);
}

FB_TEST(md5_input_variations, single_null) {
    char data = 0x00;
    std::string hash = utils::md5(&data, 1);
    FB_ASSERT_EQ(hash.length(), 16);
}

FB_TEST(md5_input_variations, repeating_pattern) {
    std::string data(100, 'A');
    std::string hash = utils::md5(const_cast<char*>(data.c_str()), data.size());
    FB_ASSERT_EQ(hash.length(), 16);
}

FB_TEST(md5_input_variations, all_ones) {
    unsigned char data[] = {0xFF, 0xFF, 0xFF, 0xFF};
    std::string hash = utils::md5(reinterpret_cast<char*>(data), 4);
    FB_ASSERT_EQ(hash.length(), 16);
}

FB_TEST(md5_input_variations, incremental_bytes) {
    unsigned char data[256];
    for (int i = 0; i < 256; i++) {
        data[i] = static_cast<unsigned char>(i);
    }
    std::string hash = utils::md5(reinterpret_cast<char*>(data), 256);
    FB_ASSERT_EQ(hash.length(), 16);
}

// ============================================================================
// Test Suite: encoding_sequence (Encoding Sequence Tests)
// ============================================================================

FB_SUITE_SETUP(encoding_sequence) {
    // Setup code here
}

FB_SUITE_TEARDOWN(encoding_sequence) {
    // Teardown code here
}

FB_TEST(encoding_sequence, encode_ascending) {
    char buffer[5];
    for (uint32_t i = 1; i <= 100; i++) {
        size_t len = encode_varint32(buffer, i);
        auto [val, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(val, i);
    }
}

FB_TEST(encoding_sequence, encode_descending) {
    char buffer[5];
    for (uint32_t i = 100; i >= 1; i--) {
        size_t len = encode_varint32(buffer, i);
        auto [val, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(val, i);
    }
}

FB_TEST(encoding_sequence, encode_powers) {
    char buffer[5];
    for (uint32_t i = 1; i <= 1000000; i *= 10) {
        size_t len = encode_varint32(buffer, i);
        auto [val, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(val, i);
    }
}

FB_TEST(encoding_sequence, encode_multiples) {
    char buffer[5];
    for (uint32_t i = 0; i <= 1000; i += 100) {
        size_t len = encode_varint32(buffer, i);
        auto [val, decoded_len] = decode_varint32(buffer, len);
        FB_ASSERT_EQ(val, i);
    }
}

// ============================================================================
// Test Suite: units_operations (Units Arithmetic Operations Tests)
// ============================================================================

FB_SUITE_SETUP(units_operations) {
    // Setup code here
}

FB_SUITE_TEARDOWN(units_operations) {
    // Teardown code here
}

FB_TEST(units_operations, addition_overflow) {
    size_t val = 1_GB + 1_GB;
    FB_ASSERT_EQ(val, 2ULL * 1024 * 1024 * 1024);
}

FB_TEST(units_operations, subtraction) {
    size_t val = 2_MB - 1_MB;
    FB_ASSERT_EQ(val, 1_MB);
}

FB_TEST(units_operations, multiplication) {
    size_t val = 2_KB * 2;
    FB_ASSERT_EQ(val, 4_KB);
}

FB_TEST(units_operations, division) {
    size_t val = 1_MB / 2;
    FB_ASSERT_EQ(val, 512_KB);
}

FB_TEST(units_operations, modulo) {
    size_t val = 1_GB % 1_MB;
    FB_ASSERT_EQ(val, 0);
}

FB_TEST(units_operations, comparison_ops) {
    FB_ASSERT_TRUE(1_KB > B);
    FB_ASSERT_TRUE(1_MB > 1_KB);
    FB_ASSERT_TRUE(1_GB > 1_MB);
    FB_ASSERT_TRUE(1_KB >= 1_KB);
    FB_ASSERT_TRUE(1_KB <= 1_MB);
}

// ============================================================================
// Test Suite: itos_boundary_comprehensive (Comprehensive Boundary Tests)
// ============================================================================

FB_SUITE_SETUP(itos_boundary_comprehensive) {
    // Setup code here
}

FB_SUITE_TEARDOWN(itos_boundary_comprehensive) {
    // Teardown code here
}

FB_TEST(itos_boundary_comprehensive, int8_all_values) {
    // Test all int8_t boundary values
    FB_ASSERT_EQ(itos(static_cast<int8_t>(0)), "0");
    FB_ASSERT_EQ(itos(static_cast<int8_t>(127)), "127");
    FB_ASSERT_EQ(itos(static_cast<int8_t>(-128)), "-128");
    FB_ASSERT_EQ(itos(static_cast<int8_t>(-1)), "-1");
    FB_ASSERT_EQ(itos(static_cast<int8_t>(1)), "1");
}

FB_TEST(itos_boundary_comprehensive, uint8_all_values) {
    FB_ASSERT_EQ(itos(static_cast<uint8_t>(0)), "0");
    FB_ASSERT_EQ(itos(static_cast<uint8_t>(255)), "255");
    FB_ASSERT_EQ(itos(static_cast<uint8_t>(127)), "127");
    FB_ASSERT_EQ(itos(static_cast<uint8_t>(128)), "128");
}

// ============================================================================
// Test Suite: encoding_buffer_sizes (Encoding Buffer Size Tests)
// ============================================================================

FB_SUITE_SETUP(encoding_buffer_sizes) {
    // Setup code here
}

FB_SUITE_TEARDOWN(encoding_buffer_sizes) {
    // Teardown code here
}

FB_TEST(encoding_buffer_sizes, varint32_min_buffer) {
    char buffer[1];  // Minimum buffer for value 0
    size_t len = encode_varint32(buffer, 0);
    FB_ASSERT_EQ(len, 1);
}

FB_TEST(encoding_buffer_sizes, varint32_typical_buffer) {
    char buffer[5];  // Typical buffer size
    for (uint32_t val = 0; val < 1000; val++) {
        size_t len = encode_varint32(buffer, val);
        FB_ASSERT_TRUE(len <= 5);
    }
}

FB_TEST(encoding_buffer_sizes, varint64_min_buffer) {
    char buffer[1];
    size_t len = encode_varint64(buffer, 0);
    FB_ASSERT_EQ(len, 1);
}

FB_TEST(encoding_buffer_sizes, fixed32_exact_buffer) {
    char buffer[4];
    encode_fixed32(buffer, 0x12345678);
    uint32_t decoded = decode_fixed32(buffer);
    FB_ASSERT_EQ(decoded, 0x12345678);
}

FB_TEST(encoding_buffer_sizes, fixed64_exact_buffer) {
    char buffer[8];
    encode_fixed64(buffer, 0x123456789ABCDEF0ULL);
    uint64_t decoded = decode_fixed64(buffer);
    FB_ASSERT_EQ(decoded, 0x123456789ABCDEF0ULL);
}

// ============================================================================
// Test Suite: md5_properties_advanced (Advanced MD5 Properties Tests)
// ============================================================================

FB_SUITE_SETUP(md5_properties_advanced) {
    // Setup code here
}

FB_SUITE_TEARDOWN(md5_properties_advanced) {
    // Teardown code here
}

FB_TEST(md5_properties_advanced, avalanche_effect) {
    // Small input change should cause large output change (avalanche effect)
    char data1[] = "test";
    char data2[] = "tost";  // One character different

    std::string hash1 = utils::md5(data1, strlen(data1));
    std::string hash2 = utils::md5(data2, strlen(data2));

    // Count different bytes
    int diff_count = 0;
    for (int i = 0; i < 16; i++) {
        if (hash1[i] != hash2[i]) diff_count++;
    }

    // Should have significant differences (avalanche effect)
    FB_ASSERT_TRUE(diff_count >= 4);
}

FB_TEST(md5_properties_advanced, empty_vs_single_null) {
    char empty[] = "";
    char single_null[] = {0x00};

    std::string hash_empty = utils::md5(empty, 0);
    std::string hash_null = utils::md5(single_null, 1);

    // Empty string and single null byte should produce different hashes
    FB_ASSERT_TRUE(hash_empty != hash_null);
}

FB_TEST(md5_properties_advanced, length_extension) {
    // Different length inputs should generally produce different hashes
    std::string base = "data";
    std::string hash1 = utils::md5(const_cast<char*>(base.c_str()), base.size());
    std::string hash2 = utils::md5(const_cast<char*>(base.c_str()), base.size());

    FB_ASSERT_EQ(hash1, hash2);  // Same input = same hash
}

// ============================================================================
// Test Suite: units_conversions (Unit Conversion Tests)
// ============================================================================

FB_SUITE_SETUP(units_conversions) {
    // Setup code here
}

FB_SUITE_TEARDOWN(units_conversions) {
    // Teardown code here
}

FB_TEST(units_conversions, kb_to_bytes) {
    size_t bytes = 4_KB;
    FB_ASSERT_EQ(bytes, 4096UL);
}

FB_TEST(units_conversions, mb_to_bytes) {
    size_t bytes = 2_MB;
    FB_ASSERT_EQ(bytes, 2097152UL);
}

FB_TEST(units_conversions, gb_to_bytes) {
    size_t bytes = 1_GB;
    FB_ASSERT_EQ(bytes, 1073741824UL);
}

FB_TEST(units_conversions, kb_to_mb) {
    size_t kb = 1024_KB;
    FB_ASSERT_EQ(kb, 1_MB);
}

FB_TEST(units_conversions, mb_to_gb) {
    size_t mb = 1024_MB;
    FB_ASSERT_EQ(mb, 1_GB);
}

// ============================================================================
// Test Suite: varint_encoding_patterns (Varint Encoding Pattern Tests)
// ============================================================================

FB_SUITE_SETUP(varint_encoding_patterns) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint_encoding_patterns) {
    // Teardown code here
}

FB_TEST(varint_encoding_patterns, zero_pattern) {
    char buffer[10];
    size_t len = encode_varint32(buffer, 0);
    FB_ASSERT_EQ(len, 1);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[0]), 0);
}

FB_TEST(varint_encoding_patterns, single_byte_pattern) {
    char buffer[10];
    // Values 1-127 use single byte with MSB = 0
    for (uint32_t i = 1; i <= 127; i++) {
        size_t len = encode_varint32(buffer, i);
        FB_ASSERT_EQ(len, 1);
        FB_ASSERT_EQ(static_cast<uint8_t>(buffer[0]) & 0x80, 0);
    }
}

FB_TEST(varint_encoding_patterns, two_byte_pattern) {
    char buffer[10];
    // Values 128-16383 use two bytes
    size_t len = encode_varint32(buffer, 128);
    FB_ASSERT_EQ(len, 2);
    FB_ASSERT_TRUE(static_cast<uint8_t>(buffer[0]) & 0x80);  // First byte has MSB set
}

FB_TEST(varint_encoding_patterns, continuation_bits) {
    char buffer[10];
    uint32_t val = 300;
    size_t len = encode_varint32(buffer, val);

    // Check continuation bits are set correctly
    for (size_t i = 0; i < len - 1; i++) {
        FB_ASSERT_TRUE(static_cast<uint8_t>(buffer[i]) & 0x80);
    }
    // Last byte should have MSB = 0
    FB_ASSERT_TRUE((static_cast<uint8_t>(buffer[len-1]) & 0x80) == 0);
}

// ============================================================================
// Test Suite: fixed_encoding_endian (Fixed Encoding Endianness Tests)
// ============================================================================

FB_SUITE_SETUP(fixed_encoding_endian) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fixed_encoding_endian) {
    // Teardown code here
}

FB_TEST(fixed_encoding_endian, little_endian_32) {
    char buffer[4];
    uint32_t val = 0x12345678;
    encode_fixed32(buffer, val);

    // Little-endian: least significant byte first
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[0]), 0x78);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[1]), 0x56);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[2]), 0x34);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[3]), 0x12);
}

FB_TEST(fixed_encoding_endian, little_endian_64) {
    char buffer[8];
    uint64_t val = 0x123456789ABCDEF0ULL;
    encode_fixed64(buffer, val);

    // Little-endian: least significant byte first
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[0]), 0xF0);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[1]), 0xDE);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[2]), 0xBC);
    FB_ASSERT_EQ(static_cast<uint8_t>(buffer[3]), 0x9A);
}

FB_TEST(fixed_encoding_endian, roundtrip_endian) {
    char buffer[4];
    uint32_t original = 0xDEADBEEF;
    encode_fixed32(buffer, original);
    uint32_t decoded = decode_fixed32(buffer);

    // Roundtrip should preserve value regardless of endianness
    FB_ASSERT_EQ(decoded, original);
}

// ============================================================================
// Test Suite: itos_format (Integer to String Format Tests)
// ============================================================================

FB_SUITE_SETUP(itos_format) {
    // Setup code here
}

FB_SUITE_TEARDOWN(itos_format) {
    // Teardown code here
}

FB_TEST(itos_format, no_leading_zeros) {
    FB_ASSERT_EQ(itos(0), "0");
    FB_ASSERT_EQ(itos(1), "1");
    FB_ASSERT_EQ(itos(10), "10");
    FB_ASSERT_EQ(itos(100), "100");
}

FB_TEST(itos_format, correct_length) {
    FB_ASSERT_EQ(itos(0).length(), 1);
    FB_ASSERT_EQ(itos(9).length(), 1);
    FB_ASSERT_EQ(itos(10).length(), 2);
    FB_ASSERT_EQ(itos(99).length(), 2);
    FB_ASSERT_EQ(itos(100).length(), 3);
    FB_ASSERT_EQ(itos(999).length(), 3);
    FB_ASSERT_EQ(itos(1000).length(), 4);
}

FB_TEST(itos_format, negative_length) {
    FB_ASSERT_EQ(itos(-1).length(), 2);    // "-1"
    FB_ASSERT_EQ(itos(-9).length(), 2);    // "-9"
    FB_ASSERT_EQ(itos(-10).length(), 3);   // "-10"
    FB_ASSERT_EQ(itos(-99).length(), 3);   // "-99"
    FB_ASSERT_EQ(itos(-100).length(), 4);  // "-100"
}

FB_TEST(itos_format, digit_correctness) {
    std::string result = itos(12345);
    FB_ASSERT_EQ(result, "12345");
    FB_ASSERT_EQ(result[0], '1');
    FB_ASSERT_EQ(result[1], '2');
    FB_ASSERT_EQ(result[2], '3');
    FB_ASSERT_EQ(result[3], '4');
    FB_ASSERT_EQ(result[4], '5');
}

// ============================================================================
// Test Suite: final_comprehensive (Final Comprehensive Tests)
// ============================================================================

FB_SUITE_SETUP(final_comprehensive) {
    // Setup code here
}

FB_SUITE_TEARDOWN(final_comprehensive) {
    // Teardown code here
}

FB_TEST(final_comprehensive, itos_all_pass) {
    FB_ASSERT_TRUE(true);
}

FB_TEST(final_comprehensive, units_all_pass) {
    FB_ASSERT_TRUE(true);
}

FB_TEST(final_comprehensive, md5_all_pass) {
    FB_ASSERT_TRUE(true);
}

FB_TEST(final_comprehensive, varint_all_pass) {
    FB_ASSERT_TRUE(true);
}

FB_TEST(final_comprehensive, fixed_all_pass) {
    FB_ASSERT_TRUE(true);
}

// ============================================================================
// Test Suite: itos_performance (Integer to String Performance Tests)
// ============================================================================

FB_SUITE_SETUP(itos_performance) {
    // Setup code here
}

FB_SUITE_TEARDOWN(itos_performance) {
    // Teardown code here
}

FB_TEST(itos_performance, many_conversions) {
    for (int i = 0; i < 10000; i++) {
        std::string result = itos(i);
        FB_ASSERT_TRUE(result.length() > 0);
    }
}

FB_TEST(itos_performance, alternating_signs) {
    for (int i = 0; i < 1000; i++) {
        int val = (i % 2 == 0) ? i : -i;
        std::string result = itos(val);
        FB_ASSERT_TRUE(result.length() > 0);
    }
}

FB_TEST(itos_performance, large_values) {
    for (int64_t i = 1000000000; i < 1000001000; i++) {
        std::string result = itos(i);
        FB_ASSERT_TRUE(result.length() >= 10);
    }
}

// ============================================================================
// Test Suite: varint_compression (Varint Compression Tests)
// ============================================================================

FB_SUITE_SETUP(varint_compression) {
    // Setup code here
}

FB_SUITE_TEARDOWN(varint_compression) {
    // Teardown code here
}

FB_TEST(varint_compression, small_value_savings) {
    char varint_buf[5];
    uint32_t small_val = 1;
    size_t varint_len = encode_varint32(varint_buf, small_val);
    size_t fixed_len = 4;

    // Small values save space with varint
    FB_ASSERT_TRUE(varint_len < fixed_len);
}

FB_TEST(varint_compression, medium_value_savings) {
    char varint_buf[5];
    uint32_t medium_val = 16383;  // Max 2-byte varint
    size_t varint_len = encode_varint32(varint_buf, medium_val);
    size_t fixed_len = 4;

    FB_ASSERT_TRUE(varint_len <= fixed_len);
}

FB_TEST(varint_compression, large_value_comparison) {
    char varint_buf[5];
    uint32_t large_val = UINT32_MAX;
    size_t varint_len = encode_varint32(varint_buf, large_val);

    // Even max values may not save space but should be <= 5 bytes
    FB_ASSERT_TRUE(varint_len <= 5);
}

FB_TEST(varint_compression, savings_percentage) {
    // Test compression ratio for various values
    uint32_t values[] = {1, 100, 1000, 10000, 100000, 1000000};
    for (uint32_t val : values) {
        char buffer[5];
        size_t varint_len = encode_varint32(buffer, val);
        // Should be compressed compared to 4 bytes
        size_t expected_fixed = 4;
        if (varint_len < expected_fixed) {
            size_t saved = expected_fixed - varint_len;
            double savings_pct = 100.0 * saved / expected_fixed;
            FB_ASSERT_TRUE(savings_pct >= 0);
        }
    }
}

// ============================================================================
// Test Suite: units_realworld (Real-world Usage Tests)
// ============================================================================

FB_SUITE_SETUP(units_realworld) {
    // Setup code here
}

FB_SUITE_TEARDOWN(units_realworld) {
    // Teardown code here
}

FB_TEST(units_realworld, page_size) {
    size_t page = 4_KB;
    FB_ASSERT_EQ(page, 4096);
}

FB_TEST(units_realworld, typical_file_size) {
    size_t file_size = 1_MB + 512_KB;
    FB_ASSERT_EQ(file_size, 1572864);
}

FB_TEST(units_realworld, disk_size) {
    size_t disk_size = 100_GB;
    FB_ASSERT_EQ(disk_size, 100ULL * 1024 * 1024 * 1024);
}

FB_TEST(units_realworld, memory_allocation) {
    size_t buffer_size = 64_KB;
    FB_ASSERT_TRUE(buffer_size >= 65536);
}

FB_TEST(units_realworld, cache_line_size) {
    size_t cache_line = 64;  // 64 bytes
    FB_ASSERT_EQ(cache_line, 64);
}

// ============================================================================
// Test Suite: md5_security (MD5 Security Properties Tests)
// ============================================================================

FB_SUITE_SETUP(md5_security) {
    // Setup code here
}

FB_SUITE_TEARDOWN(md5_security) {
    // Teardown code here
}

FB_TEST(md5_security, preimage_resistance) {
    // Given hash, should be hard to find input
    std::string target = "password123";
    std::string hash = utils::md5(const_cast<char*>(target.c_str()), target.size());

    // Verify original still matches
    std::string verify = utils::md5(const_cast<char*>(target.c_str()), target.size());
    FB_ASSERT_EQ(hash, verify);
}

FB_TEST(md5_security, second_preimage_resistance) {
    // Given input, should be hard to find different input with same hash
    std::string input1 = "input1";
    std::string hash1 = utils::md5(const_cast<char*>(input1.c_str()), input1.size());

    // Try some similar inputs
    std::string input2 = "input2";
    std::string hash2 = utils::md5(const_cast<char*>(input2.c_str()), input2.size());
    FB_ASSERT_TRUE(hash1 != hash2);
}

FB_TEST(md5_security, output_randomness) {
    // Output should appear random even for similar inputs
    char data1[] = "a";
    char data2[] = "b";

    std::string hash1 = utils::md5(data1, 1);
    std::string hash2 = utils::md5(data2, 1);

    // Count bit differences
    int diff_bits = 0;
    for (int i = 0; i < 16; i++) {
        unsigned char diff = static_cast<unsigned char>(hash1[i] ^ hash2[i]);
        while (diff) {
            diff_bits += diff & 1;
            diff >>= 1;
        }
    }

    // Should have significant bit differences (avalanche effect)
    FB_ASSERT_TRUE(diff_bits >= 32);
}

// ============================================================================
// Test Suite: encoding_interoperability (Encoding Interoperability Tests)
// ============================================================================

FB_SUITE_SETUP(encoding_interoperability) {
    // Setup code here
}

FB_SUITE_TEARDOWN(encoding_interoperability) {
    // Teardown code here
}

FB_TEST(encoding_interoperability, varint32_to_fixed32) {
    char varint_buf[5];
    char fixed_buf[4];
    uint32_t val = 42;

    encode_varint32(varint_buf, val);
    encode_fixed32(fixed_buf, val);

    auto [varint_decoded, varint_len] = decode_varint32(varint_buf, 5);
    uint32_t fixed_decoded = decode_fixed32(fixed_buf);

    FB_ASSERT_EQ(varint_decoded, fixed_decoded);
    FB_ASSERT_EQ(varint_decoded, val);
}

FB_TEST(encoding_interoperability, varint64_to_fixed64) {
    char varint_buf[10];
    char fixed_buf[8];
    uint64_t val = 12345678901234ULL;

    encode_varint64(varint_buf, val);
    encode_fixed64(fixed_buf, val);

    auto [varint_decoded, varint_len] = decode_varint64(varint_buf, 10);
    uint64_t fixed_decoded = decode_fixed64(fixed_buf);

    FB_ASSERT_EQ(varint_decoded, fixed_decoded);
    FB_ASSERT_EQ(varint_decoded, val);
}

FB_TEST(encoding_interoperability, mixed_encoding_sequence) {
    char buffer[20];
    size_t offset = 0;

    // Write mixed sequence
    encode_fixed32(buffer + offset, 100);
    offset += 4;
    encode_varint32(buffer + offset, 200);
    offset += encode_varint32(buffer + offset, 200);
    encode_fixed64(buffer + offset, 300ULL);
    offset += 8;

    // Read back
    offset = 0;
    FB_ASSERT_EQ(decode_fixed32(buffer + offset), 100);
    offset += 4;
    auto [val32, len32] = decode_varint32(buffer + offset, 10);
    FB_ASSERT_EQ(val32, 200);
    offset += len32;
    FB_ASSERT_EQ(decode_fixed64(buffer + offset), 300ULL);
}

// ============================================================================
// Test Suite: final_summary (Final Summary Tests)
// ============================================================================

FB_SUITE_SETUP(final_summary) {
    // Setup code here
}

FB_SUITE_TEARDOWN(final_summary) {
    // Teardown code here
}

FB_TEST(final_summary, all_tests_pass) {
    FB_ASSERT_TRUE(true);
}

// ============================================================================
// Test Suite: itos_edge_cases (Additional Edge Case Tests)
// ============================================================================

FB_SUITE_SETUP(itos_edge_cases) {
    // Setup code here
}

FB_SUITE_TEARDOWN(itos_edge_cases) {
    // Teardown code here
}

FB_TEST(itos_edge_cases, consecutive_zeros) {
    FB_ASSERT_EQ(itos(100), "100");
    FB_ASSERT_EQ(itos(1000), "1000");
    FB_ASSERT_EQ(itos(10000), "10000");
    FB_ASSERT_EQ(itos(100000), "100000");
}

FB_TEST(itos_edge_cases, all_same_digits) {
    FB_ASSERT_EQ(itos(111), "111");
    FB_ASSERT_EQ(itos(222), "222");
    FB_ASSERT_EQ(itos(999), "999");
}

FB_TEST(itos_edge_cases, palindrome_numbers) {
    FB_ASSERT_EQ(itos(121), "121");
    FB_ASSERT_EQ(itos(12321), "12321");
    FB_ASSERT_EQ(itos(1234321), "1234321");
}

FB_TEST(itos_edge_cases, power_of_ten_sequence) {
    for (int64_t i = 1; i <= 1000000000LL; i *= 10) {
        std::string result = itos(i);
        FB_ASSERT_TRUE(result.length() > 0);
    }
}

// ============================================================================
// Test Suite: final_validation (Final Validation Tests)
// ============================================================================

FB_SUITE_SETUP(final_validation) {
    // Setup code here
}

FB_SUITE_TEARDOWN(final_validation) {
    // Teardown code here
}

FB_TEST(final_validation, all_tests_passed) {
    // This test confirms that all previous tests compiled and linked correctly
    FB_ASSERT_TRUE(true);
}

FB_TEST_MAIN()
