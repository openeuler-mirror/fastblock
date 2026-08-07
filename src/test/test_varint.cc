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
 * Unit tests for varint and fixed integer encoding/decoding utilities.
 * Build: requires project build environment with SPDK headers.
 */

#include "utils/varint.h"

#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <limits>

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

static void test_encode_decode_varint32() {
    char buf[10];
    uint32_t values[] = {0, 1, 127, 128, 255, 256, 16383, 16384,
                         2097151, 268435455, std::numeric_limits<uint32_t>::max()};

    for (uint32_t v : values) {
        std::memset(buf, 0, sizeof(buf));
        size_t enc_len = encode_varint32(buf, v);
        TEST_ASSERT(enc_len > 0, "varint32 encode length should be > 0");
        TEST_ASSERT(enc_len <= 5, "varint32 encode length should be <= 5");

        auto [decoded, dec_len] = decode_varint32(buf, sizeof(buf));
        TEST_ASSERT(decoded == v, "varint32 roundtrip mismatch");
        TEST_ASSERT(dec_len == enc_len, "varint32 decode length mismatch");
    }
}

static void test_encode_decode_varint64() {
    char buf[16];
    uint64_t values[] = {0, 1, 127, 128, 16384, 2097152,
                         268435456ULL, 34359738368ULL,
                         std::numeric_limits<uint64_t>::max()};

    for (uint64_t v : values) {
        std::memset(buf, 0, sizeof(buf));
        size_t enc_len = encode_varint64(buf, v);
        TEST_ASSERT(enc_len > 0, "varint64 encode length should be > 0");
        TEST_ASSERT(enc_len <= 10, "varint64 encode length should be <= 10");

        auto [decoded, dec_len] = decode_varint64(buf, sizeof(buf));
        TEST_ASSERT(decoded == v, "varint64 roundtrip mismatch");
        TEST_ASSERT(dec_len == enc_len, "varint64 decode length mismatch");
    }
}

static void test_encode_decode_fixed32() {
    char buf[4];
    uint32_t values[] = {0, 1, 255, 65535, 16777215, std::numeric_limits<uint32_t>::max()};

    for (uint32_t v : values) {
        std::memset(buf, 0, sizeof(buf));
        encode_fixed32(buf, v);
        uint32_t decoded = decode_fixed32(buf);
        TEST_ASSERT(decoded == v, "fixed32 roundtrip mismatch");
    }
}

static void test_encode_decode_fixed64() {
    char buf[8];
    uint64_t values[] = {0, 1, 255, 65535, 4294967295ULL,
                         std::numeric_limits<uint64_t>::max()};

    for (uint64_t v : values) {
        std::memset(buf, 0, sizeof(buf));
        encode_fixed64(buf, v);
        uint64_t decoded = decode_fixed64(buf);
        TEST_ASSERT(decoded == v, "fixed64 roundtrip mismatch");
    }
}

static void test_fixed64_split() {
    char buf1[4], buf2[4];
    uint64_t v = 0x0123456789ABCDEFULL;

    encode_fixed64(buf1, 4, buf2, v);

    char full[8];
    std::memcpy(full, buf1, 4);
    std::memcpy(full + 4, buf2, 4);
    uint64_t decoded = decode_fixed64(full);
    TEST_ASSERT(decoded == v, "fixed64 split encode/decode mismatch");

    uint64_t decoded_split = decode_fixed64(buf1, 4, buf2);
    TEST_ASSERT(decoded_split == v, "fixed64 split decode mismatch");
}

int main() {
    test_encode_decode_varint32();
    test_encode_decode_varint64();
    test_encode_decode_fixed32();
    test_encode_decode_fixed64();
    test_fixed64_split();

    std::printf("\nvarint test results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
