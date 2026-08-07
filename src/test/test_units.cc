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

#include "utils/units.h"

#include <cstdio>

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
    TEST_ASSERT(B == 1, "B should be 1");
    TEST_ASSERT(KB == 1024, "KB should be 1024");
    TEST_ASSERT(MB == 1024 * 1024, "MB should be 1048576");
    TEST_ASSERT(GB == 1024 * 1024 * 1024, "GB should be 1073741824");

    TEST_ASSERT(KB == 1024 * B, "KB == 1024 * B");
    TEST_ASSERT(MB == 1024 * KB, "MB == 1024 * KB");
    TEST_ASSERT(GB == 1024 * MB, "GB == 1024 * MB");

    // User-defined literal operators
    TEST_ASSERT(1_KB == 1024, "1_KB should be 1024");
    TEST_ASSERT(4_KB == 4096, "4_KB should be 4096");
    TEST_ASSERT(1_MB == 1048576, "1_MB should be 1048576");
    TEST_ASSERT(1_GB == 1073741824, "1_GB should be 1073741824");
    TEST_ASSERT(2_GB == 2ULL * 1024 * 1024 * 1024, "2_GB mismatch");

    // Common use case: 4K block size
    TEST_ASSERT(4_KB == 4096, "4KB block size should be 4096");

    std::printf("\nunits test results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
