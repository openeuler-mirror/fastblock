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

#include "raw_rdma_stats.h"

#include <atomic>
#include <cstdint>
#include <deque>
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

    bool is_running() const noexcept;
    uint32_t shard_count() const noexcept;
    uint16_t listen_port(uint32_t shard_id) const noexcept;
    size_t connection_count() const noexcept;
    /* Connections currently established on a single shard listener. */
    size_t connection_count(uint32_t shard_id) const noexcept;
    /* Comma-separated listen ports for all shards (empty if stopped). */
    std::string ports_string() const;
    raw_rdma_server_stats collect_stats() const;
    /* Aggregate per-connection counters across all live connections. */
    void get_io_totals(uint64_t* recv_total,
                       uint64_t* send_total,
                       uint64_t* error_total) const noexcept;
    /* Snapshot of per-shard listen ports (0 if shard missing). */
    std::vector<uint16_t> listen_ports() const;
    size_t max_connection_limit() const noexcept;
    /* Bind address used by active listeners (empty when stopped). */
    const std::string& bind_address() const noexcept;
    /* Connections that completed RDMA_CM ESTABLISHED. */
    size_t established_connection_count() const noexcept;

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
        /* Multi-slot RECV staging for pipelined client requests. */
        static constexpr size_t max_recv_slots{4};
        struct recv_slot {
            void* buf{nullptr};
            size_t len{0};
            ibv_mr* mr{nullptr};
            bool posted{false};
        };
        recv_slot recv_slots[max_recv_slots]{};
        /* Legacy single-buffer aliases (slot 0) kept during migration. */
        void* recv_buf{nullptr};
        size_t recv_buf_len{0};
        ibv_mr* recv_mr{nullptr};
        /* Response SEND buffer (header+body), registered once per conn. */
        void* send_buf{nullptr};
        size_t send_buf_len{0};
        ibv_mr* send_mr{nullptr};
        bool send_in_flight{false};
        /* Serialized responses waiting for SEND slot (async object I/O). */
        std::mutex send_mu{};
        std::deque<std::vector<uint8_t>> send_queue{};
        static constexpr size_t max_send_queue{64};
        /* Per-connection counters for diagnostics. */
        std::atomic<uint64_t> recv_count{0};
        std::atomic<uint64_t> send_count{0};
        std::atomic<uint64_t> error_count{0};
        std::string peer_address{};
    };

    bool start_listener(uint32_t shard_id);
    void stop_listener(listener_context& listener) noexcept;
    void run_listener(uint32_t shard_id) noexcept;
    bool handle_connect_request(rdma_cm_id* id, uint32_t shard_id) noexcept;
    bool post_recv(connection_context* conn) noexcept;
    bool post_recv_slot(connection_context* conn, size_t slot) noexcept;
    bool ensure_recv_slots(connection_context* conn) noexcept;
    void free_recv_slots(connection_context* conn) noexcept;
    bool ensure_send_mr(connection_context* conn) noexcept;
    bool post_send(connection_context* conn, size_t length) noexcept;
    bool send_response(connection_context* conn,
                       const void* req_hdr,
                       uint32_t status,
                       const void* body,
                       uint32_t body_len) noexcept;
    bool enqueue_response_frame(connection_context* conn,
                                std::vector<uint8_t> frame) noexcept;
    void try_flush_send_queue(connection_context* conn) noexcept;
    /* Drain outstanding SEND WRs with a bounded wait before destroy. */
    void drain_send_queue(connection_context* conn,
                          int timeout_ms) noexcept;
    void dispatch_get_leader(connection_context* conn,
                             const void* req_hdr,
                             const uint8_t* body,
                             uint32_t body_len) noexcept;
    void dispatch_read(connection_context* conn,
                       const void* req_hdr,
                       const uint8_t* body,
                       uint32_t body_len) noexcept;
    void dispatch_write(connection_context* conn,
                        const void* req_hdr,
                        const uint8_t* body,
                        uint32_t body_len) noexcept;
    void dispatch_delete(connection_context* conn,
                         const void* req_hdr,
                         const uint8_t* body,
                         uint32_t body_len) noexcept;
    void handle_recv_complete(connection_context* conn,
                              uint32_t byte_len,
                              size_t slot) noexcept;
    void poll_cq(connection_context* conn) noexcept;
    void destroy_connection(connection_context* conn) noexcept;
    void close_all_connections() noexcept;
    std::shared_ptr<connection_context>
    retain_connection(connection_context* conn) noexcept;

    osd_service* _service{nullptr};
    std::atomic<bool> _running{false};
    std::string _bind_address{};
    std::vector<std::unique_ptr<listener_context>> _listeners{};
    mutable std::mutex _connections_mutex{};
    std::vector<std::shared_ptr<connection_context>> _connections{};
    static constexpr size_t max_connections{256};
    /* Global lifetime counters; survive connection teardown. */
    std::atomic<uint64_t> _accept_total{0};
    std::atomic<uint64_t> _reject_total{0};
    std::atomic<uint64_t> _dispatch_error_total{0};
};
