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
#include "raw_tcp_server.h"

#include "osd_service.h"
#include "fastblock/utils/err_num.h"

#include <spdk/log.h>

#include <algorithm>
#include <arpa/inet.h>
#include <endian.h>
#include <fcntl.h>
#include <google/protobuf/stubs/callback.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

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

struct osd_raw_tcp_inflight_response {
    raw_header request_header{};
    std::vector<uint8_t> response_body{};
    std::atomic<bool> detached{false};
    std::atomic<bool> completed{false};
    uint32_t status{0};
};

struct osd_raw_tcp_connection_state {
    int fd{-1};
    std::atomic<bool> stopping{false};
    std::atomic<bool> reader_done{false};
    std::atomic<uint64_t> requests_received{0};
    std::atomic<uint64_t> responses_ready{0};
    std::atomic<uint64_t> responses_sent{0};
    std::atomic<uint64_t> responses_reordered{0};
    std::atomic<uint64_t> callbacks_dropped{0};
    std::atomic<uint64_t> protocol_errors{0};
    std::mutex mutex{};
    std::condition_variable cv{};
    size_t peak_pending{0};
    std::deque<std::shared_ptr<osd_raw_tcp_inflight_response>> pending{};
    std::deque<std::shared_ptr<osd_raw_tcp_inflight_response>> ready{};
    std::unordered_map<uint64_t, std::shared_ptr<osd_raw_tcp_inflight_response>> inflight_by_seq{};
};

namespace {

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

constexpr int raw_backlog = 128;
constexpr int raw_poll_timeout_ms = 1000;
constexpr uint16_t min_raw_port = 10001U;
constexpr uint16_t max_raw_port = 19999U;
constexpr size_t max_raw_body_len = (4U * 1024U * 1024U) + 1024U;
constexpr size_t raw_max_inflight_requests = 128U;
constexpr uint64_t raw_connection_summary_interval = 64U;

class raw_async_done : public google::protobuf::Closure {
public:
    explicit raw_async_done(std::function<void()> fn)
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

uint16_t random_raw_port() {
    thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(min_raw_port, max_raw_port);
    return static_cast<uint16_t>(dist(gen));
}

int raw_status_from_errno(const int state) noexcept {
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

bool validate_request_header(const raw_header& hdr) noexcept {
    return le32toh(hdr.magic) == raw_magic
      && hdr.version_major == raw_version_major
      && hdr.version_minor == raw_version_minor
      && hdr.service == raw_service_osd
      && (le32toh(hdr.flags) & raw_flag_response) == 0
      && le32toh(hdr.status) == 0
      && le32toh(hdr.body_len) <= max_raw_body_len;
}

bool wait_for_fd(const int fd, const short events, const std::atomic<bool>& running) noexcept {
    while (running.load(std::memory_order_acquire)) {
        ::pollfd pfd{};
        pfd.fd = fd;
        pfd.events = events;
        auto rc = ::poll(&pfd, 1, raw_poll_timeout_ms);
        if (rc > 0) {
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return false;
            }
            return (pfd.revents & events) != 0;
        }
        if (rc == 0) {
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }

    return false;
}

bool recv_all(int fd, void* buf, size_t len, const std::atomic<bool>& running) noexcept {
    auto* cursor = reinterpret_cast<uint8_t*>(buf);
    size_t received = 0;

    while (received < len) {
        if (!wait_for_fd(fd, POLLIN, running)) {
            return false;
        }
        auto rc = ::recv(fd, cursor + received, len - received, 0);
        if (rc > 0) {
            received += static_cast<size_t>(rc);
            continue;
        }
        if (rc == 0) {
            return false;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        return false;
    }

    return true;
}

bool send_all(int fd, const void* buf, size_t len, const std::atomic<bool>& running) noexcept {
    auto* cursor = reinterpret_cast<const uint8_t*>(buf);
    size_t sent = 0;

    while (sent < len) {
        if (!wait_for_fd(fd, POLLOUT, running)) {
            return false;
        }
        auto rc = ::send(fd, cursor + sent, len - sent, MSG_NOSIGNAL);
        if (rc > 0) {
            sent += static_cast<size_t>(rc);
            continue;
        }
        if (rc == 0) {
            return false;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        return false;
    }

    return true;
}

bool set_nonblocking(int fd) noexcept {
    int flags;

    if (fd < 0) {
        return false;
    }

    flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return true;
    }

    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int fill_connection_frame(int fd, uint8_t* buf, size_t target, size_t* completed) {
    if (fd < 0 || !buf || !completed) {
        return -EINVAL;
    }

    while (*completed < target) {
        auto rc = ::recv(fd, buf + *completed, target - *completed, 0);

        if (rc > 0) {
            *completed += static_cast<size_t>(rc);
            continue;
        }
        if (rc == 0) {
            return -ECONNRESET;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -errno ? -errno : -EIO;
    }

    return 1;
}

bool write_message(int fd,
                   const raw_header& hdr,
                   const void* body,
                   size_t body_len,
                   const std::atomic<bool>& running) noexcept {
    raw_header rsp = hdr;
    rsp.magic = htole32(raw_magic);
    rsp.version_major = raw_version_major;
    rsp.version_minor = raw_version_minor;
    rsp.service = raw_service_osd;
    rsp.flags = htole32(le32toh(hdr.flags) | raw_flag_response);
    rsp.body_len = htole32(static_cast<uint32_t>(body_len));

    if (!send_all(fd, &rsp, sizeof(rsp), running)) {
        return false;
    }
    if (body_len == 0) {
        return true;
    }

    return send_all(fd, body, body_len, running);
}

bool write_error_response(int fd,
                          const raw_header& req_hdr,
                          const uint32_t status,
                          const std::atomic<bool>& running) noexcept {
    raw_header rsp = req_hdr;
    rsp.status = htole32(status);
    return write_message(fd, rsp, nullptr, 0, running);
}

int create_listener(const std::string& bind_address, uint16_t* port) noexcept {
    if (!port) {
        return -EINVAL;
    }

    ::sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!bind_address.empty()) {
        if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
            return -EINVAL;
        }
    }

    for (int attempt = 0; attempt < 256; ++attempt) {
        auto candidate = random_raw_port();
        auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -errno;
        }

        int reuse = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            auto saved_errno = errno;
            ::close(fd);
            return -saved_errno;
        }
        if (!set_nonblocking(fd)) {
            auto saved_errno = errno;
            ::close(fd);
            return saved_errno ? -saved_errno : -EIO;
        }

        addr.sin_port = htons(candidate);
        if (::bind(fd, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr)) == 0) {
            if (::listen(fd, raw_backlog) == 0) {
                *port = candidate;
                return fd;
            }
        }

        ::close(fd);
    }

    return -EADDRINUSE;
}

struct raw_read_context {
    std::shared_ptr<osd_raw_tcp_connection_state> connection{};
    std::shared_ptr<osd_raw_tcp_inflight_response> inflight{};
    osd::read_request request{};
    osd::read_reply response{};
};

struct raw_write_context {
    std::shared_ptr<osd_raw_tcp_connection_state> connection{};
    std::shared_ptr<osd_raw_tcp_inflight_response> inflight{};
    osd::write_request request{};
    osd::write_reply response{};
};

struct raw_delete_context {
    std::shared_ptr<osd_raw_tcp_connection_state> connection{};
    std::shared_ptr<osd_raw_tcp_inflight_response> inflight{};
    osd::delete_request request{};
    osd::delete_reply response{};
};

void notify_connection(const std::shared_ptr<osd_raw_tcp_connection_state>& connection) {
    if (connection) {
        connection->cv.notify_all();
    }
}

void stop_connection(const std::shared_ptr<osd_raw_tcp_connection_state>& connection) {
    if (!connection) {
        return;
    }

    connection->stopping.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(connection->mutex);
        for (auto& entry : connection->inflight_by_seq) {
            if (entry.second) {
                entry.second->detached.store(true, std::memory_order_release);
            }
        }
        connection->inflight_by_seq.clear();
        connection->pending.clear();
        connection->ready.clear();
    }
    notify_connection(connection);
}

bool enqueue_inflight_response(
  const std::shared_ptr<osd_raw_tcp_connection_state>& connection,
  const std::shared_ptr<osd_raw_tcp_inflight_response>& inflight) {
    const auto seq = inflight ? le64toh(inflight->request_header.seq) : 0;

    if (!connection || !inflight) {
        return false;
    }
    if (seq == 0) {
        return false;
    }

    std::unique_lock<std::mutex> lk(connection->mutex);
    connection->cv.wait(lk, [&connection]() {
        return connection->stopping.load(std::memory_order_acquire) ||
               connection->pending.size() < raw_max_inflight_requests;
    });
    if (connection->stopping.load(std::memory_order_acquire)) {
        return false;
    }
    if (connection->inflight_by_seq.find(seq) != connection->inflight_by_seq.end()) {
        connection->protocol_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    connection->pending.push_back(inflight);
    connection->inflight_by_seq.emplace(seq, inflight);
    connection->requests_received.fetch_add(1, std::memory_order_relaxed);
    if (connection->pending.size() > connection->peak_pending) {
        connection->peak_pending = connection->pending.size();
    }
    lk.unlock();
    notify_connection(connection);
    return true;
}

void complete_inflight_response(
  const std::shared_ptr<osd_raw_tcp_connection_state>& connection,
  const std::shared_ptr<osd_raw_tcp_inflight_response>& inflight,
  const uint32_t status,
  std::vector<uint8_t> response_body = {}) {
    if (!connection || !inflight) {
        return;
    }
    if (inflight->detached.load(std::memory_order_acquire) ||
        connection->stopping.load(std::memory_order_acquire) ||
        inflight->completed.load(std::memory_order_acquire)) {
        connection->callbacks_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(connection->mutex);
        const auto seq = le64toh(inflight->request_header.seq);
        auto it = connection->inflight_by_seq.find(seq);

        if (it == connection->inflight_by_seq.end() ||
            it->second.get() != inflight.get()) {
            connection->callbacks_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (inflight->detached.load(std::memory_order_acquire) ||
            connection->stopping.load(std::memory_order_acquire) ||
            inflight->completed.load(std::memory_order_acquire)) {
            connection->callbacks_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        inflight->status = status;
        inflight->response_body = std::move(response_body);
        inflight->completed.store(true, std::memory_order_release);
        connection->ready.push_back(inflight);
    }
    connection->responses_ready.fetch_add(1, std::memory_order_relaxed);
    notify_connection(connection);
}

std::vector<uint8_t> build_leader_response_body(const osd_service::leader_endpoint& leader) {
    raw_get_leader_rsp rsp{};
    std::vector<uint8_t> body(sizeof(rsp) + leader.leader_addr.size());

    rsp.leader_id = htole32(static_cast<uint32_t>(leader.leader_id));
    rsp.leader_port = htole16(static_cast<uint16_t>(leader.leader_port));
    rsp.address_len = htole16(static_cast<uint16_t>(leader.leader_addr.size()));
    std::memcpy(body.data(), &rsp, sizeof(rsp));
    if (!leader.leader_addr.empty()) {
        std::memcpy(body.data() + sizeof(rsp),
                    leader.leader_addr.data(),
                    leader.leader_addr.size());
    }

    return body;
}

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

void dispatch_get_leader_request(
  osd_service* service,
  const std::shared_ptr<osd_raw_tcp_connection_state>& connection,
  const std::shared_ptr<osd_raw_tcp_inflight_response>& inflight,
  const std::vector<uint8_t>& body) {
    raw_get_leader_req req{};
    osd_service::leader_endpoint leader{};

    if (!service || !inflight) {
        complete_inflight_response(connection, inflight, raw_status_internal_error);
        return;
    }
    if (body.size() != sizeof(req)) {
        complete_inflight_response(connection, inflight, raw_status_invalid_request);
        return;
    }

    std::memcpy(&req, body.data(), sizeof(req));
    leader = service->resolve_pg_leader(le32toh(req.pool_id), le32toh(req.pg_id), true);
    if (leader.state != err::E_SUCCESS) {
        complete_inflight_response(
          connection,
          inflight,
          static_cast<uint32_t>(raw_status_from_errno(leader.state)));
        return;
    }
    if (leader.leader_port <= 0 ||
        leader.leader_port > std::numeric_limits<uint16_t>::max() ||
        leader.leader_id < 0 ||
        leader.leader_addr.size() > std::numeric_limits<uint16_t>::max()) {
        complete_inflight_response(connection, inflight, raw_status_internal_error);
        return;
    }

    complete_inflight_response(connection,
                               inflight,
                               raw_status_ok,
                               build_leader_response_body(leader));
}

void dispatch_read_request(
  osd_service* service,
  const std::shared_ptr<osd_raw_tcp_connection_state>& connection,
  const std::shared_ptr<osd_raw_tcp_inflight_response>& inflight,
  const std::vector<uint8_t>& body) {
    raw_read_object_req req{};
    auto ctx = std::make_shared<raw_read_context>();
    uint16_t object_name_len{};

    if (!service || !inflight) {
        complete_inflight_response(connection, inflight, raw_status_internal_error);
        return;
    }
    if (body.size() < sizeof(req)) {
        complete_inflight_response(connection, inflight, raw_status_invalid_request);
        return;
    }

    std::memcpy(&req, body.data(), sizeof(req));
    object_name_len = le16toh(req.object_name_len);
    if (body.size() != sizeof(req) + object_name_len || object_name_len == 0) {
        complete_inflight_response(connection, inflight, raw_status_invalid_request);
        return;
    }

    ctx->connection = connection;
    ctx->inflight = inflight;
    ctx->request.set_pool_id(le32toh(req.pool_id));
    ctx->request.set_pg_id(le32toh(req.pg_id));
    ctx->request.set_offset(le64toh(req.offset));
    ctx->request.set_length(le32toh(req.length));
    ctx->request.set_object_name(
      reinterpret_cast<const char*>(body.data() + sizeof(req)),
      object_name_len);
    service->process_read(
      nullptr,
      &ctx->request,
      &ctx->response,
      new raw_async_done([ctx]() {
          const auto state = ctx->response.state();

          if (state != err::E_SUCCESS) {
              complete_inflight_response(
                ctx->connection,
                ctx->inflight,
                static_cast<uint32_t>(raw_status_from_errno(state)));
              return;
          }
          if (ctx->response.data().size() >
              max_raw_body_len - sizeof(raw_read_object_rsp)) {
              complete_inflight_response(
                ctx->connection,
                ctx->inflight,
                raw_status_internal_error);
              return;
          }

          complete_inflight_response(ctx->connection,
                                     ctx->inflight,
                                     raw_status_ok,
                                     build_read_response_body(ctx->response.data()));
      }));
}

void dispatch_write_request(
  osd_service* service,
  const std::shared_ptr<osd_raw_tcp_connection_state>& connection,
  const std::shared_ptr<osd_raw_tcp_inflight_response>& inflight,
  const std::vector<uint8_t>& body) {
    raw_write_object_req req{};
    auto ctx = std::make_shared<raw_write_context>();
    uint16_t object_name_len{};
    uint32_t data_len{};

    if (!service || !inflight) {
        complete_inflight_response(connection, inflight, raw_status_internal_error);
        return;
    }
    if (body.size() < sizeof(req)) {
        complete_inflight_response(connection, inflight, raw_status_invalid_request);
        return;
    }

    std::memcpy(&req, body.data(), sizeof(req));
    object_name_len = le16toh(req.object_name_len);
    data_len = le32toh(req.data_len);
    if (body.size() != sizeof(req) + object_name_len + data_len ||
        object_name_len == 0) {
        complete_inflight_response(connection, inflight, raw_status_invalid_request);
        return;
    }

    ctx->connection = connection;
    ctx->inflight = inflight;
    ctx->request.set_pool_id(le32toh(req.pool_id));
    ctx->request.set_pg_id(le32toh(req.pg_id));
    ctx->request.set_offset(le64toh(req.offset));
    ctx->request.set_object_name(
      reinterpret_cast<const char*>(body.data() + sizeof(req)),
      object_name_len);
    ctx->request.set_data(
      reinterpret_cast<const char*>(body.data() + sizeof(req) + object_name_len),
      data_len);
    service->process_write(
      nullptr,
      &ctx->request,
      &ctx->response,
      new raw_async_done([ctx]() {
          complete_inflight_response(
            ctx->connection,
            ctx->inflight,
            static_cast<uint32_t>(raw_status_from_errno(ctx->response.state())));
      }));
}

void dispatch_delete_request(
  osd_service* service,
  const std::shared_ptr<osd_raw_tcp_connection_state>& connection,
  const std::shared_ptr<osd_raw_tcp_inflight_response>& inflight,
  const std::vector<uint8_t>& body) {
    raw_delete_object_req req{};
    auto ctx = std::make_shared<raw_delete_context>();
    uint16_t object_name_len{};

    if (!service || !inflight) {
        complete_inflight_response(connection, inflight, raw_status_internal_error);
        return;
    }
    if (body.size() < sizeof(req)) {
        complete_inflight_response(connection, inflight, raw_status_invalid_request);
        return;
    }

    std::memcpy(&req, body.data(), sizeof(req));
    object_name_len = le16toh(req.object_name_len);
    if (body.size() != sizeof(req) + object_name_len || object_name_len == 0) {
        complete_inflight_response(connection, inflight, raw_status_invalid_request);
        return;
    }

    ctx->connection = connection;
    ctx->inflight = inflight;
    ctx->request.set_pool_id(le32toh(req.pool_id));
    ctx->request.set_pg_id(le32toh(req.pg_id));
    ctx->request.set_object_name(
      reinterpret_cast<const char*>(body.data() + sizeof(req)),
      object_name_len);
    service->process_delete(
      nullptr,
      &ctx->request,
      &ctx->response,
      new raw_async_done([ctx]() {
          complete_inflight_response(
            ctx->connection,
            ctx->inflight,
            static_cast<uint32_t>(raw_status_from_errno(ctx->response.state())));
      }));
}

void dispatch_request(
  osd_service* service,
  const std::shared_ptr<osd_raw_tcp_connection_state>& connection,
  const std::shared_ptr<osd_raw_tcp_inflight_response>& inflight,
  const std::vector<uint8_t>& body) {
    if (!inflight) {
        return;
    }
    if (!validate_request_header(inflight->request_header)) {
        connection->protocol_errors.fetch_add(1, std::memory_order_relaxed);
        complete_inflight_response(connection, inflight, raw_status_invalid_request);
        return;
    }

    switch (inflight->request_header.opcode) {
    case raw_op_get_leader:
        dispatch_get_leader_request(service, connection, inflight, body);
        return;
    case raw_op_read_object:
        dispatch_read_request(service, connection, inflight, body);
        return;
    case raw_op_write_object:
        dispatch_write_request(service, connection, inflight, body);
        return;
    case raw_op_delete_object:
        dispatch_delete_request(service, connection, inflight, body);
        return;
    default:
        complete_inflight_response(connection, inflight, raw_status_invalid_request);
        return;
    }
}

} // namespace

void osd_raw_tcp_server::reset_connection_frame(connection_context* conn) noexcept {
    if (!conn) {
        return;
    }

    conn->recv_frame.clear();
    conn->recv_header_bytes = 0;
    conn->recv_body_bytes = 0;
    conn->recv_target_body_bytes = 0;
}

int osd_raw_tcp_server::try_receive_one_request(
  connection_context* conn,
  raw_header* hdr_out,
  std::vector<uint8_t>* body_out) noexcept {
    raw_header hdr{};
    int ret;

    if (!conn || !hdr_out || !body_out) {
        return -EINVAL;
    }

    if (conn->recv_frame.empty()) {
        conn->recv_frame.resize(sizeof(raw_header));
    }

    if (conn->recv_header_bytes < sizeof(raw_header)) {
        ret = fill_connection_frame(conn->fd,
                                    conn->recv_frame.data(),
                                    sizeof(raw_header),
                                    &conn->recv_header_bytes);
        if (ret <= 0) {
            return ret;
        }
        std::memcpy(&hdr, conn->recv_frame.data(), sizeof(hdr));
        if (!validate_request_header(hdr)) {
            reset_connection_frame(conn);
            return -EPROTO;
        }
        conn->recv_target_body_bytes = le32toh(hdr.body_len);
        conn->recv_frame.resize(sizeof(raw_header) + conn->recv_target_body_bytes);
        if (conn->recv_target_body_bytes == 0) {
            *hdr_out = hdr;
            body_out->clear();
            reset_connection_frame(conn);
            return 1;
        }
    }

    ret = fill_connection_frame(conn->fd,
                                conn->recv_frame.data() + sizeof(raw_header),
                                conn->recv_target_body_bytes,
                                &conn->recv_body_bytes);
    if (ret <= 0) {
        return ret;
    }

    std::memcpy(&hdr, conn->recv_frame.data(), sizeof(hdr));
    *hdr_out = hdr;
    body_out->assign(conn->recv_frame.begin() + sizeof(raw_header),
                     conn->recv_frame.end());
    reset_connection_frame(conn);
    return 1;
}

osd_raw_tcp_server::osd_raw_tcp_server(osd_service* service)
  : _service(service) {}

osd_raw_tcp_server::~osd_raw_tcp_server() noexcept {
    stop();
}

bool osd_raw_tcp_server::start(const std::string& bind_address, const uint32_t shard_count) {
    if (!_service) {
        return false;
    }
    if (_running.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }

    _bind_address = bind_address;
    _listeners.clear();
    _listeners.resize(shard_count);
    for (uint32_t shard_id = 0; shard_id < shard_count; ++shard_id) {
        if (start_listener(shard_id)) {
            continue;
        }

        stop();
        return false;
    }

    return true;
}

void osd_raw_tcp_server::stop() noexcept {
    const auto was_running = _running.exchange(false, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lk(_connections_mutex);
        for (auto& conn : _connections) {
            stop_connection(conn->state);
            if (conn->fd >= 0) {
                ::shutdown(conn->fd, SHUT_RDWR);
                ::close(conn->fd);
                conn->fd = -1;
            }
        }
    }
    for (auto& listener : _listeners) {
        if (listener.fd >= 0) {
            ::shutdown(listener.fd, SHUT_RDWR);
            ::close(listener.fd);
        }
    }
    if (!was_running) {
        return;
    }

    for (auto& listener : _listeners) {
        if (listener.worker.joinable()) {
            listener.worker.join();
        }
        listener.fd = -1;
        listener.port = 0;
    }

    {
        std::lock_guard<std::mutex> lk(_connections_mutex);
        _connections.clear();
    }
}

uint16_t osd_raw_tcp_server::listen_port(const uint32_t shard_id) const noexcept {
    if (shard_id >= _listeners.size()) {
        return 0;
    }

    return _listeners[shard_id].port;
}

bool osd_raw_tcp_server::start_listener(const uint32_t shard_id) {
    auto& listener = _listeners.at(shard_id);
    auto fd = create_listener(_bind_address, &listener.port);
    if (fd < 0) {
        SPDK_ERRLOG(
          "ERROR: create raw TCP listener on shard %u failed: %s\n",
          shard_id,
          std::strerror(-fd));
        return false;
    }

    listener.fd = fd;
    try {
        listener.worker = std::thread([this, shard_id]() {
            run_listener(shard_id);
        });
    } catch (const std::exception& e) {
        SPDK_ERRLOG(
          "ERROR: create raw TCP worker on shard %u failed: %s\n",
          shard_id,
          e.what());
        ::close(listener.fd);
        listener.fd = -1;
        listener.port = 0;
        return false;
    }
    SPDK_NOTICELOG(
      "Started raw TCP server on shard %u, listen %s:%u\n",
      shard_id,
      _bind_address.c_str(),
      listener.port);
    return true;
}

void osd_raw_tcp_server::cleanup_finished_connections() noexcept {
    std::lock_guard<std::mutex> lk(_connections_mutex);

    for (auto it = _connections.begin(); it != _connections.end();) {
        if (!(*it)->done.load(std::memory_order_acquire)) {
            ++it;
            continue;
        }

        if ((*it)->writer.joinable()) {
            (*it)->writer.join();
        }
        (*it)->state.reset();
        it = _connections.erase(it);
    }
}

void osd_raw_tcp_server::log_connection_summary(const uint32_t shard_id) noexcept {
    uint64_t requests = 0;
    uint64_t ready = 0;
    uint64_t sent = 0;
    uint64_t reordered = 0;
    size_t active = 0;
    size_t inflight = 0;
    size_t pending = 0;
    size_t ready_depth = 0;

    std::lock_guard<std::mutex> lk(_connections_mutex);
    for (const auto& conn : _connections) {
        if (!conn || conn->done.load(std::memory_order_acquire) || !conn->state) {
            continue;
        }
        active++;
        requests += conn->state->requests_received.load(std::memory_order_relaxed);
        ready += conn->state->responses_ready.load(std::memory_order_relaxed);
        sent += conn->state->responses_sent.load(std::memory_order_relaxed);
        reordered += conn->state->responses_reordered.load(std::memory_order_relaxed);
        inflight += conn->state->inflight_by_seq.size();
        pending += conn->state->pending.size();
        ready_depth += conn->state->ready.size();
    }

    if (!active) {
        return;
    }

    SPDK_NOTICELOG(
      "raw TCP shard %u summary: active=%zu inflight=%zu pending=%zu ready=%zu recv=%llu ready_rsp=%llu sent=%llu reordered=%llu\n",
      shard_id,
      active,
      inflight,
      pending,
      ready_depth,
      static_cast<unsigned long long>(requests),
      static_cast<unsigned long long>(ready),
      static_cast<unsigned long long>(sent),
      static_cast<unsigned long long>(reordered));
}

bool osd_raw_tcp_server::start_connection(
  const int client_fd,
  const sockaddr_in& peer_addr,
  const uint32_t shard_id) noexcept {
    auto conn = std::make_unique<connection_context>();
    char peer_host[INET_ADDRSTRLEN] = {};

    if (!set_nonblocking(client_fd)) {
        SPDK_ERRLOG(
          "ERROR: set raw TCP connection nonblocking on shard %u failed: %s\n",
          shard_id,
          std::strerror(errno));
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        return false;
    }

    conn->fd = client_fd;
    conn->shard_id = shard_id;
    conn->state = std::make_shared<osd_raw_tcp_connection_state>();
    conn->state->fd = client_fd;
    if (::inet_ntop(AF_INET, &peer_addr.sin_addr, peer_host, sizeof(peer_host))) {
        conn->peer_address = peer_host;
    }
    try {
        conn->writer = std::thread([this, shard_id, state = conn->state]() {
            write_connection_responses(state, shard_id);
        });
    } catch (const std::exception& e) {
        SPDK_ERRLOG(
          "ERROR: create raw TCP writer on shard %u failed: %s\n",
          shard_id,
          e.what());
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        return false;
    }

    SPDK_NOTICELOG("Accepted raw TCP connection on shard %u peer=%s fd=%d\n",
                   shard_id,
                   conn->peer_address.empty() ? "<unknown>" :
                     conn->peer_address.c_str(),
                   client_fd);
    {
        std::lock_guard<std::mutex> lk(_connections_mutex);
        _connections.push_back(std::move(conn));
    }
    return true;
}

void osd_raw_tcp_server::run_listener(const uint32_t shard_id) noexcept {
    auto& listener = _listeners.at(shard_id);
    uint64_t accept_count = 0;

    while (_running.load(std::memory_order_acquire)) {
        std::vector<::pollfd> pfds{};
        std::vector<connection_context*> conns{};
        int rc;

        cleanup_finished_connections();
        pfds.push_back({.fd = listener.fd, .events = POLLIN, .revents = 0});
        {
            std::lock_guard<std::mutex> lk(_connections_mutex);
            for (const auto& conn : _connections) {
                if (!conn || conn->done.load(std::memory_order_acquire) ||
                    conn->fd < 0 || conn->shard_id != shard_id) {
                    continue;
                }
                conns.push_back(conn.get());
                pfds.push_back({.fd = conn->fd,
                                .events = static_cast<short>(POLLIN | POLLERR |
                                                              POLLHUP | POLLNVAL),
                                .revents = 0});
            }
        }

        rc = ::poll(pfds.data(), pfds.size(), raw_poll_timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (_running.load(std::memory_order_acquire)) {
                SPDK_ERRLOG(
                  "ERROR: poll raw TCP loop on shard %u failed: %s\n",
                  shard_id,
                  std::strerror(errno));
            }
            continue;
        }
        if (rc == 0) {
            continue;
        }

        if ((pfds[0].revents & POLLIN) != 0) {
            ::sockaddr_in peer_addr{};
            ::socklen_t peer_len = sizeof(peer_addr);
            auto client_fd = ::accept(listener.fd,
                                      reinterpret_cast<::sockaddr*>(&peer_addr),
                                      &peer_len);
            for (;;) {
                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK &&
                        errno != EINTR && _running.load(std::memory_order_acquire)) {
                        SPDK_ERRLOG(
                          "ERROR: drain raw TCP accept queue on shard %u failed: %s\n",
                          shard_id,
                          std::strerror(errno));
                    }
                    break;
                }
                if (!start_connection(client_fd, peer_addr, shard_id)) {
                    break;
                }
                accept_count++;
                if (accept_count % raw_connection_summary_interval == 0) {
                    log_connection_summary(shard_id);
                }

                peer_addr = {};
                peer_len = sizeof(peer_addr);
                client_fd = ::accept(listener.fd,
                                     reinterpret_cast<::sockaddr*>(&peer_addr),
                                     &peer_len);
            }
        }

        for (size_t i = 1; i < pfds.size(); ++i) {
            if (pfds[i].revents == 0) {
                continue;
            }
            service_connection_io(conns[i - 1], pfds[i].revents);
        }
    }

    cleanup_finished_connections();
}

void osd_raw_tcp_server::close_connection(connection_context *conn) noexcept {
    auto state = std::shared_ptr<osd_raw_tcp_connection_state>{};

    if (!conn) {
        return;
    }

    state = conn->state;
    if (state) {
        state->reader_done.store(true, std::memory_order_release);
        stop_connection(state);
    }
    if (conn->fd >= 0) {
        ::shutdown(conn->fd, SHUT_RDWR);
        ::close(conn->fd);
        conn->fd = -1;
    }
    conn->done.store(true, std::memory_order_release);
    if (!state) {
        return;
    }

    SPDK_NOTICELOG(
      "Closed raw TCP connection on shard %u: recv=%llu ready=%llu sent=%llu reordered=%llu callbacks_dropped=%llu protocol_errors=%llu peak_pending=%zu pending_left=%zu ready_left=%zu\n",
      conn->shard_id,
      static_cast<unsigned long long>(state->requests_received.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(state->responses_ready.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(state->responses_sent.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(state->responses_reordered.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(state->callbacks_dropped.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(state->protocol_errors.load(std::memory_order_relaxed)),
      state->peak_pending,
      state->pending.size(),
      state->ready.size());
}

void osd_raw_tcp_server::service_connection_io(connection_context *conn,
                                               const short revents) noexcept {
    if (!conn || conn->done.load(std::memory_order_acquire) || conn->fd < 0 ||
        !conn->state) {
        return;
    }
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        close_connection(conn);
        return;
    }
    if ((revents & POLLIN) == 0) {
        return;
    }

    for (;;) {
        raw_header req_hdr{};
        std::shared_ptr<osd_raw_tcp_inflight_response> inflight{};
        std::vector<uint8_t> body{};
        int recv_ret = try_receive_one_request(conn, &req_hdr, &body);

        if (recv_ret < 0) {
            conn->state->protocol_errors.fetch_add(1, std::memory_order_relaxed);
            close_connection(conn);
            return;
        }
        if (recv_ret == 0) {
            break;
        }

        inflight = std::make_shared<osd_raw_tcp_inflight_response>();
        inflight->request_header = req_hdr;
        inflight->status = raw_status_internal_error;
        if (!enqueue_inflight_response(conn->state, inflight)) {
            close_connection(conn);
            return;
        }

        dispatch_request(_service, conn->state, inflight, body);
    }
}

void osd_raw_tcp_server::write_connection_responses(
  std::shared_ptr<osd_raw_tcp_connection_state> state,
  const uint32_t shard_id) noexcept {
    if (!state) {
        return;
    }

    while (_running.load(std::memory_order_acquire)) {
        std::shared_ptr<osd_raw_tcp_inflight_response> inflight{};
        bool reordered = false;

        {
            std::unique_lock<std::mutex> lk(state->mutex);
            state->cv.wait(lk, [&state]() {
                return state->stopping.load(std::memory_order_acquire) ||
                       (state->reader_done.load(std::memory_order_acquire) &&
                        state->pending.empty()) ||
                       !state->ready.empty();
            });
            if (state->stopping.load(std::memory_order_acquire)) {
                break;
            }
            if (state->reader_done.load(std::memory_order_acquire) &&
                state->pending.empty()) {
                break;
            }
            if (state->ready.empty()) {
                continue;
            }

            inflight = state->ready.front();
            state->ready.pop_front();
            auto it = std::find(state->pending.begin(), state->pending.end(),
                                inflight);
            if (it == state->pending.end()) {
                continue;
            }
            state->inflight_by_seq.erase(le64toh(inflight->request_header.seq));
            reordered = it != state->pending.begin();
            state->pending.erase(it);
        }
        notify_connection(state);

        inflight->request_header.status = htole32(inflight->status);
        if (!write_message(state->fd,
                           inflight->request_header,
                           inflight->response_body.data(),
                           inflight->response_body.size(),
                           _running)) {
            stop_connection(state);
            break;
        }
        state->responses_sent.fetch_add(1, std::memory_order_relaxed);
        if (reordered) {
            state->responses_reordered.fetch_add(1, std::memory_order_relaxed);
        }
        if (state->responses_sent.load(std::memory_order_relaxed) %
            raw_connection_summary_interval == 0) {
            log_connection_summary(shard_id);
        }
    }

    SPDK_NOTICELOG("Stopped raw TCP writer on shard %u\n", shard_id);
}
