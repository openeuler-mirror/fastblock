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
#include <string>
#include <thread>
#include <vector>

class osd_service;
struct rdma_cm_id;
struct rdma_event_channel;

/*
 * Raw protocol over RDMA for kfastblock (kernel client <-> OSD data plane).
 * Parallel to osd_raw_tcp_server; does not replace userspace protobuf RDMA RPC.
 */
class osd_raw_rdma_server {
public:
    explicit osd_raw_rdma_server(osd_service* service);
    ~osd_raw_rdma_server() noexcept;

    bool start(const std::string& bind_address, uint32_t shard_count);
    void stop() noexcept;

    uint16_t listen_port(uint32_t shard_id) const noexcept;

private:
    struct listener_context {
        uint16_t port{0};
        uint32_t shard_id{0};
        rdma_event_channel* channel{nullptr};
        rdma_cm_id* listen_id{nullptr};
        std::thread worker{};
        std::atomic<bool> stop{false};
    };

    bool start_listener(uint32_t shard_id);
    void stop_listener(listener_context& listener) noexcept;
    void run_listener(uint32_t shard_id) noexcept;

    osd_service* _service{nullptr};
    std::atomic<bool> _running{false};
    std::string _bind_address{};
    std::vector<std::unique_ptr<listener_context>> _listeners{};
};
