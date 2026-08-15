/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/rdma/pool_quota.h"

#include <cstdio>
#include <list>

namespace {

int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

} // namespace

int main() {
  using msg::rdma::alloc_class;
  using msg::rdma::apply_default_pool_quota;
  using msg::rdma::can_alloc;
  using msg::rdma::freelist_put;
  using msg::rdma::freelist_take;
  using msg::rdma::pool_quota;
  using msg::rdma::update_bulk_gate;

  int a = 1, b = 2, c = 3, d = 4;
  std::list<int*> fl{&a, &b, &c, &d};

  auto none = freelist_take(fl, 5);
  EXPECT_TRUE(none == nullptr);
  EXPECT_TRUE(fl.size() == 4);

  auto two = freelist_take(fl, 2);
  EXPECT_TRUE(two != nullptr);
  EXPECT_TRUE(two[0] == &a);
  EXPECT_TRUE(two[1] == &b);
  EXPECT_TRUE(fl.size() == 2);

  auto rest = freelist_take(fl, 2);
  EXPECT_TRUE(rest != nullptr);
  EXPECT_TRUE(fl.empty());
  EXPECT_TRUE(freelist_take(fl, 1) == nullptr);

  freelist_put(fl, two.get(), 2);
  EXPECT_TRUE(fl.size() == 2);
  freelist_put(fl, rest.get(), 2);
  EXPECT_TRUE(fl.size() == 4);

  pool_quota q{};
  apply_default_pool_quota(q, 2048);
  EXPECT_TRUE(q.reserved_priority == 4);
  EXPECT_TRUE(q.reserved_per_connection == 2);
  EXPECT_TRUE(q.low_watermark > 0);
  EXPECT_TRUE(q.high_watermark >= q.low_watermark);
  q.n_connections = 3;
  EXPECT_TRUE(q.reserved_floor() == 4 + 2 * 3);

  bool bulk = true;
  uint64_t hits = 0;
  update_bulk_gate(q.low_watermark - 1, q, &bulk, &hits);
  EXPECT_TRUE(!bulk);
  EXPECT_TRUE(hits == 1);
  update_bulk_gate(q.low_watermark - 1, q, &bulk, &hits);
  EXPECT_TRUE(hits == 1);
  update_bulk_gate(q.high_watermark, q, &bulk, &hits);
  EXPECT_TRUE(bulk);

  q.n_connections = 2;
  const auto floor = q.reserved_floor();
  EXPECT_TRUE(can_alloc(floor, 1, alloc_class::priority, q, false));
  EXPECT_TRUE(can_alloc(floor + 1, 1, alloc_class::reserved, q, false));
  EXPECT_TRUE(!can_alloc(q.reserved_priority, 1, alloc_class::reserved, q, true));
  EXPECT_TRUE(!can_alloc(floor + 8, 1, alloc_class::bulk, q, false));
  EXPECT_TRUE(can_alloc(floor + 8, 8, alloc_class::bulk, q, true));
  EXPECT_TRUE(!can_alloc(floor + 7, 8, alloc_class::bulk, q, true));
  EXPECT_TRUE(!can_alloc(3, 4, alloc_class::priority, q, true));

  if (g_failures != 0) {
    std::fprintf(stderr, "%d rdma_memory_pool checks failed\n", g_failures);
    return 1;
  }
  return 0;
}
