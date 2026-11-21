/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include "fastblock/msg/sock/connection.h"
#include "fastblock/msg/sock/options.h"
#include "fastblock/utils/simple_poller.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct spdk_sock_group;
struct spdk_thread;

namespace msg {
namespace sock {

class client : public std::enable_shared_from_this<client> {
public:
    using connection = sock::connection;
    using options = sock::options;

    client(std::string name, ::spdk_thread* thread, std::shared_ptr<options> opts);
    ~client() noexcept;

    client(const client&) = delete;
    client& operator=(const client&) = delete;

    void start(std::optional<std::function<void()>> on_start = std::nullopt);
    void stop(std::optional<std::function<void()>> on_stop = std::nullopt);

    void emplace_connection(std::string addr, uint16_t port,
                            std::function<void(bool, std::shared_ptr<connection>)> cb);

    void remove_connection(std::shared_ptr<connection> conn, std::function<void(bool)> cb);

    ::spdk_thread* get_thread() noexcept { return _thread; }
    bool is_start() const noexcept { return _started; }
    options* get_options() noexcept { return _opts.get(); }
    probe& stats() noexcept { return _probe; }

    int poll();

private:
    void handle_start();
    void handle_stop();
    void on_connected(::spdk_sock* s, uint64_t generation);
    void scan_connect_timeouts(std::chrono::system_clock::time_point now);
    void fail_pending_connect(uint64_t generation, bool cancel_cm);

    struct pending_connect {
        std::function<void(bool, std::shared_ptr<connection>)> cb;
        std::chrono::system_clock::time_point deadline{};
    };

    std::string _name;
    ::spdk_thread* _thread{nullptr};
    std::shared_ptr<options> _opts;
    ::spdk_sock_group* _group{nullptr};
    utils::simple_poller _poller;
    probe _probe{};
    bool _started{false};
    bool _stopping{false};
    uint64_t _generation{1};
    std::chrono::system_clock::time_point _last_scan{};
    std::vector<std::shared_ptr<connection>> _conns;
    std::unordered_map<uint64_t, pending_connect> _pending_connects;
    std::optional<std::function<void()>> _on_stop;
};

} // namespace sock
} // namespace msg
