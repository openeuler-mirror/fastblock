/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/rdma/oob_payload.h"

#include <cstdio>
#include <string>

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
  using msg::rdma::attach_rpc_payload;
  using msg::rdma::peel_rpc_payload;

  {
    osd::write_request w;
    w.set_pool_id(1);
    w.set_pg_id(2);
    w.set_object_name("obj");
    w.set_offset(4096);
    w.set_data(std::string(4096, 'A'));
    const auto before = w.ByteSizeLong();
    std::string oob;
    peel_rpc_payload(w, &oob);
    EXPECT_TRUE(oob.size() == 4096);
    EXPECT_TRUE(w.data().empty());
    EXPECT_TRUE(w.data_len() == 4096);
    EXPECT_TRUE(w.ByteSizeLong() < before);
    EXPECT_TRUE(w.object_name() == "obj");
    EXPECT_TRUE(attach_rpc_payload(w, oob));
    EXPECT_TRUE(w.data().size() == 4096);
    EXPECT_TRUE(w.data()[0] == 'A');
  }

  {
    msg_appendentries_t ae;
    ae.set_node_id(1);
    auto* e0 = ae.add_entries();
    e0->set_term(1);
    e0->set_idx(10);
    e0->set_type(0);
    e0->set_meta("meta0");
    e0->set_data(std::string(4096, 'B'));
    auto* e1 = ae.add_entries();
    e1->set_term(1);
    e1->set_idx(11);
    e1->set_data(std::string(4096, 'C'));
    const auto before = ae.ByteSizeLong();
    std::string oob;
    peel_rpc_payload(ae, &oob);
    EXPECT_TRUE(oob.size() == 8192);
    EXPECT_TRUE(ae.entries(0).data().empty());
    EXPECT_TRUE(ae.entries(1).data().empty());
    EXPECT_TRUE(ae.ByteSizeLong() < before);
    EXPECT_TRUE(attach_rpc_payload(ae, oob));
    EXPECT_TRUE(ae.entries(0).data() == std::string(4096, 'B'));
    EXPECT_TRUE(ae.entries(1).data() == std::string(4096, 'C'));
  }

  {
    osd::write_request empty;
    empty.set_object_name("x");
    std::string oob;
    peel_rpc_payload(empty, &oob);
    EXPECT_TRUE(oob.empty());
    EXPECT_TRUE(attach_rpc_payload(empty, oob));
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "oob_payload_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("oob_payload_test: ok\n");
  return 0;
}
