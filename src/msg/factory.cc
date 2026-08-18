/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/channel.h"

#include "fastblock/msg/probe_rnic.h"
#include "fastblock/msg/sock/client.h"
#include "fastblock/msg/sock/server.h"
#include "fastblock/msg/sock/types.h"

#ifdef FASTBLOCK_WITH_RDMA
#include "fastblock/msg/rdma/client.h"
#include "fastblock/msg/rdma/server.h"
#endif

#include <spdk/log.h>

#include <atomic>
#include <limits>
#include <stdexcept>

namespace msg {
namespace {

#ifdef FASTBLOCK_WITH_RDMA
class rdma_client_iface : public client {
public:
    explicit rdma_client_iface(std::shared_ptr<rdma::client> inner) : _inner(std::move(inner)) {}

    void start() override { _inner->start(); }
    bool is_start() noexcept override { return _inner->is_start(); }
    void stop(std::optional<std::function<void()>> cb) override { _inner->stop(std::move(cb)); }
    void emplace_connection(
      std::string addr,
      uint16_t port,
      std::function<void(bool, std::shared_ptr<channel>)> cb) override {
        _inner->emplace_connection(
          std::move(addr),
          port,
          [cb = std::move(cb)](bool ok, std::shared_ptr<rdma::client::connection> conn) {
              cb(ok, std::move(conn));
          });
    }
    void remove_connection(std::shared_ptr<channel> conn, std::function<void(bool)> cb) override {
        _inner->remove_connection(
          std::dynamic_pointer_cast<rdma::client::connection>(conn), std::move(cb));
    }
    ::spdk_thread* get_thread() noexcept override { return _inner->get_thread(); }

private:
    std::shared_ptr<rdma::client> _inner;
};

class rdma_server_iface : public server {
public:
    explicit rdma_server_iface(std::shared_ptr<rdma::server> inner) : _inner(std::move(inner)) {}

    void create_listener(uint16_t port) override { _inner->create_listener(port); }
    void start() override { _inner->start(); }
    void add_service(google::protobuf::Service* svc) override { _inner->add_service(svc); }
    uint16_t listen_port() noexcept override { return _inner->listen_port(); }
    std::string listen_address() noexcept override { return _inner->listen_address(); }
    void stop(std::optional<std::function<void()>> cb) override { _inner->stop(std::move(cb)); }

private:
    std::shared_ptr<rdma::server> _inner;
};
#endif

class sock_client_iface : public client {
public:
    explicit sock_client_iface(std::shared_ptr<sock::client> inner) : _inner(std::move(inner)) {}

    void start() override { _inner->start(); }
    bool is_start() noexcept override { return _inner->is_start(); }
    void stop(std::optional<std::function<void()>> cb) override { _inner->stop(std::move(cb)); }
    void emplace_connection(
      std::string addr,
      uint16_t port,
      std::function<void(bool, std::shared_ptr<channel>)> cb) override {
        _inner->emplace_connection(
          std::move(addr),
          port,
          [cb = std::move(cb)](bool ok, std::shared_ptr<sock::connection> conn) {
              cb(ok, std::move(conn));
          });
    }
    void remove_connection(std::shared_ptr<channel> conn, std::function<void(bool)> cb) override {
        _inner->remove_connection(
          std::dynamic_pointer_cast<sock::connection>(conn), std::move(cb));
    }
    ::spdk_thread* get_thread() noexcept override { return _inner->get_thread(); }

private:
    std::shared_ptr<sock::client> _inner;
};

class sock_server_iface : public server {
public:
    explicit sock_server_iface(std::shared_ptr<sock::server> inner) : _inner(std::move(inner)) {}

    void create_listener(uint16_t port) override { _inner->create_listener(port); }
    void start() override { _inner->start(); }
    void add_service(google::protobuf::Service* svc) override { _inner->add_service(svc); }
    uint16_t listen_port() noexcept override { return _inner->listen_port(); }
    std::string listen_address() noexcept override { return _inner->listen_address(); }
    void stop(std::optional<std::function<void()>> cb) override { _inner->stop(std::move(cb)); }

private:
    std::shared_ptr<sock::server> _inner;
};

boost::property_tree::ptree& mutable_conf(const client_ctor& c) {
    if (!c.conf) {
        throw std::runtime_error("make_client: null conf");
    }
    return const_cast<boost::property_tree::ptree&>(*c.conf);
}

boost::property_tree::ptree& mutable_conf(const server_ctor& c) {
    if (!c.conf) {
        throw std::runtime_error("make_server: null conf");
    }
    return const_cast<boost::property_tree::ptree&>(*c.conf);
}

#ifdef FASTBLOCK_WITH_RDMA
constexpr transport_kind k_compiled_default = transport_kind::rdma;
#else
constexpr transport_kind k_compiled_default = transport_kind::sock;
#endif

std::atomic<transport_kind> g_active{k_compiled_default};
std::atomic<size_t> g_ae_budget{std::numeric_limits<size_t>::max()};

std::string read_transport_key(const boost::property_tree::ptree& conf) {
    const auto it = conf.find("msg_transport");
    if (it == conf.not_found()) {
        return {};
    }
    try {
        return it->second.get_value<std::string>();
    } catch (...) {
        return {};
    }
}

size_t read_ae_budget(const boost::property_tree::ptree& conf) {
    const auto it = conf.find("msg_sock_ae_max_oob_bytes");
    if (it == conf.not_found()) {
        return sock::default_ae_max_oob_bytes;
    }
    try {
        const auto v = it->second.get_value<uint32_t>();
        return v == 0 ? sock::default_ae_max_oob_bytes : v;
    } catch (...) {
        return sock::default_ae_max_oob_bytes;
    }
}

void remember(transport_kind k, const boost::property_tree::ptree& conf) {
    g_active.store(k, std::memory_order_release);
    if (k == transport_kind::sock) {
        g_ae_budget.store(read_ae_budget(conf), std::memory_order_release);
    } else {
        g_ae_budget.store(std::numeric_limits<size_t>::max(), std::memory_order_release);
    }
}

transport_kind choose_auto(const boost::property_tree::ptree& conf) {
#ifdef FASTBLOCK_WITH_RDMA
    const auto probe = probe_rnic(conf, env_allow_rxe());
    if (probe.usable) {
        SPDK_NOTICELOG(
          "msg_transport auto -> rdma (%s %s)\n",
          probe.device.c_str(),
          probe.detail.c_str());
        return transport_kind::rdma;
    }
    SPDK_NOTICELOG("msg_transport auto -> sock (%s)\n", probe.detail.c_str());
    return transport_kind::sock;
#else
    (void)conf;
    return transport_kind::sock;
#endif
}

} // namespace

const char* transport_name(transport_kind k) noexcept {
    return k == transport_kind::sock ? "sock" : "rdma";
}

transport_kind active_transport() noexcept {
    return g_active.load(std::memory_order_acquire);
}

std::size_t ae_oob_budget() noexcept {
    return g_ae_budget.load(std::memory_order_acquire);
}

transport_kind parse_transport(const boost::property_tree::ptree& conf) noexcept {
    const auto v = read_transport_key(conf);
    if (v == "sock") {
        remember(transport_kind::sock, conf);
        return transport_kind::sock;
    }
    if (v == "rdma") {
#ifdef FASTBLOCK_WITH_RDMA
        remember(transport_kind::rdma, conf);
        return transport_kind::rdma;
#else
        SPDK_ERRLOG("msg_transport=rdma but RDMA was not compiled in, using sock\n");
        remember(transport_kind::sock, conf);
        return transport_kind::sock;
#endif
    }
    if (!v.empty() && v != "auto") {
        SPDK_ERRLOG("unknown msg_transport '%s', auto-selecting\n", v.c_str());
    }
    const auto k = choose_auto(conf);
    remember(k, conf);
    return k;
}

transport_kind resolve_transport(boost::property_tree::ptree& conf) {
    const auto raw = read_transport_key(conf);
#ifdef FASTBLOCK_WITH_RDMA
    if (raw == "rdma") {
        const auto probe = probe_rnic(conf, true);
        if (!probe.usable) {
            throw std::runtime_error(
              std::string("msg_transport=rdma but no active RNIC: ") + probe.detail);
        }
        SPDK_NOTICELOG(
          "msg_transport=rdma (%s %s)\n", probe.device.c_str(), probe.detail.c_str());
    }
#else
    if (raw == "rdma") {
        throw std::runtime_error("msg_transport=rdma but FASTBLOCK_WITH_RDMA=OFF");
    }
#endif
    const auto k = parse_transport(conf);
    conf.put("msg_transport", transport_name(k));
    if (raw.empty() || raw == "auto") {
        SPDK_NOTICELOG("resolved msg_transport=%s\n", transport_name(k));
    } else if (raw == "sock") {
        SPDK_NOTICELOG("msg_transport=sock (explicit)\n");
    }
    return k;
}

std::shared_ptr<client> make_client(transport_kind k, client_ctor c) {
    if (!c.thread) {
        throw std::runtime_error("make_client: thread is required");
    }
    if (k == transport_kind::sock) {
        auto opts = sock::options::make(mutable_conf(c));
        auto inner = std::make_shared<sock::client>(std::move(c.name), c.thread, std::move(opts));
        return std::make_shared<sock_client_iface>(std::move(inner));
    }
#ifdef FASTBLOCK_WITH_RDMA
    auto opts = rdma::client::make_options(mutable_conf(c));
    auto inner =
      std::make_shared<rdma::client>(std::move(c.name), c.thread, std::move(opts), c.numa_sock_id);
    return std::make_shared<rdma_client_iface>(std::move(inner));
#else
    throw std::runtime_error("make_client: RDMA was not compiled in");
#endif
}

std::shared_ptr<server> make_server(transport_kind k, server_ctor c) {
    if (k == transport_kind::sock) {
        auto opts = sock::options::make(mutable_conf(c));
        std::shared_ptr<sock::server> inner;
        if (c.thread) {
            inner = std::make_shared<sock::server>(c.thread, std::move(opts));
        } else {
            inner = std::make_shared<sock::server>(
              std::move(c.name), const_cast<::spdk_cpuset*>(c.cpumask), std::move(opts));
        }
        return std::make_shared<sock_server_iface>(std::move(inner));
    }
#ifdef FASTBLOCK_WITH_RDMA
    auto opts = rdma::server::make_options(mutable_conf(c));
    std::shared_ptr<rdma::server> inner;
    if (c.thread) {
        inner = std::make_shared<rdma::server>(c.thread, std::move(opts), c.numa_sock_id);
    } else {
        inner = std::make_shared<rdma::server>(
          std::move(c.name),
          const_cast<::spdk_cpuset*>(c.cpumask),
          std::move(opts),
          c.numa_sock_id);
    }
    return std::make_shared<rdma_server_iface>(std::move(inner));
#else
    throw std::runtime_error("make_server: RDMA was not compiled in");
#endif
}

} // namespace msg
