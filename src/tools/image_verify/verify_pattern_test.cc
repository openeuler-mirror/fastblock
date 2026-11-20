/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "verify_pattern.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace image_verify;

static int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

int main() {
  const uint64_t seed = 0x1122334455667788ull;
  const uint64_t off_a = 0;
  const uint64_t off_b = 4096;
  auto a = make_payload(seed, off_a, 4096);
  auto b = make_payload(seed, off_b, 4096);
  EXPECT_TRUE(a.size() == 4096);
  EXPECT_TRUE(b.size() == 4096);
  EXPECT_TRUE(a != b);
  EXPECT_TRUE(check_payload(a.data(), a.size(), seed, off_a) ==
              pattern_error::ok);
  EXPECT_TRUE(check_payload(b.data(), b.size(), seed, off_b) ==
              pattern_error::ok);
  EXPECT_TRUE(check_payload(a.data(), a.size(), seed, off_b) ==
              pattern_error::offset_mismatch);
  EXPECT_TRUE(check_payload(a.data(), a.size(), seed + 1, off_a) ==
              pattern_error::seed_mismatch);

  // Constant 0x55 (block_bench) must not pass as unique data.
  std::string bench(4096, static_cast<char>(0x55));
  EXPECT_TRUE(decode_payload(bench.data(), bench.size(), nullptr) ==
              pattern_error::bad_magic);

  // Two writes of the same offset/seed are identical (replay-safe).
  auto a2 = make_payload(seed, off_a, 4096);
  EXPECT_TRUE(a == a2);

  // Different seed at same offset is a different payload.
  auto c = make_payload(seed + 1, off_a, 4096);
  EXPECT_TRUE(a != c);

  // Flip one body byte.
  a[100] ^= 0x01;
  EXPECT_TRUE(check_payload(a.data(), a.size(), seed, off_a) ==
              pattern_error::body_mismatch);

  // Flip header CRC.
  auto d = make_payload(seed, off_a, 4096);
  d[30] ^= 0x01;
  EXPECT_TRUE(decode_payload(d.data(), d.size(), nullptr) ==
              pattern_error::bad_header_crc);

  if (g_failures != 0) {
    std::fprintf(stderr, "image_verify_pattern_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("image_verify_pattern_test: ok\n");
  return 0;
}
