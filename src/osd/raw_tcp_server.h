/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class osd_service;
struct osd_raw_tcp_connection_state;

class osd_raw_tcp_server {
public:
    explicit osd_raw_tcp_server(osd_service* service);
    ~osd_raw_tcp_server() noexcept;

    bool start(const std::string& bind_address, uint32_t shard_count);
    void stop() noexcept;

    uint16_t listen_port(uint32_t shard_id) const noexcept;

private:
    struct listener_context {
        int fd{-1};
        uint16_t port{0};
        std::thread worker{};
    };

    struct connection_context {
        int fd{-1};
        uint32_t shard_id{0};
        std::atomic<bool> done{false};
        std::string peer_address{};
        std::shared_ptr<osd_raw_tcp_connection_state> state{};
        std::thread worker{};
        std::thread writer{};
    };

    bool start_listener(uint32_t shard_id);
    void cleanup_finished_connections() noexcept;
    void log_connection_summary(uint32_t shard_id) noexcept;
    void run_listener(uint32_t shard_id) noexcept;
    void handle_connection(connection_context *conn, uint32_t shard_id) noexcept;
    void write_connection_responses(
      std::shared_ptr<osd_raw_tcp_connection_state> state,
      uint32_t shard_id) noexcept;

private:
    osd_service* _service{nullptr};
    std::atomic<bool> _running{false};
    std::string _bind_address{};
    std::vector<listener_context> _listeners{};
    std::mutex _connections_mutex{};
    std::vector<std::unique_ptr<connection_context>> _connections{};
};
