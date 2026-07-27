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

#include "raw_rdma_server.h"

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <spdk/log.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <random>

namespace {

constexpr uint16_t min_raw_rdma_port = 20001U;
constexpr uint16_t max_raw_rdma_port = 29999U;
constexpr int raw_rdma_bind_attempts = 64;

uint16_t random_raw_rdma_port() {
    thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(min_raw_rdma_port,
                                                 max_raw_rdma_port);
    return static_cast<uint16_t>(dist(gen));
}

} // namespace

osd_raw_rdma_server::osd_raw_rdma_server(osd_service* service)
  : _service(service) {}

osd_raw_rdma_server::~osd_raw_rdma_server() noexcept {
    stop();
}

bool osd_raw_rdma_server::start_listener(uint32_t shard_id) {
    if (shard_id >= _listeners.size() || !_listeners[shard_id]) {
        return false;
    }
    auto& listener = *_listeners[shard_id];
    listener.shard_id = shard_id;
    listener.stop.store(false, std::memory_order_release);

    listener.channel = ::rdma_create_event_channel();
    if (!listener.channel) {
        SPDK_ERRLOG("raw RDMA: rdma_create_event_channel failed: %s\n",
                    std::strerror(errno));
        return false;
    }

    if (::rdma_create_id(listener.channel, &listener.listen_id, &listener,
                         RDMA_PS_TCP)) {
        SPDK_ERRLOG("raw RDMA: rdma_create_id failed: %s\n",
                    std::strerror(errno));
        ::rdma_destroy_event_channel(listener.channel);
        listener.channel = nullptr;
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (::inet_pton(AF_INET, _bind_address.c_str(), &addr.sin_addr) != 1) {
        SPDK_ERRLOG("raw RDMA: invalid bind address %s\n",
                    _bind_address.c_str());
        return false;
    }

    bool bound = false;
    for (int attempt = 0; attempt < raw_rdma_bind_attempts; ++attempt) {
        const uint16_t port = random_raw_rdma_port();
        addr.sin_port = htons(port);
        if (::rdma_bind_addr(listener.listen_id,
                             reinterpret_cast<sockaddr*>(&addr)) == 0) {
            listener.port = port;
            bound = true;
            break;
        }
    }
    if (!bound) {
        SPDK_ERRLOG("raw RDMA: rdma_bind_addr failed on %s after %d tries\n",
                    _bind_address.c_str(), raw_rdma_bind_attempts);
        return false;
    }

    if (::rdma_listen(listener.listen_id, 128)) {
        SPDK_ERRLOG("raw RDMA: rdma_listen failed on %s:%u: %s\n",
                    _bind_address.c_str(), listener.port, std::strerror(errno));
        return false;
    }

    SPDK_NOTICELOG("raw RDMA shard %u listening on %s:%u\n",
                   shard_id, _bind_address.c_str(), listener.port);
    listener.worker = std::thread([this, shard_id]() {
        run_listener(shard_id);
    });
    return true;
}

void osd_raw_rdma_server::destroy_connection(connection_context* conn) noexcept {
    if (!conn) {
        return;
    }
    conn->established = false;
    if (conn->id) {
        if (conn->id->qp) {
            ::rdma_destroy_qp(conn->id);
        }
        ::rdma_destroy_id(conn->id);
        conn->id = nullptr;
    }
    if (conn->recv_mr) {
        ::ibv_dereg_mr(conn->recv_mr);
        conn->recv_mr = nullptr;
    }
    if (conn->recv_buf) {
        ::free(conn->recv_buf);
        conn->recv_buf = nullptr;
        conn->recv_buf_len = 0;
    }
    if (conn->cq) {
        ::ibv_destroy_cq(conn->cq);
        conn->cq = nullptr;
    }
    if (conn->pd) {
        ::ibv_dealloc_pd(conn->pd);
        conn->pd = nullptr;
    }
}

void osd_raw_rdma_server::close_all_connections() noexcept {
    std::lock_guard<std::mutex> lock(_connections_mutex);
    for (auto& conn : _connections) {
        destroy_connection(conn.get());
    }
    _connections.clear();
}

bool osd_raw_rdma_server::handle_connect_request(rdma_cm_id* id,
                                                 uint32_t shard_id) noexcept {
    if (!id || !id->verbs) {
        return false;
    }

    auto conn = std::make_unique<connection_context>();
    conn->shard_id = shard_id;
    /* Own cm_id only after accept succeeds; caller rejects/destroys on failure. */
    conn->id = nullptr;
    id->context = nullptr;

    conn->pd = ::ibv_alloc_pd(id->verbs);
    if (!conn->pd) {
        SPDK_ERRLOG("raw RDMA: ibv_alloc_pd failed: %s\n", std::strerror(errno));
        return false;
    }

    conn->cq = ::ibv_create_cq(id->verbs, 64, nullptr, nullptr, 0);
    if (!conn->cq) {
        SPDK_ERRLOG("raw RDMA: ibv_create_cq failed: %s\n", std::strerror(errno));
        destroy_connection(conn.get());
        return false;
    }

    ibv_qp_init_attr qp_attr{};
    qp_attr.send_cq = conn->cq;
    qp_attr.recv_cq = conn->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 32;
    qp_attr.cap.max_recv_wr = 32;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.sq_sig_all = 0;

    if (::rdma_create_qp(id, conn->pd, &qp_attr)) {
        SPDK_ERRLOG("raw RDMA: rdma_create_qp failed: %s\n",
                    std::strerror(errno));
        destroy_connection(conn.get());
        return false;
    }

    rdma_conn_param param{};
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 3;
    param.rnr_retry_count = 3;
    if (::rdma_accept(id, &param)) {
        SPDK_ERRLOG("raw RDMA: rdma_accept failed: %s\n", std::strerror(errno));
        if (id->qp) {
            ::rdma_destroy_qp(id);
        }
        destroy_connection(conn.get());
        return false;
    }

    conn->id = id;
    id->context = conn.get();
    {
        std::lock_guard<std::mutex> lock(_connections_mutex);
        _connections.emplace_back(std::move(conn));
    }
    SPDK_NOTICELOG("raw RDMA shard %u accepted connection request\n", shard_id);
    return true;
}

void osd_raw_rdma_server::stop_listener(listener_context& listener) noexcept {
    listener.stop.store(true, std::memory_order_release);
    if (listener.worker.joinable()) {
        listener.worker.join();
    }
    if (listener.listen_id) {
        ::rdma_destroy_id(listener.listen_id);
        listener.listen_id = nullptr;
    }
    if (listener.channel) {
        ::rdma_destroy_event_channel(listener.channel);
        listener.channel = nullptr;
    }
    listener.port = 0;
}

void osd_raw_rdma_server::run_listener(uint32_t shard_id) noexcept {
    if (shard_id >= _listeners.size() || !_listeners[shard_id]) {
        return;
    }
    auto& listener = *_listeners[shard_id];
    if (!listener.channel) {
        return;
    }

    while (!listener.stop.load(std::memory_order_acquire)) {
        pollfd pfd{};
        pfd.fd = listener.channel->fd;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, 200);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            SPDK_ERRLOG("raw RDMA shard %u poll failed: %s\n",
                        shard_id, std::strerror(errno));
            break;
        }
        if (rc == 0 || (pfd.revents & POLLIN) == 0) {
            continue;
        }

        rdma_cm_event* event = nullptr;
        if (::rdma_get_cm_event(listener.channel, &event)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (!listener.stop.load(std::memory_order_acquire)) {
                SPDK_ERRLOG("raw RDMA shard %u get_cm_event failed: %s\n",
                            shard_id, std::strerror(errno));
            }
            break;
        }

        if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
            if (!handle_connect_request(event->id, shard_id)) {
                ::rdma_reject(event->id, nullptr, 0);
                ::rdma_destroy_id(event->id);
            }
        } else if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
            auto* conn = static_cast<connection_context*>(event->id->context);
            if (conn) {
                conn->established = true;
                SPDK_NOTICELOG("raw RDMA shard %u connection established\n",
                               shard_id);
            }
        } else if (event->event == RDMA_CM_EVENT_DISCONNECTED ||
                   event->event == RDMA_CM_EVENT_DEVICE_REMOVAL) {
            auto* conn = static_cast<connection_context*>(event->id->context);
            ::rdma_ack_cm_event(event);
            event = nullptr;
            if (conn) {
                std::lock_guard<std::mutex> lock(_connections_mutex);
                for (auto it = _connections.begin(); it != _connections.end();
                     ++it) {
                    if (it->get() == conn) {
                        destroy_connection(conn);
                        _connections.erase(it);
                        break;
                    }
                }
            }
            continue;
        }
        if (event) {
            ::rdma_ack_cm_event(event);
        }
    }
}

bool osd_raw_rdma_server::start(const std::string& bind_address,
                                uint32_t shard_count) {
    if (_running.load(std::memory_order_acquire)) {
        return true;
    }
    if (!_service || bind_address.empty() || shard_count == 0) {
        return false;
    }

    _bind_address = bind_address;
    _listeners.clear();
    _listeners.reserve(shard_count);
    for (uint32_t i = 0; i < shard_count; ++i) {
        _listeners.emplace_back(std::make_unique<listener_context>());
    }
    for (uint32_t i = 0; i < shard_count; ++i) {
        if (!start_listener(i)) {
            stop();
            return false;
        }
    }
    SPDK_NOTICELOG("raw RDMA server started on %s shards=%u\n",
                   _bind_address.c_str(), shard_count);
    _running.store(true, std::memory_order_release);
    return true;
}

void osd_raw_rdma_server::stop() noexcept {
    if (!_running.exchange(false, std::memory_order_acq_rel) &&
        _listeners.empty()) {
        return;
    }
    for (auto& listener : _listeners) {
        if (listener) {
            stop_listener(*listener);
        }
    }
    close_all_connections();
    _listeners.clear();
    _bind_address.clear();
    SPDK_NOTICELOG("raw RDMA server stopped\n");
}

uint16_t osd_raw_rdma_server::listen_port(uint32_t shard_id) const noexcept {
    if (shard_id >= _listeners.size() || !_listeners[shard_id]) {
        return 0;
    }
    return _listeners[shard_id]->port;
}
