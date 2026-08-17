/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#define FB_OP_TYPE_LOCK_DEBUG(...) do {} while (0)
#include "osd/op_type_excl_lock.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

using lock_t = op_type_excl_lock<utils::operation_type>;

// Models osd_service_complete<read_reply>::finish on -ENOMEM: unlock
// immediately from complete(). The old lock woke this waiter again
// before pop_front (UAF + stack overflow).
struct fail_then_unlock : public utils::context {
  lock_t* lock;
  int* done_count;
  fail_then_unlock(lock_t* l, int* c) : lock(l), done_count(c) {}
  void finish(int) override {
    ++*done_count;
    lock->unlock(utils::operation_type::READ);
  }
};

struct hold_lock : public utils::context {
  int* done_count;
  explicit hold_lock(int* c) : done_count(c) {}
  void finish(int) override { ++*done_count; }
};

void test_sync_fail_drains_waiters() {
  lock_t lock;
  int writers = 0;
  int failed = 0;
  constexpr int kWaiters = 4096;

  // WRITE holds the object (nvmf mkfs write). Queued READs wait because
  // the types are exclusive. Unlocking the write wakes them; each read
  // fails immediately (-ENOMEM / not-leader) and unlocks again.
  lock.lock(utils::operation_type::WRITE, new hold_lock(&writers));
  EXPECT_TRUE(writers == 1);

  for (int i = 0; i < kWaiters; ++i) {
    lock.lock(utils::operation_type::READ, new fail_then_unlock(&lock, &failed));
  }
  EXPECT_TRUE(failed == 0);
  EXPECT_TRUE(lock.holders() == static_cast<uint64_t>(1 + kWaiters));

  lock.unlock(utils::operation_type::WRITE);
  EXPECT_TRUE(failed == kWaiters);
  EXPECT_TRUE(lock.holders() == 0);
}

void test_incompatible_waiter_stays() {
  lock_t lock;
  int reads = 0;
  int writes = 0;
  lock.lock(utils::operation_type::READ, new hold_lock(&reads));
  lock.lock(utils::operation_type::WRITE, new hold_lock(&writes));
  EXPECT_TRUE(reads == 1);
  EXPECT_TRUE(writes == 0);
  EXPECT_TRUE(lock.holders() == 2);
  lock.unlock(utils::operation_type::READ);
  EXPECT_TRUE(writes == 1);
  EXPECT_TRUE(lock.holders() == 1);
  lock.unlock(utils::operation_type::WRITE);
  EXPECT_TRUE(lock.holders() == 0);
}

void test_compatible_holders_share() {
  lock_t lock;
  int a = 0;
  int b = 0;
  auto* r1 = new hold_lock(&a);
  auto* r2 = new hold_lock(&b);
  lock.lock(utils::operation_type::READ, r1);
  lock.lock(utils::operation_type::READ, r2);
  EXPECT_TRUE(a == 1);
  EXPECT_TRUE(b == 1);
  EXPECT_TRUE(lock.holders() == 2);
  lock.unlock(utils::operation_type::READ);
  EXPECT_TRUE(lock.holders() == 1);
  lock.unlock(utils::operation_type::READ);
  EXPECT_TRUE(lock.holders() == 0);
}

} // namespace

int main() {
  test_sync_fail_drains_waiters();
  test_incompatible_waiter_stays();
  test_compatible_holders_share();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("op_type_excl_lock_test OK\n");
  return 0;
}
