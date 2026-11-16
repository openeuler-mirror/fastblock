/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 *
 * Regression tests for RTC local-first predicates (PR-2) and related pure logic.
 * No SPDK runtime required.
 */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>

// Mirror pure predicate from rtc_local_first.h (avoid pulling SPDK headers).
namespace rtc_test {

inline bool is_on_home_spdk_thread_pred(
  const uint32_t this_shard,
  const uint32_t home_shard,
  const bool same_spdk_thread) noexcept {
    return this_shard == home_shard
        && this_shard != std::numeric_limits<uint32_t>::max()
        && same_spdk_thread;
}

} // namespace rtc_test

static int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

static void test_local_first_predicate() {
    // Home path: same shard + same SPDK thread → inline
    EXPECT_TRUE(rtc_test::is_on_home_spdk_thread_pred(1, 1, true));

    // Wrong shard → must hop
    EXPECT_FALSE(rtc_test::is_on_home_spdk_thread_pred(0, 1, true));

    // Same core/shard id but different SPDK thread (raw worker) → hop (K17)
    EXPECT_FALSE(rtc_test::is_on_home_spdk_thread_pred(1, 1, false));

    // Invalid shard
    EXPECT_FALSE(rtc_test::is_on_home_spdk_thread_pred(
      std::numeric_limits<uint32_t>::max(), 0, true));
}

int main() {
    test_local_first_predicate();

    if (g_failures != 0) {
        std::fprintf(stderr, "rtc_local_first_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("rtc_local_first_test: all passed\n");
    return 0;
}
