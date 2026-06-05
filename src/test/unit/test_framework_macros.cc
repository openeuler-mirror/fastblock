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

// Main function for test runner
FB_TEST_MAIN()