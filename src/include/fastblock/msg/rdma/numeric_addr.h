/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <cstdint>
#include <string>
#include <string_view>

namespace msg {
namespace rdma {

// Fill sockaddr from a numeric IPv4/IPv6 literal (optional [brackets] on v6).
// Returns false for hostnames so the caller can fall back to rdma_getaddrinfo.
inline bool try_numeric_sockaddr(
  std::string_view addr, uint16_t port, ::sockaddr_storage& ss) noexcept {
    std::memset(&ss, 0, sizeof(ss));
    std::string host{addr};
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }

    auto* in4 = reinterpret_cast<::sockaddr_in*>(&ss);
    if (::inet_pton(AF_INET, host.c_str(), &in4->sin_addr) == 1) {
        in4->sin_family = AF_INET;
        in4->sin_port = htons(port);
        return true;
    }

    std::memset(&ss, 0, sizeof(ss));
    auto* in6 = reinterpret_cast<::sockaddr_in6*>(&ss);
    if (::inet_pton(AF_INET6, host.c_str(), &in6->sin6_addr) == 1) {
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        return true;
    }
    return false;
}

} // namespace rdma
} // namespace msg
