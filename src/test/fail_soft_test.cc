/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/rdma/types.h"

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
  using msg::rdma::is_soft_rpc_error;
  using msg::rdma::status;
  using msg::rdma::string_status;

  EXPECT_TRUE(msg::rdma::default_rpc_timeout_us == 1000000);

  EXPECT_TRUE(std::string(string_status(status::resource_exhausted)) ==
              "resource_exhausted");
  EXPECT_TRUE(std::string(string_status(status::request_timeout)) ==
              "request_timeout");
  EXPECT_TRUE(std::string(string_status(status::terminating)) == "terminating");

  EXPECT_TRUE(is_soft_rpc_error("resource_exhausted"));
  EXPECT_TRUE(is_soft_rpc_error("timeout"));
  EXPECT_TRUE(is_soft_rpc_error("request_timeout"));
  EXPECT_TRUE(is_soft_rpc_error("rpc call failed with reply status request_timeout"));
  EXPECT_TRUE(!is_soft_rpc_error("connection terminated"));
  EXPECT_TRUE(!is_soft_rpc_error("too_large"));
  EXPECT_TRUE(!is_soft_rpc_error("server terminating"));

  if (g_failures != 0) {
    std::fprintf(stderr, "%d fail_soft checks failed\n", g_failures);
    return 1;
  }
  return 0;
}
