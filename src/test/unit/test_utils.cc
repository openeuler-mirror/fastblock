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
