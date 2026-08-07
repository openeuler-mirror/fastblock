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

#include "utils/itos.h"

#include <cassert>
#include <cstdio>
#include <climits>
#include <string>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

int main() {
    TEST_ASSERT(itos(0) == "0", "itos(0) should be \"0\"");
    TEST_ASSERT(itos(1) == "1", "itos(1) should be \"1\"");
    TEST_ASSERT(itos(9) == "9", "itos(9) should be \"9\"");
    TEST_ASSERT(itos(10) == "10", "itos(10) should be \"10\"");
    TEST_ASSERT(itos(42) == "42", "itos(42) should be \"42\"");
    TEST_ASSERT(itos(123) == "123", "itos(123) should be \"123\"");
    TEST_ASSERT(itos(1000) == "1000", "itos(1000) should be \"1000\"");
    TEST_ASSERT(itos(-1) == "-1", "itos(-1) should be \"-1\"");
    TEST_ASSERT(itos(-42) == "-42", "itos(-42) should be \"-42\"");
    TEST_ASSERT(itos(2147483647) == "2147483647", "itos(INT_MAX) mismatch");

    // uint64_t
    TEST_ASSERT(itos(uint64_t{0}) == "0", "itos(uint64 0) should be \"0\"");
    TEST_ASSERT(itos(uint64_t{999999999999ULL}) == "999999999999", "itos large uint64 mismatch");

    std::printf("\nitos test results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
