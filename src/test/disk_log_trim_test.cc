/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */

/**
 * In-process stand-in for the cluster snapshot trigger:
 * after leader apply + trim, a lagged follower's next_idx is below
 * log_get_base_index() (do_recovery then takes _recovery_by_snapshot).
 */

#include "localstore/aos/log_store.h"
#include "localstore/aos/mem_device_backend.h"
#include "localstore/aos/osd_store.h"
#include "localstore/disk_log.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define EXPECT_EQ(a, b)                                                        \
  do {                                                                         \
    const auto _a = (a);                                                       \
    const auto _b = (b);                                                       \
    if (!(_a == _b)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: got %llu vs %llu\n", __FILE__,         \
                   __LINE__, static_cast<unsigned long long>(_a),              \
                   static_cast<unsigned long long>(_b));                       \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

using namespace fastblock::aos;

static void on_done(void* a, int err) { *static_cast<int*>(a) = err; }

int main() {
  setenv("FASTBLOCK_RAFT_LOG_TRIM_TRIGGER_ENTRIES", "128", 1);
  setenv("FASTBLOCK_RAFT_LOG_TRIM_KEEP_ENTRIES", "64", 1);
  setenv("FASTBLOCK_RAFT_RECOVERY_TEST_LOGS", "1", 1);

  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  int err = -1;
  osd.format(on_done, &err);
  EXPECT_EQ(err, 0);
  err = -1;
  osd.create_pg(1, 0, on_done, &err);
  EXPECT_EQ(err, 0);

  log_store* lg = osd.get_or_create_log(1, 0, osd.serving_epoch(1, 0));
  EXPECT_TRUE(lg != nullptr);
  disk_log dlog(lg);

  for (uint64_t i = 1; i <= 200; ++i) {
    log_entry e;
    e.term = 1;
    e.index = i;
    e.type = 0;
    e.meta = "m";
    e.data = "x";
    err = -1;
    lg->append(e, on_done, &err);
    EXPECT_EQ(err, 0);
  }
  EXPECT_EQ(dlog.get_lowest_index(), 1u);

  dlog.advance_trim_index(100);
  EXPECT_EQ(dlog.get_lowest_index(), 1u);  // below trigger

  dlog.advance_trim_index(200);
  // cut = 200 - 64 = 136; trim_to drops <=136; base becomes 137
  EXPECT_EQ(dlog.get_lowest_index(), 137u);
  EXPECT_TRUE(lg->contains(137));
  EXPECT_TRUE(!lg->contains(136));

  const uint64_t follower_next_idx = 1;
  EXPECT_TRUE(follower_next_idx < dlog.get_lowest_index());

  if (g_failures != 0) {
    std::fprintf(stderr, "disk_log_trim_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("disk_log_trim_test: ok (next_idx=1 < base=%lu)\n",
              static_cast<unsigned long>(dlog.get_lowest_index()));
  return 0;
}
