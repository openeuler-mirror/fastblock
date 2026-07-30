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
struct ibv_cq;
struct ibv_mr;
struct ibv_pd;
struct rdma_cm_event;
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

    struct connection_context {
        uint32_t shard_id{0};
        rdma_cm_id* id{nullptr};
        ibv_pd* pd{nullptr};
        ibv_cq* cq{nullptr};
        bool established{false};
        /* Single posted recv buffer for raw header+body (MVP). */
        void* recv_buf{nullptr};
        size_t recv_buf_len{0};
        ibv_mr* recv_mr{nullptr};
        /* Response SEND buffer (header+body), registered once per conn. */
        void* send_buf{nullptr};
        size_t send_buf_len{0};
        ibv_mr* send_mr{nullptr};
        bool send_in_flight{false};
    };

    bool start_listener(uint32_t shard_id);
    void stop_listener(listener_context& listener) noexcept;
    void run_listener(uint32_t shard_id) noexcept;
    bool handle_connect_request(rdma_cm_id* id, uint32_t shard_id) noexcept;
    bool post_recv(connection_context* conn) noexcept;
    bool ensure_send_mr(connection_context* conn) noexcept;
    bool post_send(connection_context* conn, size_t length) noexcept;
    void handle_recv_complete(connection_context* conn, uint32_t byte_len) noexcept;
    void poll_cq(connection_context* conn) noexcept;
    void destroy_connection(connection_context* conn) noexcept;
    void close_all_connections() noexcept;

    osd_service* _service{nullptr};
    std::atomic<bool> _running{false};
    std::string _bind_address{};
    std::vector<std::unique_ptr<listener_context>> _listeners{};
    std::mutex _connections_mutex{};
    std::vector<std::unique_ptr<connection_context>> _connections{};
};
