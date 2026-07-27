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
#include <endian.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr uint16_t min_raw_rdma_port = 20001U;
constexpr uint16_t max_raw_rdma_port = 29999U;
constexpr int raw_rdma_bind_attempts = 64;
/* raw header (24) + max object body (~4MiB) + margin */
constexpr size_t raw_rdma_recv_buf_len = (4U * 1024U * 1024U) + 4096U;
constexpr size_t raw_rdma_send_buf_len = raw_rdma_recv_buf_len;
constexpr size_t max_raw_body_len = (4U * 1024U * 1024U) + 1024U;

constexpr uint32_t raw_magic = 0x46425257U;
constexpr uint8_t raw_version_major = 1U;
constexpr uint8_t raw_version_minor = 0U;
constexpr uint8_t raw_service_osd = 2U;

constexpr uint8_t raw_op_get_leader = 1U;
constexpr uint8_t raw_op_read_object = 2U;
constexpr uint8_t raw_op_write_object = 3U;
constexpr uint8_t raw_op_delete_object = 4U;

constexpr uint32_t raw_flag_response = 1U << 0;

constexpr uint32_t raw_status_ok = 0U;
constexpr uint32_t raw_status_invalid_request = 1U;
constexpr uint32_t raw_status_not_found = 2U;
constexpr uint32_t raw_status_stale_epoch = 3U;
constexpr uint32_t raw_status_retry_later = 4U;
constexpr uint32_t raw_status_not_leader = 5U;
constexpr uint32_t raw_status_pg_initializing = 6U;
constexpr uint32_t raw_status_osd_down = 7U;
constexpr uint32_t raw_status_internal_error = 8U;

struct raw_header {
    uint32_t magic;
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t service;
    uint8_t opcode;
    uint32_t flags;
    uint64_t seq;
    uint32_t status;
    uint32_t body_len;
} __attribute__((packed));

uint16_t random_raw_rdma_port() {
    thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(min_raw_rdma_port,
                                                 max_raw_rdma_port);
    return static_cast<uint16_t>(dist(gen));
}

bool validate_request_header(const raw_header& hdr) noexcept {
    if (le32toh(hdr.magic) != raw_magic) {
        return false;
    }
    if (hdr.version_major != raw_version_major) {
        return false;
    }
    if (hdr.service != raw_service_osd) {
        return false;
    }
    if ((le32toh(hdr.flags) & raw_flag_response) != 0) {
        return false;
    }
    if (le32toh(hdr.body_len) > max_raw_body_len) {
        return false;
    }
    return true;
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
    conn->send_in_flight = false;
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
    if (conn->send_mr) {
        ::ibv_dereg_mr(conn->send_mr);
        conn->send_mr = nullptr;
    }
    if (conn->send_buf) {
        ::free(conn->send_buf);
        conn->send_buf = nullptr;
        conn->send_buf_len = 0;
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

bool osd_raw_rdma_server::ensure_send_mr(connection_context* conn) noexcept {
    if (!conn || !conn->pd) {
        return false;
    }
    if (conn->send_buf && conn->send_mr) {
        return true;
    }
    if (!conn->send_buf) {
        conn->send_buf = ::malloc(raw_rdma_send_buf_len);
        if (!conn->send_buf) {
            return false;
        }
        conn->send_buf_len = raw_rdma_send_buf_len;
    }
    conn->send_mr = ::ibv_reg_mr(conn->pd, conn->send_buf, conn->send_buf_len,
                                 0);
    if (!conn->send_mr) {
        SPDK_ERRLOG("raw RDMA: ibv_reg_mr send failed: %s\n",
                    std::strerror(errno));
        ::free(conn->send_buf);
        conn->send_buf = nullptr;
        conn->send_buf_len = 0;
        return false;
    }
    return true;
}

bool osd_raw_rdma_server::post_send(connection_context* conn,
                                    size_t length) noexcept {
    if (!conn || !conn->id || !conn->id->qp || !conn->send_mr ||
        !conn->send_buf || length == 0 || length > conn->send_buf_len) {
        return false;
    }
    if (conn->send_in_flight) {
        SPDK_ERRLOG("raw RDMA: post_send while previous SEND in flight\n");
        return false;
    }

    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(conn->send_buf);
    sge.length = static_cast<uint32_t>(length);
    sge.lkey = conn->send_mr->lkey;

    ibv_send_wr wr{};
    wr.wr_id = reinterpret_cast<uint64_t>(conn);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    ibv_send_wr* bad = nullptr;
    if (::ibv_post_send(conn->id->qp, &wr, &bad)) {
        SPDK_ERRLOG("raw RDMA: ibv_post_send failed: %s\n", std::strerror(errno));
        return false;
    }
    conn->send_in_flight = true;
    return true;
}

void osd_raw_rdma_server::handle_recv_complete(connection_context* conn,
                                               uint32_t byte_len) noexcept {
    if (!conn || !conn->recv_buf || byte_len < sizeof(raw_header)) {
        SPDK_ERRLOG("raw RDMA: RECV too short (%u)\n", byte_len);
        return;
    }

    raw_header hdr{};
    std::memcpy(&hdr, conn->recv_buf, sizeof(hdr));
    if (!validate_request_header(hdr)) {
        SPDK_ERRLOG("raw RDMA: invalid request header op=%u body_len=%u\n",
                    hdr.opcode, le32toh(hdr.body_len));
        return;
    }

    const uint32_t body_len = le32toh(hdr.body_len);
    if (sizeof(raw_header) + body_len > byte_len) {
        SPDK_ERRLOG("raw RDMA: truncated body need=%zu got=%u\n",
                    sizeof(raw_header) + body_len, byte_len);
        return;
    }

    /* Opcode dispatch + response SEND land in follow-up commits. */
    (void)body_len;
    SPDK_DEBUGLOG(osd, "raw RDMA RECV seq=%llu op=%u body=%u\n",
                  static_cast<unsigned long long>(le64toh(hdr.seq)),
                  hdr.opcode, body_len);
}

void osd_raw_rdma_server::poll_cq(connection_context* conn) noexcept {
    if (!conn || !conn->cq) {
        return;
    }
    ibv_wc wc[16];
    const int n = ::ibv_poll_cq(conn->cq, 16, wc);
    for (int i = 0; i < n; ++i) {
        if (wc[i].status != IBV_WC_SUCCESS) {
            SPDK_ERRLOG("raw RDMA CQ error status=%d opcode=%d\n",
                        wc[i].status, wc[i].opcode);
            if (wc[i].opcode == IBV_WC_SEND) {
                conn->send_in_flight = false;
            }
            continue;
        }
        if (wc[i].opcode == IBV_WC_RECV) {
            handle_recv_complete(conn, wc[i].byte_len);
            post_recv(conn);
        } else if (wc[i].opcode == IBV_WC_SEND) {
            conn->send_in_flight = false;
        }
    }
}

bool osd_raw_rdma_server::post_recv(connection_context* conn) noexcept {
    if (!conn || !conn->id || !conn->id->qp || !conn->pd) {
        return false;
    }
    if (!conn->recv_buf) {
        conn->recv_buf = ::malloc(raw_rdma_recv_buf_len);
        if (!conn->recv_buf) {
            return false;
        }
        conn->recv_buf_len = raw_rdma_recv_buf_len;
        conn->recv_mr = ::ibv_reg_mr(
          conn->pd, conn->recv_buf, conn->recv_buf_len,
          IBV_ACCESS_LOCAL_WRITE);
        if (!conn->recv_mr) {
            ::free(conn->recv_buf);
            conn->recv_buf = nullptr;
            conn->recv_buf_len = 0;
            return false;
        }
    }

    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(conn->recv_buf);
    sge.length = static_cast<uint32_t>(conn->recv_buf_len);
    sge.lkey = conn->recv_mr->lkey;

    ibv_recv_wr wr{};
    wr.wr_id = reinterpret_cast<uint64_t>(conn);
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_recv_wr* bad = nullptr;
    if (::ibv_post_recv(conn->id->qp, &wr, &bad)) {
        SPDK_ERRLOG("raw RDMA: ibv_post_recv failed: %s\n", std::strerror(errno));
        return false;
    }
    return true;
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
        {
            std::lock_guard<std::mutex> lock(_connections_mutex);
            for (auto& c : _connections) {
                if (c && c->shard_id == shard_id && c->established) {
                    poll_cq(c.get());
                }
            }
        }

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
                if (!post_recv(conn)) {
                    SPDK_ERRLOG(
                      "raw RDMA shard %u post_recv failed after ESTABLISHED\n",
                      shard_id);
                }
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
