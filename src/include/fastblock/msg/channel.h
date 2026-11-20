/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include "fastblock/msg/bind_error.h"

#include <spdk/env.h>
#include <spdk/thread.h>

#include <boost/property_tree/ptree.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <google/protobuf/service.h>

namespace msg {

enum class transport_kind : uint8_t { rdma = 1, sock = 2 };

class channel : public google::protobuf::RpcChannel {
public:
    ~channel() override = default;
    virtual bool is_terminated() noexcept = 0;
    virtual int async_shutdown() = 0;
};

class client {
public:
    virtual ~client() = default;
    virtual void start() = 0;
    virtual bool is_start() noexcept = 0;
    virtual void stop(std::optional<std::function<void()>> cb) = 0;
    virtual void emplace_connection(
      std::string addr,
      uint16_t port,
      std::function<void(bool, std::shared_ptr<channel>)> cb) = 0;
    virtual void remove_connection(
      std::shared_ptr<channel> conn,
      std::function<void(bool)> cb) = 0;
    virtual ::spdk_thread* get_thread() noexcept = 0;
};

class server {
public:
    virtual ~server() = default;
    virtual void create_listener(uint16_t port) = 0;
    virtual void start() = 0;
    virtual void add_service(google::protobuf::Service* svc) = 0;
    virtual uint16_t listen_port() noexcept = 0;
    virtual std::string listen_address() noexcept = 0;
    virtual void stop(std::optional<std::function<void()>> cb) = 0;
};

struct client_ctor {
    std::string name;
    ::spdk_thread* thread{nullptr};
    const boost::property_tree::ptree* conf{nullptr};
    int numa_sock_id{SPDK_ENV_SOCKET_ID_ANY};
};

struct server_ctor {
    std::string name;
    ::spdk_thread* thread{nullptr};
    const struct spdk_cpuset* cpumask{nullptr};
    const boost::property_tree::ptree* conf{nullptr};
    int numa_sock_id{SPDK_ENV_SOCKET_ID_ANY};
};

transport_kind parse_transport(const boost::property_tree::ptree& conf) noexcept;
// Probe NICs when msg_transport is missing / "auto". Writes the chosen key
// back into conf so every later parse_transport sees the same answer.
transport_kind resolve_transport(boost::property_tree::ptree& conf);
transport_kind active_transport() noexcept;
const char* transport_name(transport_kind k) noexcept;
// SIZE_MAX when RDMA (unlimited). 8MiB default on sock.
std::size_t ae_oob_budget() noexcept;
std::shared_ptr<client> make_client(transport_kind k, client_ctor c);
std::shared_ptr<server> make_server(transport_kind k, server_ctor c);

} // namespace msg
