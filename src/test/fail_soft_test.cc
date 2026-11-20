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
  using msg::rdma::classify_rpc_error;
  using msg::rdma::is_retryable;
  using msg::rdma::is_soft_rpc_error;
  using msg::rdma::rpc_error_class;
  using msg::rdma::status;
  using msg::rdma::string_status;
  using msg::rdma::tears_connection;

  EXPECT_TRUE(msg::rdma::default_rpc_timeout_us == 1000000);

  EXPECT_TRUE(std::string(string_status(status::resource_exhausted)) ==
              "resource_exhausted");
  EXPECT_TRUE(std::string(string_status(status::request_timeout)) ==
              "request_timeout");
  EXPECT_TRUE(std::string(string_status(status::terminating)) == "terminating");

  EXPECT_TRUE(is_retryable(rpc_error_class::resource_exhausted));
  EXPECT_TRUE(is_retryable(rpc_error_class::timeout));
  EXPECT_TRUE(is_retryable(rpc_error_class::request_timeout));
  EXPECT_TRUE(!is_retryable(rpc_error_class::too_large));
  EXPECT_TRUE(!is_retryable(rpc_error_class::application));
  EXPECT_TRUE(!is_retryable(rpc_error_class::connection));
  EXPECT_TRUE(!is_retryable(rpc_error_class::peer_terminating));

  EXPECT_TRUE(tears_connection(rpc_error_class::connection));
  EXPECT_TRUE(tears_connection(rpc_error_class::peer_terminating));
  EXPECT_TRUE(!tears_connection(rpc_error_class::resource_exhausted));
  EXPECT_TRUE(!tears_connection(rpc_error_class::timeout));
  EXPECT_TRUE(!tears_connection(rpc_error_class::too_large));
  EXPECT_TRUE(!tears_connection(rpc_error_class::application));

  EXPECT_TRUE(classify_rpc_error("resource_exhausted") ==
              rpc_error_class::resource_exhausted);
  EXPECT_TRUE(classify_rpc_error("timeout") == rpc_error_class::timeout);
  EXPECT_TRUE(classify_rpc_error("request_timeout") ==
              rpc_error_class::request_timeout);
  EXPECT_TRUE(classify_rpc_error("connection terminated") ==
              rpc_error_class::connection);
  EXPECT_TRUE(classify_rpc_error("too_large") == rpc_error_class::too_large);
  EXPECT_TRUE(classify_rpc_error("server terminating") ==
              rpc_error_class::peer_terminating);
  // Prefix / English prose is application, not a retryable token.
  EXPECT_TRUE(classify_rpc_error(
                "rpc call failed with reply status request_timeout") ==
              rpc_error_class::application);
  EXPECT_TRUE(!is_soft_rpc_error(
    "rpc call failed with reply status request_timeout"));
  EXPECT_TRUE(is_soft_rpc_error("resource_exhausted"));
  EXPECT_TRUE(is_soft_rpc_error("timeout"));
  EXPECT_TRUE(!is_soft_rpc_error("connection terminated"));

  if (g_failures != 0) {
    std::fprintf(stderr, "%d fail_soft checks failed\n", g_failures);
    return 1;
  }
  return 0;
}
