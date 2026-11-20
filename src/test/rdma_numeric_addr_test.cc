/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/rdma/numeric_addr.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>

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
  using msg::rdma::try_numeric_sockaddr;

  ::sockaddr_storage ss{};
  EXPECT_TRUE(try_numeric_sockaddr("10.211.55.27", 1234, ss));
  EXPECT_TRUE(ss.ss_family == AF_INET);
  auto* in4 = reinterpret_cast<::sockaddr_in*>(&ss);
  EXPECT_TRUE(ntohs(in4->sin_port) == 1234);
  char v4[INET_ADDRSTRLEN] = {};
  EXPECT_TRUE(::inet_ntop(AF_INET, &in4->sin_addr, v4, sizeof(v4)) != nullptr);
  EXPECT_TRUE(std::strcmp(v4, "10.211.55.27") == 0);

  EXPECT_TRUE(try_numeric_sockaddr("::1", 80, ss));
  EXPECT_TRUE(ss.ss_family == AF_INET6);
  auto* in6 = reinterpret_cast<::sockaddr_in6*>(&ss);
  EXPECT_TRUE(ntohs(in6->sin6_port) == 80);

  EXPECT_TRUE(try_numeric_sockaddr("[2001:db8::1]", 443, ss));
  EXPECT_TRUE(ss.ss_family == AF_INET6);
  in6 = reinterpret_cast<::sockaddr_in6*>(&ss);
  EXPECT_TRUE(ntohs(in6->sin6_port) == 443);

  EXPECT_TRUE(!try_numeric_sockaddr("osd1.local", 1, ss));
  EXPECT_TRUE(!try_numeric_sockaddr("localhost", 1, ss));
  EXPECT_TRUE(!try_numeric_sockaddr("", 1, ss));
  EXPECT_TRUE(!try_numeric_sockaddr("not an ip", 22, ss));

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
