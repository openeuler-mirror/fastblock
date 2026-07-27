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

#include "osd_service.h"
#include "fastblock/rpc/osd_msg.pb.h"
#include "fastblock/utils/err_num.h"

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <spdk/log.h>

#include <arpa/inet.h>
#include <endian.h>
#include <google/protobuf/stubs/callback.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t min_raw_rdma_port = 20001U;
constexpr uint16_t max_raw_rdma_port = 29999U;
constexpr int raw_rdma_bind_attempts = 64;
/* Backlog for rdma_listen; parallel to TCP raw accept queue depth. */
constexpr int raw_rdma_listen_backlog = 128;
/* Listener poll() timeout between CQ sweeps (ms). */
constexpr int raw_rdma_poll_timeout_ms = 200;
/* Per-connection CQ capacity; must cover multi-slot RECV + SEND pipeline. */
constexpr int raw_rdma_cq_depth = 64;
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

struct raw_get_leader_req {
    uint32_t pool_id;
    uint32_t pg_id;
} __attribute__((packed));

struct raw_get_leader_rsp {
    uint32_t leader_id;
    uint16_t leader_port;
    uint16_t address_len;
} __attribute__((packed));

struct raw_read_object_req {
    uint32_t pool_id;
    uint32_t pg_id;
    uint64_t offset;
    uint32_t length;
    uint16_t object_name_len;
    uint16_t reserved;
} __attribute__((packed));

struct raw_read_object_rsp {
    uint32_t data_len;
    uint32_t reserved;
} __attribute__((packed));

struct raw_write_object_req {
    uint32_t pool_id;
    uint32_t pg_id;
    uint64_t offset;
    uint32_t data_len;
    uint16_t object_name_len;
    uint16_t reserved;
} __attribute__((packed));

struct raw_delete_object_req {
    uint32_t pool_id;
    uint32_t pg_id;
    uint16_t object_name_len;
    uint16_t reserved;
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
    /* Minor is soft: accept any minor for forward compatibility. */
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

const char* raw_opcode_name(uint8_t op) noexcept {
    switch (op) {
    case raw_op_get_leader:
        return "GET_LEADER";
    case raw_op_read_object:
        return "READ";
    case raw_op_write_object:
        return "WRITE";
    case raw_op_delete_object:
        return "DELETE";
    default:
        return "UNKNOWN";
    }
}

uint32_t raw_status_from_errno(const int state) noexcept {
    switch (state) {
    case err::E_SUCCESS:
        return raw_status_ok;
    case err::E_INVAL:
        return raw_status_invalid_request;
    case err::RAFT_ERR_NOT_FOUND_PG:
    case err::ERR_NOT_FOUND_POOL:
        return raw_status_not_found;
    case err::RAFT_ERR_NOT_FOUND_LEADER:
    case err::RAFT_ERR_NO_CONNECTED:
    case err::RAFT_ERR_MEMBERSHIP_CHANGING:
    case err::RAFT_ERR_SNAPSHOT_WAIT_APPLY:
        return raw_status_retry_later;
    case err::RAFT_ERR_NOT_LEADER:
        return raw_status_not_leader;
    case err::RAFT_ERR_PG_INITIALIZING:
    case err::OSD_STARTING:
        return raw_status_pg_initializing;
    case err::OSD_DOWN:
        return raw_status_osd_down;
    default:
        return raw_status_internal_error;
    }
}

raw_header make_response_header(const raw_header& req,
                                uint32_t status,
                                uint32_t body_len) noexcept {
    raw_header rsp{};
    rsp.magic = htole32(raw_magic);
    rsp.version_major = raw_version_major;
    rsp.version_minor = raw_version_minor;
    rsp.service = req.service;
    rsp.opcode = req.opcode;
    rsp.flags = htole32(raw_flag_response);
    rsp.seq = req.seq;
    rsp.status = htole32(status);
    rsp.body_len = htole32(body_len);
    return rsp;
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

    if (::rdma_listen(listener.listen_id, raw_rdma_listen_backlog)) {
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
    /* Best-effort drain so in-flight responses can complete. */
    drain_send_queue(conn, 200);
    conn->send_in_flight = false;
    {
        std::lock_guard<std::mutex> lock(conn->send_mu);
        conn->send_queue.clear();
    }
    if (conn->id) {
        if (conn->id->qp) {
            ::rdma_destroy_qp(conn->id);
        }
        ::rdma_destroy_id(conn->id);
        conn->id = nullptr;
    }
    free_recv_slots(conn);
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

std::shared_ptr<osd_raw_rdma_server::connection_context>
osd_raw_rdma_server::retain_connection(connection_context* conn) noexcept {
    if (!conn) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(_connections_mutex);
    for (auto& c : _connections) {
        if (c.get() == conn) {
            return c;
        }
    }
    return nullptr;
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

bool osd_raw_rdma_server::enqueue_response_frame(
  connection_context* conn,
  std::vector<uint8_t> frame) noexcept {
    if (!conn || frame.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(conn->send_mu);
        if (conn->send_queue.size() >= connection_context::max_send_queue) {
            SPDK_ERRLOG("raw RDMA: send queue full depth=%lu\n",
                        static_cast<unsigned long>(conn->send_queue.size()));
            conn->error_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        conn->send_queue.emplace_back(std::move(frame));
    }
    try_flush_send_queue(conn);
    return true;
}

void osd_raw_rdma_server::try_flush_send_queue(connection_context* conn) noexcept {
    if (!conn) {
        return;
    }
    if (!ensure_send_mr(conn)) {
        return;
    }

    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> lock(conn->send_mu);
        if (conn->send_in_flight || conn->send_queue.empty()) {
            return;
        }
        frame = std::move(conn->send_queue.front());
        conn->send_queue.pop_front();
    }
    if (frame.size() > conn->send_buf_len) {
        SPDK_ERRLOG("raw RDMA: queued frame too large size=%zu\n", frame.size());
        return;
    }
    std::memcpy(conn->send_buf, frame.data(), frame.size());
    if (!post_send(conn, frame.size())) {
        SPDK_ERRLOG("raw RDMA: flush post_send failed\n");
    }
}

void osd_raw_rdma_server::drain_send_queue(connection_context* conn,
                                           int timeout_ms) noexcept {
    if (!conn || timeout_ms < 0) {
        return;
    }
    try_flush_send_queue(conn);
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool inflight = false;
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> lock(conn->send_mu);
            inflight = conn->send_in_flight;
            queued = conn->send_queue.size();
        }
        if (!inflight && queued == 0) {
            return;
        }
        poll_cq(conn);
        try_flush_send_queue(conn);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    size_t leftover = 0;
    {
        std::lock_guard<std::mutex> lock(conn->send_mu);
        leftover = conn->send_queue.size();
    }
    if (leftover > 0 || conn->send_in_flight) {
        SPDK_NOTICELOG(
          "raw RDMA drain timeout peer=%s queued=%zu inflight=%d\n",
          conn->peer_address.c_str(), leftover,
          conn->send_in_flight ? 1 : 0);
    }
}

bool osd_raw_rdma_server::send_response(connection_context* conn,
                                        const void* req_hdr,
                                        uint32_t status,
                                        const void* body,
                                        uint32_t body_len) noexcept {
    if (!conn || !req_hdr) {
        return false;
    }
    if (body_len > 0 && !body) {
        return false;
    }
    if (sizeof(raw_header) + body_len > raw_rdma_send_buf_len) {
        SPDK_ERRLOG("raw RDMA: response too large body=%u\n", body_len);
        return false;
    }

    raw_header req{};
    std::memcpy(&req, req_hdr, sizeof(req));
    const raw_header rsp = make_response_header(req, status, body_len);
    std::vector<uint8_t> frame(sizeof(rsp) + body_len);
    std::memcpy(frame.data(), &rsp, sizeof(rsp));
    if (body_len > 0) {
        std::memcpy(frame.data() + sizeof(rsp), body, body_len);
    }
    return enqueue_response_frame(conn, std::move(frame));
}

namespace {

class raw_rdma_async_done : public google::protobuf::Closure {
public:
    explicit raw_rdma_async_done(std::function<void()> fn)
      : _fn(std::move(fn)) {}

    void Run() override {
        if (_fn) {
            _fn();
        }
        delete this;
    }

private:
    std::function<void()> _fn{};
};

std::vector<uint8_t> build_read_response_body(const std::string& data) {
    raw_read_object_rsp rsp{};
    std::vector<uint8_t> body(sizeof(rsp) + data.size());
    rsp.data_len = htole32(static_cast<uint32_t>(data.size()));
    rsp.reserved = 0;
    std::memcpy(body.data(), &rsp, sizeof(rsp));
    if (!data.empty()) {
        std::memcpy(body.data() + sizeof(rsp), data.data(), data.size());
    }
    return body;
}

} // namespace

void osd_raw_rdma_server::dispatch_read(connection_context* conn,
                                        const void* req_hdr,
                                        const uint8_t* body,
                                        uint32_t body_len) noexcept {
    if (!conn || !req_hdr || !_service) {
        return;
    }
    if (!body || body_len < sizeof(raw_read_object_req)) {
        send_response(conn, req_hdr, raw_status_invalid_request, nullptr, 0);
        return;
    }

    raw_read_object_req req{};
    std::memcpy(&req, body, sizeof(req));
    const uint16_t object_name_len = le16toh(req.object_name_len);
    if (body_len != sizeof(req) + object_name_len || object_name_len == 0) {
        send_response(conn, req_hdr, raw_status_invalid_request, nullptr, 0);
        return;
    }

    struct read_ctx {
        osd_raw_rdma_server* server{nullptr};
        std::shared_ptr<connection_context> conn{};
        raw_header req_hdr{};
        osd::read_request request{};
        osd::read_reply response{};
    };
    auto rctx = std::make_shared<read_ctx>();
    rctx->server = this;
    rctx->conn = retain_connection(conn);
    if (!rctx->conn) {
        return;
    }
    std::memcpy(&rctx->req_hdr, req_hdr, sizeof(rctx->req_hdr));
    rctx->request.set_pool_id(le32toh(req.pool_id));
    rctx->request.set_pg_id(le32toh(req.pg_id));
    rctx->request.set_offset(le64toh(req.offset));
    rctx->request.set_length(le32toh(req.length));
    rctx->request.set_object_name(
      reinterpret_cast<const char*>(body + sizeof(req)), object_name_len);

    _service->process_read(
      nullptr, &rctx->request, &rctx->response,
      new raw_rdma_async_done([rctx]() {
          if (!rctx->server || !rctx->conn) {
              return;
          }
          auto* c = rctx->conn.get();
          const auto state = rctx->response.state();
          if (state != err::E_SUCCESS) {
              rctx->server->send_response(
                c, &rctx->req_hdr,
                raw_status_from_errno(state), nullptr, 0);
              return;
          }
          if (rctx->response.data().size() >
              max_raw_body_len - sizeof(raw_read_object_rsp)) {
              rctx->server->send_response(
                c, &rctx->req_hdr, raw_status_internal_error, nullptr, 0);
              return;
          }
          auto body_out = build_read_response_body(rctx->response.data());
          rctx->server->send_response(
            c, &rctx->req_hdr, raw_status_ok,
            body_out.data(), static_cast<uint32_t>(body_out.size()));
      }));
}

void osd_raw_rdma_server::dispatch_write(connection_context* conn,
                                         const void* req_hdr,
                                         const uint8_t* body,
                                         uint32_t body_len) noexcept {
    if (!conn || !req_hdr || !_service) {
        return;
    }
    if (!body || body_len < sizeof(raw_write_object_req)) {
        send_response(conn, req_hdr, raw_status_invalid_request, nullptr, 0);
        return;
    }

    raw_write_object_req req{};
    std::memcpy(&req, body, sizeof(req));
    const uint16_t object_name_len = le16toh(req.object_name_len);
    const uint32_t data_len = le32toh(req.data_len);
    if (body_len != sizeof(req) + object_name_len + data_len ||
        object_name_len == 0) {
        send_response(conn, req_hdr, raw_status_invalid_request, nullptr, 0);
        return;
    }

    struct write_ctx {
        osd_raw_rdma_server* server{nullptr};
        std::shared_ptr<connection_context> conn{};
        raw_header req_hdr{};
        osd::write_request request{};
        osd::write_reply response{};
    };
    auto wctx = std::make_shared<write_ctx>();
    wctx->server = this;
    wctx->conn = retain_connection(conn);
    if (!wctx->conn) {
        return;
    }
    std::memcpy(&wctx->req_hdr, req_hdr, sizeof(wctx->req_hdr));
    wctx->request.set_pool_id(le32toh(req.pool_id));
    wctx->request.set_pg_id(le32toh(req.pg_id));
    wctx->request.set_offset(le64toh(req.offset));
    wctx->request.set_object_name(
      reinterpret_cast<const char*>(body + sizeof(req)), object_name_len);
    wctx->request.set_data(
      reinterpret_cast<const char*>(body + sizeof(req) + object_name_len),
      data_len);

    _service->process_write(
      nullptr, &wctx->request, &wctx->response,
      new raw_rdma_async_done([wctx]() {
          if (!wctx->server || !wctx->conn) {
              return;
          }
          wctx->server->send_response(
            wctx->conn.get(), &wctx->req_hdr,
            raw_status_from_errno(wctx->response.state()), nullptr, 0);
      }));
}

void osd_raw_rdma_server::dispatch_delete(connection_context* conn,
                                          const void* req_hdr,
                                          const uint8_t* body,
                                          uint32_t body_len) noexcept {
    if (!conn || !req_hdr || !_service) {
        return;
    }
    if (!body || body_len < sizeof(raw_delete_object_req)) {
        send_response(conn, req_hdr, raw_status_invalid_request, nullptr, 0);
        return;
    }

    raw_delete_object_req req{};
    std::memcpy(&req, body, sizeof(req));
    const uint16_t object_name_len = le16toh(req.object_name_len);
    if (body_len != sizeof(req) + object_name_len || object_name_len == 0) {
        send_response(conn, req_hdr, raw_status_invalid_request, nullptr, 0);
        return;
    }

    struct delete_ctx {
        osd_raw_rdma_server* server{nullptr};
        std::shared_ptr<connection_context> conn{};
        raw_header req_hdr{};
        osd::delete_request request{};
        osd::delete_reply response{};
    };
    auto dctx = std::make_shared<delete_ctx>();
    dctx->server = this;
    dctx->conn = retain_connection(conn);
    if (!dctx->conn) {
        return;
    }
    std::memcpy(&dctx->req_hdr, req_hdr, sizeof(dctx->req_hdr));
    dctx->request.set_pool_id(le32toh(req.pool_id));
    dctx->request.set_pg_id(le32toh(req.pg_id));
    dctx->request.set_object_name(
      reinterpret_cast<const char*>(body + sizeof(req)), object_name_len);

    _service->process_delete(
      nullptr, &dctx->request, &dctx->response,
      new raw_rdma_async_done([dctx]() {
          if (!dctx->server || !dctx->conn) {
              return;
          }
          dctx->server->send_response(
            dctx->conn.get(), &dctx->req_hdr,
            raw_status_from_errno(dctx->response.state()), nullptr, 0);
      }));
}

void osd_raw_rdma_server::dispatch_get_leader(connection_context* conn,
                                              const void* req_hdr,
                                              const uint8_t* body,
                                              uint32_t body_len) noexcept {
    if (!conn || !req_hdr || !_service) {
        return;
    }
    if (body_len != sizeof(raw_get_leader_req) || !body) {
        send_response(conn, req_hdr, raw_status_invalid_request, nullptr, 0);
        return;
    }

    raw_get_leader_req req{};
    std::memcpy(&req, body, sizeof(req));
    /* Return raw RDMA data-plane port so kernel client can stay on RDMA. */
    auto leader = _service->resolve_pg_leader_raw_rdma(le32toh(req.pool_id),
                                                      le32toh(req.pg_id));
    if (leader.state != err::E_SUCCESS) {
        send_response(conn, req_hdr,
                      raw_status_from_errno(leader.state), nullptr, 0);
        return;
    }
    if (leader.leader_port <= 0 ||
        leader.leader_port > std::numeric_limits<uint16_t>::max() ||
        leader.leader_id < 0 ||
        leader.leader_addr.size() > std::numeric_limits<uint16_t>::max()) {
        send_response(conn, req_hdr, raw_status_internal_error, nullptr, 0);
        return;
    }

    raw_get_leader_rsp rsp{};
    rsp.leader_id = htole32(static_cast<uint32_t>(leader.leader_id));
    rsp.leader_port = htole16(static_cast<uint16_t>(leader.leader_port));
    rsp.address_len = htole16(static_cast<uint16_t>(leader.leader_addr.size()));

    std::vector<uint8_t> out(sizeof(rsp) + leader.leader_addr.size());
    std::memcpy(out.data(), &rsp, sizeof(rsp));
    if (!leader.leader_addr.empty()) {
        std::memcpy(out.data() + sizeof(rsp), leader.leader_addr.data(),
                    leader.leader_addr.size());
    }
    send_response(conn, req_hdr, raw_status_ok, out.data(),
                  static_cast<uint32_t>(out.size()));
}

void osd_raw_rdma_server::handle_recv_complete(connection_context* conn,
                                               uint32_t byte_len,
                                               size_t slot) noexcept {
    if (!conn || slot >= connection_context::max_recv_slots) {
        return;
    }
    auto& rs = conn->recv_slots[slot];
    rs.posted = false;
    if (!rs.buf || byte_len < sizeof(raw_header)) {
        conn->error_count.fetch_add(1, std::memory_order_relaxed);
        _dispatch_error_total.fetch_add(1, std::memory_order_relaxed);
        SPDK_ERRLOG("raw RDMA: RECV too short (%u) slot=%zu\n", byte_len, slot);
        return;
    }

    raw_header hdr{};
    std::memcpy(&hdr, rs.buf, sizeof(hdr));
    if (!validate_request_header(hdr)) {
        conn->error_count.fetch_add(1, std::memory_order_relaxed);
        _dispatch_error_total.fetch_add(1, std::memory_order_relaxed);
        SPDK_ERRLOG("raw RDMA: invalid request header op=%u body_len=%u\n",
                    hdr.opcode, le32toh(hdr.body_len));
        send_response(conn, &hdr, raw_status_invalid_request, nullptr, 0);
        return;
    }

    const uint32_t body_len = le32toh(hdr.body_len);
    if (sizeof(raw_header) + body_len > byte_len) {
        conn->error_count.fetch_add(1, std::memory_order_relaxed);
        _dispatch_error_total.fetch_add(1, std::memory_order_relaxed);
        SPDK_ERRLOG("raw RDMA: truncated body need=%zu got=%u\n",
                    sizeof(raw_header) + body_len, byte_len);
        send_response(conn, &hdr, raw_status_invalid_request, nullptr, 0);
        return;
    }

    /* Copy out of staging slot so it can be re-posted immediately. */
    std::vector<uint8_t> body_copy;
    if (body_len > 0) {
        const auto* body_ptr =
          static_cast<const uint8_t*>(rs.buf) + sizeof(raw_header);
        body_copy.assign(body_ptr, body_ptr + body_len);
    }
    const uint8_t* body = body_copy.empty() ? nullptr : body_copy.data();

    switch (hdr.opcode) {
    case raw_op_get_leader:
        dispatch_get_leader(conn, &hdr, body, body_len);
        break;
    case raw_op_read_object:
        dispatch_read(conn, &hdr, body, body_len);
        break;
    case raw_op_write_object:
        dispatch_write(conn, &hdr, body, body_len);
        break;
    case raw_op_delete_object:
        dispatch_delete(conn, &hdr, body, body_len);
        break;
    default:
        _dispatch_error_total.fetch_add(1, std::memory_order_relaxed);
        SPDK_ERRLOG("raw RDMA: unsupported opcode=%u (%s) peer=%s\n",
                    hdr.opcode, raw_opcode_name(hdr.opcode),
                    conn->peer_address.c_str());
        send_response(conn, &hdr, raw_status_invalid_request, nullptr, 0);
        break;
    }
}

void osd_raw_rdma_server::poll_cq(connection_context* conn) noexcept {
    if (!conn || !conn->cq) {
        return;
    }
    ibv_wc wc[16];
    const int n = ::ibv_poll_cq(conn->cq, 16, wc);
    for (int i = 0; i < n; ++i) {
        if (wc[i].status != IBV_WC_SUCCESS) {
            conn->error_count.fetch_add(1, std::memory_order_relaxed);
            SPDK_ERRLOG("raw RDMA CQ error status=%d opcode=%d peer=%s\n",
                        wc[i].status, wc[i].opcode,
                        conn->peer_address.c_str());
            if (wc[i].opcode == IBV_WC_SEND) {
                conn->send_in_flight = false;
                try_flush_send_queue(conn);
            } else if (wc[i].opcode == IBV_WC_RECV) {
                /* Always re-arm the failed RECV slot so the QP stays usable. */
                const size_t slot = static_cast<size_t>(wc[i].wr_id);
                if (slot < connection_context::max_recv_slots) {
                    conn->recv_slots[slot].posted = false;
                    if (!post_recv_slot(conn, slot)) {
                        SPDK_ERRLOG(
                          "raw RDMA: re-post RECV after CQ error failed slot=%zu\n",
                          slot);
                    }
                }
            }
            continue;
        }
        if (wc[i].opcode == IBV_WC_RECV) {
            conn->recv_count.fetch_add(1, std::memory_order_relaxed);
            const size_t slot = static_cast<size_t>(wc[i].wr_id);
            handle_recv_complete(conn, wc[i].byte_len, slot);
            if (!post_recv_slot(conn, slot)) {
                conn->error_count.fetch_add(1, std::memory_order_relaxed);
                SPDK_ERRLOG("raw RDMA: post_recv_slot failed after complete slot=%zu\n",
                            slot);
            }
        } else if (wc[i].opcode == IBV_WC_SEND) {
            conn->send_count.fetch_add(1, std::memory_order_relaxed);
            conn->send_in_flight = false;
            try_flush_send_queue(conn);
        }
    }
}

void osd_raw_rdma_server::free_recv_slots(connection_context* conn) noexcept {
    if (!conn) {
        return;
    }
    for (size_t i = 0; i < connection_context::max_recv_slots; ++i) {
        auto& rs = conn->recv_slots[i];
        if (rs.mr) {
            ::ibv_dereg_mr(rs.mr);
            rs.mr = nullptr;
        }
        if (rs.buf) {
            ::free(rs.buf);
            rs.buf = nullptr;
        }
        rs.len = 0;
        rs.posted = false;
    }
    conn->recv_buf = nullptr;
    conn->recv_buf_len = 0;
    conn->recv_mr = nullptr;
}

bool osd_raw_rdma_server::ensure_recv_slots(connection_context* conn) noexcept {
    if (!conn || !conn->pd) {
        return false;
    }
    for (size_t i = 0; i < connection_context::max_recv_slots; ++i) {
        auto& rs = conn->recv_slots[i];
        if (rs.buf && rs.mr) {
            continue;
        }
        rs.buf = ::malloc(raw_rdma_recv_buf_len);
        if (!rs.buf) {
            free_recv_slots(conn);
            return false;
        }
        rs.len = raw_rdma_recv_buf_len;
        rs.mr = ::ibv_reg_mr(conn->pd, rs.buf, rs.len, IBV_ACCESS_LOCAL_WRITE);
        if (!rs.mr) {
            SPDK_ERRLOG("raw RDMA: ibv_reg_mr recv slot %zu failed: %s\n",
                        i, std::strerror(errno));
            free_recv_slots(conn);
            return false;
        }
        rs.posted = false;
    }
    /* Keep slot0 aliases for any residual single-buffer call sites. */
    conn->recv_buf = conn->recv_slots[0].buf;
    conn->recv_buf_len = conn->recv_slots[0].len;
    conn->recv_mr = conn->recv_slots[0].mr;
    return true;
}

bool osd_raw_rdma_server::post_recv_slot(connection_context* conn,
                                         size_t slot) noexcept {
    if (!conn || !conn->id || !conn->id->qp || !conn->pd ||
        slot >= connection_context::max_recv_slots) {
        return false;
    }
    if (!ensure_recv_slots(conn)) {
        return false;
    }
    auto& rs = conn->recv_slots[slot];
    if (rs.posted) {
        return true;
    }

    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(rs.buf);
    sge.length = static_cast<uint32_t>(rs.len);
    sge.lkey = rs.mr->lkey;

    ibv_recv_wr wr{};
    wr.wr_id = static_cast<uint64_t>(slot);
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_recv_wr* bad = nullptr;
    if (::ibv_post_recv(conn->id->qp, &wr, &bad)) {
        SPDK_ERRLOG("raw RDMA: ibv_post_recv slot=%zu failed: %s\n",
                    slot, std::strerror(errno));
        return false;
    }
    rs.posted = true;
    return true;
}

bool osd_raw_rdma_server::post_recv(connection_context* conn) noexcept {
    if (!conn) {
        return false;
    }
    /* Post all free slots (used on ESTABLISHED and as bulk re-arm). */
    int posted = 0;
    for (size_t i = 0; i < connection_context::max_recv_slots; ++i) {
        if (conn->recv_slots[i].posted) {
            continue;
        }
        if (post_recv_slot(conn, i)) {
            ++posted;
        }
    }
    return posted > 0 ||
           (conn->recv_slots[0].posted); /* already fully armed */
}

bool osd_raw_rdma_server::handle_connect_request(rdma_cm_id* id,
                                                 uint32_t shard_id) noexcept {
    if (!id || !id->verbs) {
        return false;
    }

    auto conn = std::make_shared<connection_context>();
    conn->shard_id = shard_id;
    /* Own cm_id only after accept succeeds; caller rejects/destroys on failure. */
    conn->id = nullptr;
    id->context = nullptr;

    conn->pd = ::ibv_alloc_pd(id->verbs);
    if (!conn->pd) {
        SPDK_ERRLOG("raw RDMA: ibv_alloc_pd failed: %s\n", std::strerror(errno));
        return false;
    }

    /* CQ depth covers multi-slot RECV + SEND queue + margin. */
    conn->cq = ::ibv_create_cq(id->verbs, raw_rdma_cq_depth, nullptr, nullptr, 0);
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
    /* At least max_recv_slots outstanding RECV WRs per connection. */
    qp_attr.cap.max_recv_wr =
      static_cast<uint32_t>(connection_context::max_recv_slots) + 4U;
    if (qp_attr.cap.max_recv_wr < 8) {
        qp_attr.cap.max_recv_wr = 8;
    }
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
        char addrbuf[INET_ADDRSTRLEN] = {};
        auto* sa = ::rdma_get_peer_addr(id);
        if (sa && sa->sa_family == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(sa);
            if (::inet_ntop(AF_INET, &sin->sin_addr, addrbuf, sizeof(addrbuf))) {
                conn->peer_address = addrbuf;
            }
        }
    }
    const std::string peer = conn->peer_address;
    {
        std::lock_guard<std::mutex> lock(_connections_mutex);
        if (_connections.size() >= max_connections) {
            SPDK_ERRLOG("raw RDMA: connections full limit=%zu peer=%s\n",
                        max_connections, peer.empty() ? "?" : peer.c_str());
            destroy_connection(conn.get());
            return false;
        }
        _connections.emplace_back(std::move(conn));
    }
    SPDK_NOTICELOG("raw RDMA shard %u accepted connection request peer=%s\n",
                   shard_id, peer.empty() ? "?" : peer.c_str());
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
        /* Snapshot shared_ptrs so poll/dispatch can retain without
         * re-locking _connections_mutex (avoids deadlock). */
        std::vector<std::shared_ptr<connection_context>> active;
        {
            std::lock_guard<std::mutex> lock(_connections_mutex);
            for (auto& c : _connections) {
                if (c && c->shard_id == shard_id && c->established) {
                    active.push_back(c);
                }
            }
        }
        for (auto& c : active) {
            poll_cq(c.get());
        }

        pollfd pfd{};
        pfd.fd = listener.channel->fd;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, raw_rdma_poll_timeout_ms);
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
            /* Refuse new work while stop() is draining listeners. */
            if (!_running.load(std::memory_order_acquire)) {
                _reject_total.fetch_add(1, std::memory_order_relaxed);
                SPDK_NOTICELOG(
                  "raw RDMA shard %u reject CONNECT_REQUEST: server not running\n",
                  shard_id);
                ::rdma_reject(event->id, nullptr, 0);
                ::rdma_destroy_id(event->id);
            } else if (!handle_connect_request(event->id, shard_id)) {
                _reject_total.fetch_add(1, std::memory_order_relaxed);
                ::rdma_reject(event->id, nullptr, 0);
                ::rdma_destroy_id(event->id);
            } else {
                _accept_total.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
            auto* conn = static_cast<connection_context*>(event->id->context);
            if (conn) {
                conn->established = true;
                /* Post multi-slot RECVs so clients can pipeline requests. */
                if (!post_recv(conn)) {
                    SPDK_ERRLOG(
                      "raw RDMA shard %u post_recv failed after ESTABLISHED peer=%s\n",
                      shard_id, conn->peer_address.c_str());
                    /* Tear down unusable connection immediately. */
                    std::lock_guard<std::mutex> lock(_connections_mutex);
                    for (auto it = _connections.begin(); it != _connections.end();
                         ++it) {
                        if (it->get() == conn) {
                            destroy_connection(conn);
                            _connections.erase(it);
                            break;
                        }
                    }
                } else {
                    SPDK_NOTICELOG(
                      "raw RDMA shard %u connection established peer=%s recv_slots=%zu\n",
                      shard_id, conn->peer_address.c_str(),
                      connection_context::max_recv_slots);
                }
            }
        } else if (event->event == RDMA_CM_EVENT_DISCONNECTED ||
                   event->event == RDMA_CM_EVENT_DEVICE_REMOVAL) {
            auto* conn = static_cast<connection_context*>(event->id->context);
            ::rdma_ack_cm_event(event);
            event = nullptr;
            if (conn) {
                SPDK_NOTICELOG(
                  "raw RDMA disconnect peer=%s recv=%lu send=%lu err=%lu\n",
                  conn->peer_address.c_str(),
                  static_cast<unsigned long>(
                    conn->recv_count.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(
                    conn->send_count.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(
                    conn->error_count.load(std::memory_order_relaxed)));
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
        SPDK_NOTICELOG("raw RDMA server already running shards=%u\n",
                       static_cast<unsigned>(this->shard_count()));
        return true;
    }
    if (!_service || bind_address.empty() || shard_count == 0) {
        SPDK_ERRLOG("raw RDMA start rejected: service=%p bind=%s shards=%u\n",
                    static_cast<void*>(_service),
                    bind_address.empty() ? "(empty)" : bind_address.c_str(),
                    shard_count);
        return false;
    }
    {
        sockaddr_in probe{};
        if (::inet_pton(AF_INET, bind_address.c_str(), &probe.sin_addr) != 1) {
            SPDK_ERRLOG("raw RDMA start rejected: invalid IPv4 bind address %s\n",
                        bind_address.c_str());
            return false;
        }
    }

    _bind_address = bind_address;
    _listeners.clear();
    _listeners.reserve(shard_count);
    for (uint32_t i = 0; i < shard_count; ++i) {
        _listeners.emplace_back(std::make_unique<listener_context>());
    }
    /* Mark running before workers process CONNECT_REQUEST so early
     * clients are accepted once a shard is listening. */
    _running.store(true, std::memory_order_release);
    for (uint32_t i = 0; i < shard_count; ++i) {
        if (!start_listener(i)) {
            stop();
            return false;
        }
    }
    SPDK_NOTICELOG("raw RDMA server started on %s shards=%u ports=[%s]\n",
                   _bind_address.c_str(), shard_count, ports_string().c_str());
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
    {
        const auto st = collect_stats();
        SPDK_NOTICELOG("raw RDMA server stopping %s\n",
                       format_raw_rdma_server_stats(st).c_str());
    }
    close_all_connections();
    _listeners.clear();
    _bind_address.clear();
    SPDK_NOTICELOG("raw RDMA server stopped\n");
}

bool osd_raw_rdma_server::is_running() const noexcept {
    return _running.load(std::memory_order_acquire);
}

uint32_t osd_raw_rdma_server::shard_count() const noexcept {
    return static_cast<uint32_t>(_listeners.size());
}

uint16_t osd_raw_rdma_server::listen_port(uint32_t shard_id) const noexcept {
    if (shard_id >= _listeners.size() || !_listeners[shard_id]) {
        return 0;
    }
    return _listeners[shard_id]->port;
}

size_t osd_raw_rdma_server::connection_count() const noexcept {
    std::lock_guard<std::mutex> lock(_connections_mutex);
    return _connections.size();
}

size_t osd_raw_rdma_server::connection_count(uint32_t shard_id) const noexcept {
    std::lock_guard<std::mutex> lock(_connections_mutex);
    size_t n = 0;
    for (const auto& c : _connections) {
        if (c && c->shard_id == shard_id) {
            ++n;
        }
    }
    return n;
}

void osd_raw_rdma_server::get_io_totals(uint64_t* recv_total,
                                        uint64_t* send_total,
                                        uint64_t* error_total) const noexcept {
    uint64_t r = 0;
    uint64_t s = 0;
    uint64_t e = 0;
    {
        std::lock_guard<std::mutex> lock(_connections_mutex);
        for (const auto& c : _connections) {
            if (!c) {
                continue;
            }
            r += c->recv_count.load(std::memory_order_relaxed);
            s += c->send_count.load(std::memory_order_relaxed);
            e += c->error_count.load(std::memory_order_relaxed);
        }
    }
    if (recv_total) {
        *recv_total = r;
    }
    if (send_total) {
        *send_total = s;
    }
    if (error_total) {
        *error_total = e;
    }
}

std::vector<uint16_t> osd_raw_rdma_server::listen_ports() const {
    std::vector<uint16_t> ports;
    ports.reserve(_listeners.size());
    for (const auto& listener : _listeners) {
        ports.push_back(listener ? listener->port : 0);
    }
    return ports;
}

size_t osd_raw_rdma_server::max_connection_limit() const noexcept {
    return max_connections;
}

const std::string& osd_raw_rdma_server::bind_address() const noexcept {
    return _bind_address;
}

size_t osd_raw_rdma_server::established_connection_count() const noexcept {
    std::lock_guard<std::mutex> lock(_connections_mutex);
    size_t n = 0;
    for (const auto& c : _connections) {
        if (c && c->established) {
            ++n;
        }
    }
    return n;
}

std::string osd_raw_rdma_server::ports_string() const {
    std::string ports;
    for (size_t i = 0; i < _listeners.size(); ++i) {
        if (i) {
            ports.push_back(',');
        }
        ports += std::to_string(_listeners[i] ? _listeners[i]->port : 0);
    }
    return ports;
}

raw_rdma_server_stats osd_raw_rdma_server::collect_stats() const {
    raw_rdma_server_stats st{};
    st.running = is_running();
    st.shard_count = shard_count();
    st.connection_count = connection_count();
    get_io_totals(&st.recv_total, &st.send_total, &st.error_total);
    st.accept_total = _accept_total.load(std::memory_order_relaxed);
    st.reject_total = _reject_total.load(std::memory_order_relaxed);
    st.dispatch_error_total =
      _dispatch_error_total.load(std::memory_order_relaxed);
    st.listen_ports.reserve(_listeners.size());
    for (size_t i = 0; i < _listeners.size(); ++i) {
        st.listen_ports.push_back(_listeners[i] ? _listeners[i]->port : 0);
    }
    return st;
}
