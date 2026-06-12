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
 * @file test_framework_macros.cc
 * @brief Unit tests for framework assertion macros
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <cmath>
#include <limits>
#include <vector>
#include <map>
#include <set>

FB_SUITE_SETUP(framework_macros) {
    // Setup code here
}

FB_SUITE_TEARDOWN(framework_macros) {
    // Teardown code here
}

// ============================================================================
// Test Suite: Float Assertion Macros
// ============================================================================

FB_TEST(framework_macros, float_near_basic) {
    double a = 1.0;
    double b = 1.0001;
    double tolerance = 0.001;

    FB_ASSERT_FLOAT_NEAR(a, b, tolerance);
}

FB_TEST(framework_macros, float_near_exact_match) {
    double a = 5.0;
    double b = 5.0;
    double tolerance = 0.0001;

    FB_ASSERT_FLOAT_NEAR(a, b, tolerance);
}

FB_TEST(framework_macros, float_near_small_values) {
    double a = 0.00001;
    double b = 0.00002;
    double tolerance = 0.00001;

    FB_ASSERT_FLOAT_NEAR(a, b, tolerance);
}

FB_TEST(framework_macros, float_near_large_values) {
    double a = 1000000.0;
    double b = 1000000.1;
    double tolerance = 0.2;

    FB_ASSERT_FLOAT_NEAR(a, b, tolerance);
}

FB_TEST(framework_macros, float_eq_basic) {
    float a = 3.14159f;
    float b = 3.14159f;

    FB_ASSERT_FLOAT_EQ(a, b);
}

FB_TEST(framework_macros, float_eq_negative) {
    double a = -2.5;
    double b = -2.5;

    FB_ASSERT_FLOAT_EQ(a, b);
}

FB_TEST(framework_macros, float_eq_zero) {
    double a = 0.0;
    double b = 0.0;

    FB_ASSERT_FLOAT_EQ(a, b);
}

FB_TEST(framework_macros, near_int_values) {
    int a = 100;
    int b = 101;
    int tolerance = 2;

    FB_ASSERT_NEAR(a, b, tolerance);
}

FB_TEST(framework_macros, near_long_values) {
    long a = 1000000L;
    long b = 1000002L;
    long tolerance = 5L;

    FB_ASSERT_NEAR(a, b, tolerance);
}

FB_TEST(framework_macros, float_near_boundary) {
    double a = 10.0;
    double b = 10.001;
    double tolerance = 0.001;

    //刚好在边界
    FB_ASSERT_FLOAT_NEAR(a, b, tolerance);
}

FB_TEST(framework_macros, float_precision_different_types) {
    float f = 1.5f;
    double d = 1.5;
    double tolerance = 0.0001;

    FB_ASSERT_FLOAT_NEAR(f, d, tolerance);
}

// ============================================================================
// Test Suite: String Assertion Macros
// ============================================================================

FB_TEST(framework_macros, str_contains_basic) {
    std::string str = "hello world";
    std::string substr = "world";

    FB_ASSERT_STR_CONTAINS(str, substr);
}

FB_TEST(framework_macros, str_contains_empty) {
    std::string str = "hello";
    std::string substr = "";

    // Empty substring should always match
    FB_ASSERT_STR_CONTAINS(str, substr);
}

FB_TEST(framework_macros, str_contains_multiple) {
    std::string str = "the quick brown fox";
    std::string substr = "quick";

    FB_ASSERT_STR_CONTAINS(str, substr);
    substr = "brown";
    FB_ASSERT_STR_CONTAINS(str, substr);
    substr = "fox";
    FB_ASSERT_STR_CONTAINS(str, substr);
}

FB_TEST(framework_macros, str_contains_case_sensitive) {
    std::string str = "Hello World";
    std::string substr = "Hello";

    FB_ASSERT_STR_CONTAINS(str, substr);
}

FB_TEST(framework_macros, str_starts_with_basic) {
    std::string str = "prefix_suffix";
    std::string prefix = "prefix";

    FB_ASSERT_STR_STARTS_WITH(str, prefix);
}

FB_TEST(framework_macros, str_starts_with_empty_prefix) {
    std::string str = "hello";
    std::string prefix = "";

    // Empty prefix should match
    FB_ASSERT_STR_STARTS_WITH(str, prefix);
}

FB_TEST(framework_macros, str_starts_with_full_string) {
    std::string str = "complete";
    std::string prefix = "complete";

    FB_ASSERT_STR_STARTS_WITH(str, prefix);
}

FB_TEST(framework_macros, str_ends_with_basic) {
    std::string str = "prefix_suffix";
    std::string suffix = "suffix";

    FB_ASSERT_STR_ENDS_WITH(str, suffix);
}

FB_TEST(framework_macros, str_ends_with_empty_suffix) {
    std::string str = "hello";
    std::string suffix = "";

    // Empty suffix should match
    FB_ASSERT_STR_ENDS_WITH(str, suffix);
}

FB_TEST(framework_macros, str_ends_with_full_string) {
    std::string str = "complete";
    std::string suffix = "complete";

    FB_ASSERT_STR_ENDS_WITH(str, suffix);
}

FB_TEST(framework_macros, str_not_contains_basic) {
    std::string str = "hello world";
    std::string substr = "foo";

    FB_ASSERT_STR_NOT_CONTAINS(str, substr);
}

FB_TEST(framework_macros, str_not_contains_substring_present) {
    std::string str = "hello";
    std::string substr = "xyz";

    FB_ASSERT_STR_NOT_CONTAINS(str, substr);
}

FB_TEST(framework_macros, str_eq_with_normal_strings) {
    std::string expected = "test string";
    std::string actual = "test string";

    FB_ASSERT_STR_EQ(expected, actual);
}

FB_TEST(framework_macros, str_eq_with_empty_strings) {
    std::string expected = "";
    std::string actual = "";

    FB_ASSERT_STR_EQ(expected, actual);
}

FB_TEST(framework_macros, str_eq_with_c_strings) {
    const char* expected = "hello";
    const char* actual = "hello";

    FB_ASSERT_STR_EQ(expected, actual);
}

// ============================================================================
// Test Suite: Container Assertion Macros
// ============================================================================

FB_TEST(framework_macros, container_empty_vector) {
    std::vector<int> vec;

    FB_ASSERT_EMPTY(vec);
}

FB_TEST(framework_macros, container_empty_map) {
    std::map<int, int> m;

    FB_ASSERT_EMPTY(m);
}

FB_TEST(framework_macros, container_empty_set) {
    std::set<std::string> s;

    FB_ASSERT_EMPTY(s);
}

FB_TEST(framework_macros, container_not_empty_after_insert) {
    std::vector<int> vec;
    vec.push_back(1);

    // Vector is not empty now
    FB_ASSERT_TRUE(!vec.empty());
}

FB_TEST(framework_macros, container_size_vector) {
    std::vector<int> vec = {1, 2, 3, 4, 5};

    FB_ASSERT_SIZE(vec, 5);
}

FB_TEST(framework_macros, container_size_map) {
    std::map<int, std::string> m;
    m[1] = "one";
    m[2] = "two";
    m[3] = "three";

    FB_ASSERT_SIZE(m, 3);
}

FB_TEST(framework_macros, container_size_set) {
    std::set<int> s = {10, 20, 30, 40};

    FB_ASSERT_SIZE(s, 4);
}

FB_TEST(framework_macros, container_contains_vector) {
    std::vector<int> vec = {1, 2, 3, 4, 5};

    FB_ASSERT_CONTAINS(vec, 3);
}

FB_TEST(framework_macros, container_contains_set) {
    std::set<std::string> s = {"apple", "banana", "cherry"};

    FB_ASSERT_CONTAINS(s, "banana");
}

FB_TEST(framework_macros, container_contains_map_key) {
    std::map<int, std::string> m;
    m[1] = "one";
    m[2] = "two";

    // Map contains key
    FB_ASSERT_TRUE(m.count(1) > 0);
    FB_ASSERT_TRUE(m.count(2) > 0);
}

FB_TEST(framework_macros, container_not_contains_vector) {
    std::vector<int> vec = {1, 2, 3};

    FB_ASSERT_NOT_CONTAINS(vec, 10);
}

FB_TEST(framework_macros, container_not_contains_set) {
    std::set<int> s = {100, 200, 300};

    FB_ASSERT_NOT_CONTAINS(s, 999);
}

FB_TEST(framework_macros, container_clear_makes_empty) {
    std::vector<int> vec = {1, 2, 3};
    vec.clear();

    FB_ASSERT_EMPTY(vec);
}

FB_TEST(framework_macros, container_size_after_operations) {
    std::vector<int> vec;

    FB_ASSERT_SIZE(vec, 0);

    vec.push_back(1);
    vec.push_back(2);
    FB_ASSERT_SIZE(vec, 2);

    vec.pop_back();
    FB_ASSERT_SIZE(vec, 1);
}

// ============================================================================
// Test Suite: Range and Bits Assertion Macros
// ============================================================================

FB_TEST(framework_macros, in_range_basic) {
    int value = 50;
    int min_val = 0;
    int max_val = 100;

    FB_ASSERT_IN_RANGE(value, min_val, max_val);
}

FB_TEST(framework_macros, in_range_boundary_min) {
    int value = 0;
    int min_val = 0;
    int max_val = 10;

    FB_ASSERT_IN_RANGE(value, min_val, max_val);
}

FB_TEST(framework_macros, in_range_boundary_max) {
    int value = 10;
    int min_val = 0;
    int max_val = 10;

    FB_ASSERT_IN_RANGE(value, min_val, max_val);
}

FB_TEST(framework_macros, in_range_negative_bounds) {
    int value = -5;
    int min_val = -10;
    int max_val = 0;

    FB_ASSERT_IN_RANGE(value, min_val, max_val);
}

FB_TEST(framework_macros, in_range_large_values) {
    long value = 1000000L;
    long min_val = 0L;
    long max_val = 2000000L;

    FB_ASSERT_IN_RANGE(value, min_val, max_val);
}

FB_TEST(framework_macros, not_in_range_basic) {
    int value = 150;
    int min_val = 0;
    int max_val = 100;

    FB_ASSERT_NOT_IN_RANGE(value, min_val, max_val);
}

FB_TEST(framework_macros, not_in_range_below_min) {
    int value = -5;
    int min_val = 0;
    int max_val = 10;

    FB_ASSERT_NOT_IN_RANGE(value, min_val, max_val);
}

FB_TEST(framework_macros, not_in_range_above_max) {
    int value = 20;
    int min_val = 5;
    int max_val = 15;

    FB_ASSERT_NOT_IN_RANGE(value, min_val, max_val);
}

FB_TEST(framework_macros, bits_set_basic) {
    uint32_t value = 0x0F;  // 00001111
    uint32_t bits = 0x07;   // 00000111

    FB_ASSERT_BITS_SET(value, bits);
}

FB_TEST(framework_macros, bits_set_all) {
    uint32_t value = 0xFF;  // all bits set
    uint32_t bits = 0xFF;

    FB_ASSERT_BITS_SET(value, bits);
}

FB_TEST(framework_macros, bits_set_single_bit) {
    uint32_t value = 0x04;  // 00000100
    uint32_t bits = 0x04;

    FB_ASSERT_BITS_SET(value, bits);
}

FB_TEST(framework_macros, bits_clear_basic) {
    uint32_t value = 0xF0;  // 11110000
    uint32_t bits = 0x0F;   // 00001111

    FB_ASSERT_BITS_CLEAR(value, bits);
}

FB_TEST(framework_macros, bits_clear_all_low) {
    uint32_t value = 0x00;  // all bits clear
    uint32_t bits = 0xFF;

    FB_ASSERT_BITS_CLEAR(value, bits);
}

FB_TEST(framework_macros, bit_set_single) {
    uint32_t value = 0x08;  // bit 3 set
    int bit = 3;

    FB_ASSERT_BIT_SET(value, bit);
}

FB_TEST(framework_macros, bit_clear_single) {
    uint32_t value = 0xF7;  // bit 3 clear
    int bit = 3;

    FB_ASSERT_BIT_CLEAR(value, bit);
}

FB_TEST(framework_macros, bit_set_high_position) {
    uint32_t value = 0x80000000;  // bit 31 set
    int bit = 31;

    FB_ASSERT_BIT_SET(value, bit);
}

// ============================================================================
// Test Suite: Random Generator Macros
// ============================================================================

FB_TEST(framework_macros, random_int_basic) {
    int min = 0;
    int max = 100;

    for (int i = 0; i < 100; i++) {
        int value = FB_RANDOM_INT(min, max);
        FB_ASSERT_IN_RANGE(value, min, max);
    }
}

FB_TEST(framework_macros, random_int_negative_range) {
    int min = -100;
    int max = -10;

    for (int i = 0; i < 50; i++) {
        int value = FB_RANDOM_INT(min, max);
        FB_ASSERT_IN_RANGE(value, min, max);
    }
}

FB_TEST(framework_macros, random_int_same_min_max) {
    int min = 42;
    int max = 42;

    int value = FB_RANDOM_INT(min, max);
    FB_ASSERT_EQ(value, 42);
}

FB_TEST(framework_macros, random_uint64_basic) {
    uint64_t min = 0;
    uint64_t max = 1000000ULL;

    for (int i = 0; i < 50; i++) {
        uint64_t value = FB_RANDOM_UINT64(min, max);
        FB_ASSERT_TRUE(value >= min);
        FB_ASSERT_TRUE(value <= max);
    }
}

FB_TEST(framework_macros, random_uint64_large_range) {
    uint64_t min = 0;
    uint64_t max = std::numeric_limits<uint64_t>::max() / 2;

    uint64_t value = FB_RANDOM_UINT64(min, max);
    FB_ASSERT_TRUE(value >= min);
    FB_ASSERT_TRUE(value <= max);
}

FB_TEST(framework_macros, random_string_basic) {
    int len = 10;
    std::string str = FB_RANDOM_STRING(len);

    FB_ASSERT_EQ(static_cast<int>(str.length()), len);
}

FB_TEST(framework_macros, random_string_various_lengths) {
    for (int len = 1; len <= 100; len++) {
        std::string str = FB_RANDOM_STRING(len);
        FB_ASSERT_EQ(static_cast<int>(str.length()), len);
    }
}

FB_TEST(framework_macros, random_string_empty) {
    std::string str = FB_RANDOM_STRING(0);
    FB_ASSERT_TRUE(str.empty());
}

FB_TEST(framework_macros, random_bytes_basic) {
    int len = 16;
    std::string bytes = FB_RANDOM_BYTES(len);

    FB_ASSERT_EQ(static_cast<int>(bytes.length()), len);
}

FB_TEST(framework_macros, random_bytes_various_lengths) {
    for (int len = 1; len <= 32; len++) {
        std::string bytes = FB_RANDOM_BYTES(len);
        FB_ASSERT_EQ(static_cast<int>(bytes.length()), len);
    }
}

FB_TEST(framework_macros, random_double_basic) {
    double min = 0.0;
    double max = 1.0;

    for (int i = 0; i < 100; i++) {
        double value = FB_RANDOM_DOUBLE(min, max);
        FB_ASSERT_TRUE(value >= min);
        FB_ASSERT_TRUE(value <= max);
    }
}

FB_TEST(framework_macros, random_double_range) {
    double min = -100.5;
    double max = 200.5;

    for (int i = 0; i < 50; i++) {
        double value = FB_RANDOM_DOUBLE(min, max);
        FB_ASSERT_TRUE(value >= min);
        FB_ASSERT_TRUE(value <= max);
    }
}

FB_TEST(framework_macros, random_bool_basic) {
    int true_count = 0;
    int false_count = 0;

    for (int i = 0; i < 1000; i++) {
        if (FB_RANDOM_BOOL()) {
            true_count++;
        } else {
            false_count++;
        }
    }

    // Both should have reasonable distribution
    FB_ASSERT_TRUE(true_count > 200);
    FB_ASSERT_TRUE(false_count > 200);
}

FB_TEST(framework_macros, random_uniqueness) {
    std::set<int> values;

    for (int i = 0; i < 100; i++) {
        values.insert(FB_RANDOM_INT(0, 10000));
    }

    // Should have mostly unique values
    FB_ASSERT_TRUE(values.size() > 90);
}

// ============================================================================
// Test Suite: Predicate Macros
// ============================================================================

FB_TEST(framework_macros, pred_between_basic) {
    auto pred = FB_PRED_BETWEEN(0, 100);

    FB_ASSERT_TRUE(pred(50));
    FB_ASSERT_TRUE(pred(0));
    FB_ASSERT_TRUE(pred(100));
    FB_ASSERT_FALSE(pred(-1));
    FB_ASSERT_FALSE(pred(101));
}

FB_TEST(framework_macros, pred_between_negative) {
    auto pred = FB_PRED_BETWEEN(-50, 50);

    FB_ASSERT_TRUE(pred(-25));
    FB_ASSERT_TRUE(pred(0));
    FB_ASSERT_TRUE(pred(25));
    FB_ASSERT_FALSE(pred(-100));
    FB_ASSERT_FALSE(pred(100));
}

FB_TEST(framework_macros, pred_not_between_basic) {
    auto pred = FB_PRED_NOT_BETWEEN(0, 10);

    FB_ASSERT_TRUE(pred(-1));
    FB_ASSERT_TRUE(pred(11));
    FB_ASSERT_FALSE(pred(5));
    FB_ASSERT_FALSE(pred(0));
    FB_ASSERT_FALSE(pred(10));
}

FB_TEST(framework_macros, pred_null_basic) {
    auto pred = FB_PRED_NULL();

    int* null_ptr = nullptr;
    int* valid_ptr = new int(42);

    FB_ASSERT_TRUE(pred(null_ptr));
    FB_ASSERT_FALSE(pred(valid_ptr));

    delete valid_ptr;
}

FB_TEST(framework_macros, pred_not_null_basic) {
    auto pred = FB_PRED_NOT_NULL();

    int* null_ptr = nullptr;
    int* valid_ptr = new int(42);

    FB_ASSERT_FALSE(pred(null_ptr));
    FB_ASSERT_TRUE(pred(valid_ptr));

    delete valid_ptr;
}

FB_TEST(framework_macros, pred_one_of_basic) {
    std::vector<int> values = {1, 3, 5, 7, 9};
    auto pred = FB_PRED_ONE_OF(values);

    FB_ASSERT_TRUE(pred(1));
    FB_ASSERT_TRUE(pred(5));
    FB_ASSERT_TRUE(pred(9));
    FB_ASSERT_FALSE(pred(0));
    FB_ASSERT_FALSE(pred(2));
    FB_ASSERT_FALSE(pred(10));
}

FB_TEST(framework_macros, pred_none_of_basic) {
    std::vector<int> values = {1, 3, 5, 7, 9};
    auto pred = FB_PRED_NONE_OF(values);

    FB_ASSERT_FALSE(pred(1));
    FB_ASSERT_FALSE(pred(5));
    FB_ASSERT_TRUE(pred(0));
    FB_ASSERT_TRUE(pred(2));
    FB_ASSERT_TRUE(pred(10));
}

FB_TEST(framework_macros, pred_negate_basic) {
    auto is_positive = [](int x) { return x > 0; };
    auto is_not_positive = FB_PRED_NEGATE(is_positive);

    FB_ASSERT_FALSE(is_not_positive(5));
    FB_ASSERT_TRUE(is_not_positive(-5));
    FB_ASSERT_TRUE(is_not_positive(0));
}

FB_TEST(framework_macros, pred_and_basic) {
    auto is_positive = [](int x) { return x > 0; };
    auto is_even = [](int x) { return x % 2 == 0; };
    auto positive_and_even = FB_PRED_AND(is_positive, is_even);

    FB_ASSERT_TRUE(positive_and_even(2));
    FB_ASSERT_TRUE(positive_and_even(4));
    FB_ASSERT_FALSE(positive_and_even(-2));
    FB_ASSERT_FALSE(positive_and_even(1));
    FB_ASSERT_FALSE(positive_and_even(0));
}

FB_TEST(framework_macros, pred_or_basic) {
    auto is_negative = [](int x) { return x < 0; };
    auto is_large = [](int x) { return x > 100; };
    auto negative_or_large = FB_PRED_OR(is_negative, is_large);

    FB_ASSERT_TRUE(negative_or_large(-5));
    FB_ASSERT_TRUE(negative_or_large(200));
    FB_ASSERT_FALSE(negative_or_large(50));
    FB_ASSERT_FALSE(negative_or_large(0));
}

// Main function for test runner
FB_TEST_MAIN()