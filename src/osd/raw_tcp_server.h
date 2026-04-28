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
#include <string>
#include <thread>
#include <vector>

class osd_service;

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

    bool start_listener(uint32_t shard_id);
    void run_listener(uint32_t shard_id) noexcept;
    void handle_connection(int client_fd, uint32_t shard_id) noexcept;

private:
    osd_service* _service{nullptr};
    std::atomic<bool> _running{false};
    std::string _bind_address{};
    std::vector<listener_context> _listeners{};
};
