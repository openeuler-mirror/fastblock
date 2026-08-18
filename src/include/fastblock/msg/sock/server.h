/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include "fastblock/msg/sock/connection.h"
#include "fastblock/msg/sock/options.h"
#include "fastblock/utils/simple_poller.h"

#include <google/protobuf/service.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct spdk_cpuset;
struct spdk_sock;
struct spdk_sock_group;
struct spdk_thread;

namespace msg {
namespace sock {

class server {
public:
    using options = sock::options;

    server(::spdk_thread* thread, std::shared_ptr<options> opts);
    server(std::string thread_name, ::spdk_cpuset* cpumask, std::shared_ptr<options> opts);
    ~server() noexcept;

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    void create_listener(uint16_t port);
    void start();
    void stop(std::optional<std::function<void()>> cb = std::nullopt);
    void add_service(google::protobuf::Service* svc);

    uint16_t listen_port() const noexcept { return _opts ? _opts->port : 0; }
    std::string listen_address() const { return _opts ? _opts->bind_address : std::string{}; }
    probe& stats() noexcept { return _probe; }

    int poll();

    struct rpc_task {
        uint64_t correlation{0};
        connection* conn{nullptr};
        std::unique_ptr<google::protobuf::Message> request;
        std::unique_ptr<google::protobuf::Message> response;
        std::unique_ptr<rdma::rpc_controller> ctrlr;
        server* owner{nullptr};
        size_t reserved{0};
        bool done{false};
        std::shared_ptr<connection> hold;
    };

    static void on_method_done(rpc_task* task);

private:
    struct service_rec {
        google::protobuf::Service* svc{nullptr};
        std::unordered_map<std::string, const google::protobuf::MethodDescriptor*> methods;
    };

    void handle_start();
    void accept_one();
    void dispatch(connection* conn, assembled_rpc rpc);

    ::spdk_thread* _thread{nullptr};
    bool _owns_thread{false};
    std::shared_ptr<options> _opts;
    ::spdk_sock_group* _group{nullptr};
    ::spdk_sock* _listener{nullptr};
    utils::simple_poller _poller;
    probe _probe{};
    bool _started{false};
    bool _stopping{false};
    std::chrono::system_clock::time_point _last_scan{};
    std::vector<std::shared_ptr<connection>> _conns;
    std::unordered_map<std::string, service_rec> _services;
    std::optional<std::function<void()>> _on_stop;
};

} // namespace sock
} // namespace msg
