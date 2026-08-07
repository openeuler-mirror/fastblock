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

#include "utils/md5.h"

#include <cstdio>
#include <cstring>
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

static std::string md5_hex(const char *data, size_t len) {
    std::string raw = utils::md5(const_cast<char *>(data), len);
    static const char hex_chars[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        hex += hex_chars[c >> 4];
        hex += hex_chars[c & 0x0f];
    }
    return hex;
}

int main() {
    // MD5("") = d41d8cd98f00b204e9800998ecf8427e
    TEST_ASSERT(md5_hex("", 0) == "d41d8cd98f00b204e9800998ecf8427e",
                "MD5 of empty string mismatch");

    // MD5("abc") = 900150983cd24fb0d6963f7d28e17f72
    TEST_ASSERT(md5_hex("abc", 3) == "900150983cd24fb0d6963f7d28e17f72",
                "MD5 of 'abc' mismatch");

    // MD5("hello") = 5d41402abc4b2a76b9719d911017c592
    TEST_ASSERT(md5_hex("hello", 5) == "5d41402abc4b2a76b9719d911017c592",
                "MD5 of 'hello' mismatch");

    // Length check: MD5 always produces 16 bytes
    std::string raw = utils::md5(const_cast<char *>("test"), 4);
    TEST_ASSERT(raw.size() == 16, "MD5 raw output should be 16 bytes");

    std::printf("\nmd5 test results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
