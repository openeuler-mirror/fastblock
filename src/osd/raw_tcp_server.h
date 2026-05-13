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
struct osd_raw_tcp_inflight_response;
struct raw_header;
struct sockaddr_in;
struct spdk_sock;
struct spdk_sock_group;

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
        int wake_read_fd{-1};
        int wake_write_fd{-1};
        spdk_sock* spdk_listener{nullptr};
        spdk_sock_group* spdk_group{nullptr};
        bool use_spdk_backend{false};
        uint16_t port{0};
        std::thread worker{};
    };

    struct connection_context {
        int fd{-1};
        spdk_sock* spdk_socket{nullptr};
        uint32_t shard_id{0};
        std::atomic<bool> done{false};
        std::string peer_address{};
        std::vector<uint8_t> recv_frame{};
        size_t recv_header_bytes{0};
        size_t recv_body_bytes{0};
        size_t recv_target_body_bytes{0};
        size_t send_bytes{0};
        bool send_reordered{false};
        std::vector<uint8_t> send_frame{};
        std::shared_ptr<osd_raw_tcp_inflight_response> sending{};
        std::shared_ptr<osd_raw_tcp_connection_state> state{};
    };

    bool start_listener(uint32_t shard_id);
    bool start_connection(int client_fd,
                          const sockaddr_in& peer_addr,
                          uint32_t shard_id) noexcept;
    bool start_spdk_connection(spdk_sock *sock,
                               uint32_t shard_id) noexcept;
    static void spdk_group_sock_cb(void *arg,
                                   spdk_sock_group *group,
                                   spdk_sock *sock) noexcept;
    static bool use_spdk_sock_backend() noexcept;
    void reset_connection_frame(connection_context *conn) noexcept;
    int fill_connection_frame(connection_context *conn,
                              uint8_t *buf,
                              size_t target,
                              size_t *completed) noexcept;
    int try_receive_one_request(connection_context *conn,
                                raw_header *hdr_out,
                                std::vector<uint8_t> *body_out) noexcept;
    void reset_connection_send(connection_context *conn) noexcept;
    int flush_connection_send_frame(connection_context *conn) noexcept;
    ssize_t connection_recv(connection_context *conn, void *buf, size_t len) noexcept;
    ssize_t connection_send(connection_context *conn, const void *buf, size_t len) noexcept;
    void close_connection(connection_context *conn) noexcept;
    void service_connection_io(connection_context *conn, short revents) noexcept;
    void service_connection_write(connection_context *conn) noexcept;
    void cleanup_finished_connections() noexcept;
    void log_connection_summary(uint32_t shard_id) noexcept;
    void run_listener(uint32_t shard_id) noexcept;
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
