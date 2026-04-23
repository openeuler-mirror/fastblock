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

#include <arpa/inet.h>
#include <endian.h>
#include <google/protobuf/stubs/callback.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <vector>

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
constexpr size_t max_raw_body_len = 4096;

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

class blocking_done : public google::protobuf::Closure {
public:
    void Run() override {
        std::lock_guard<std::mutex> lk(_mutex);
        _done = true;
        _cv.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lk(_mutex);
        _cv.wait(lk, [this]() {
            return _done;
        });
    }

private:
    std::mutex _mutex{};
    std::condition_variable _cv{};
    bool _done{false};
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
        if (errno == EINTR) {
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
        if (errno == EINTR) {
            continue;
        }
        return false;
    }

    return true;
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

int execute_read(osd_service* service,
                 uint32_t pool_id,
                 uint32_t pg_id,
                 const std::string& object_name,
                 uint64_t offset,
                 uint32_t length,
                 std::string* data) {
    osd::read_request req{};
    osd::read_reply rsp{};
    blocking_done done{};

    req.set_pool_id(pool_id);
    req.set_pg_id(pg_id);
    req.set_object_name(object_name);
    req.set_offset(offset);
    req.set_length(length);
    service->process_read(nullptr, &req, &rsp, &done);
    done.wait();
    if (rsp.state() == err::E_SUCCESS && data) {
        *data = rsp.data();
    }
    return rsp.state();
}

int execute_write(osd_service* service,
                  uint32_t pool_id,
                  uint32_t pg_id,
                  const std::string& object_name,
                  uint64_t offset,
                  const std::string& data) {
    osd::write_request req{};
    osd::write_reply rsp{};
    blocking_done done{};

    req.set_pool_id(pool_id);
    req.set_pg_id(pg_id);
    req.set_object_name(object_name);
    req.set_offset(offset);
    req.set_data(data);
    service->process_write(nullptr, &req, &rsp, &done);
    done.wait();
    return rsp.state();
}

int execute_delete(osd_service* service,
                   uint32_t pool_id,
                   uint32_t pg_id,
                   const std::string& object_name) {
    osd::delete_request req{};
    osd::delete_reply rsp{};
    blocking_done done{};

    req.set_pool_id(pool_id);
    req.set_pg_id(pg_id);
    req.set_object_name(object_name);
    service->process_delete(nullptr, &req, &rsp, &done);
    done.wait();
    return rsp.state();
}

} // namespace

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
        for (auto& conn : _connections) {
            if (conn->worker.joinable()) {
                conn->worker.join();
            }
        }
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

        if ((*it)->worker.joinable()) {
            (*it)->worker.join();
        }
        it = _connections.erase(it);
    }
}

void osd_raw_tcp_server::run_listener(const uint32_t shard_id) noexcept {
    auto& listener = _listeners.at(shard_id);
    while (_running.load(std::memory_order_acquire)) {
        if (!wait_for_fd(listener.fd, POLLIN, _running)) {
            continue;
        }

        cleanup_finished_connections();

        ::sockaddr_in peer_addr{};
        ::socklen_t peer_len = sizeof(peer_addr);
        auto client_fd = ::accept(listener.fd,
                                  reinterpret_cast<::sockaddr*>(&peer_addr),
                                  &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (_running.load(std::memory_order_acquire)) {
                SPDK_ERRLOG(
                  "ERROR: accept raw TCP connection on shard %u failed: %s\n",
                  shard_id,
                  std::strerror(errno));
            }
            continue;
        }

        auto conn = std::make_unique<connection_context>();
        conn->fd = client_fd;
        try {
            conn->worker = std::thread([this, shard_id, ctx = conn.get()]() {
                handle_connection(ctx, shard_id);
            });
        } catch (const std::exception& e) {
            SPDK_ERRLOG(
              "ERROR: create raw TCP connection worker on shard %u failed: %s\n",
              shard_id,
              e.what());
            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(_connections_mutex);
            _connections.push_back(std::move(conn));
        }
    }

    cleanup_finished_connections();
}

void osd_raw_tcp_server::handle_connection(connection_context *conn, const uint32_t shard_id) noexcept {
    auto client_fd = conn ? conn->fd : -1;

    if (client_fd < 0) {
        if (conn) {
            conn->done.store(true, std::memory_order_release);
        }
        return;
    }

    while (_running.load(std::memory_order_acquire)) {
        raw_header req_hdr{};
        if (!recv_all(client_fd, &req_hdr, sizeof(req_hdr), _running)) {
            break;
        }

        if (!validate_request_header(req_hdr)) {
            if (!write_error_response(client_fd, req_hdr, raw_status_invalid_request, _running)) {
                break;
            }
            continue;
        }

        const auto body_len = le32toh(req_hdr.body_len);
        std::vector<uint8_t> body(body_len);
        if (body_len != 0 && !recv_all(client_fd, body.data(), body.size(), _running)) {
            break;
        }

        if (req_hdr.opcode != raw_op_get_leader) {
            if (req_hdr.opcode == raw_op_read_object) {
                raw_read_object_req req{};
                raw_read_object_rsp rsp{};
                std::string data{};
                std::string object_name{};
                uint16_t object_name_len{};
                uint32_t length{};
                int state{};

                if (body.size() < sizeof(req)) {
                    if (!write_error_response(client_fd, req_hdr, raw_status_invalid_request, _running)) {
                        break;
                    }
                    continue;
                }

                std::memcpy(&req, body.data(), sizeof(req));
                object_name_len = le16toh(req.object_name_len);
                length = le32toh(req.length);
                if (body.size() != sizeof(req) + object_name_len || object_name_len == 0) {
                    if (!write_error_response(client_fd, req_hdr, raw_status_invalid_request, _running)) {
                        break;
                    }
                    continue;
                }

                object_name.assign(
                  reinterpret_cast<const char*>(body.data() + sizeof(req)),
                  object_name_len);
                state = execute_read(
                  _service,
                  le32toh(req.pool_id),
                  le32toh(req.pg_id),
                  object_name,
                  le64toh(req.offset),
                  length,
                  &data);
                if (state != err::E_SUCCESS) {
                    if (!write_error_response(
                          client_fd,
                          req_hdr,
                          static_cast<uint32_t>(raw_status_from_errno(state)),
                          _running)) {
                        break;
                    }
                    continue;
                }

                rsp.data_len = htole32(static_cast<uint32_t>(data.size()));
                rsp.reserved = 0;
                std::vector<uint8_t> rsp_body(sizeof(rsp) + data.size());
                std::memcpy(rsp_body.data(), &rsp, sizeof(rsp));
                if (!data.empty()) {
                    std::memcpy(rsp_body.data() + sizeof(rsp), data.data(), data.size());
                }
                req_hdr.status = htole32(raw_status_ok);
                if (!write_message(client_fd, req_hdr, rsp_body.data(), rsp_body.size(), _running)) {
                    break;
                }
                continue;
            }

            if (req_hdr.opcode == raw_op_write_object) {
                raw_write_object_req req{};
                std::string object_name{};
                std::string data{};
                uint16_t object_name_len{};
                uint32_t data_len{};
                int state{};

                if (body.size() < sizeof(req)) {
                    if (!write_error_response(client_fd, req_hdr, raw_status_invalid_request, _running)) {
                        break;
                    }
                    continue;
                }

                std::memcpy(&req, body.data(), sizeof(req));
                object_name_len = le16toh(req.object_name_len);
                data_len = le32toh(req.data_len);
                if (body.size() != sizeof(req) + object_name_len + data_len || object_name_len == 0) {
                    if (!write_error_response(client_fd, req_hdr, raw_status_invalid_request, _running)) {
                        break;
                    }
                    continue;
                }

                object_name.assign(
                  reinterpret_cast<const char*>(body.data() + sizeof(req)),
                  object_name_len);
                data.assign(
                  reinterpret_cast<const char*>(body.data() + sizeof(req) + object_name_len),
                  data_len);
                state = execute_write(
                  _service,
                  le32toh(req.pool_id),
                  le32toh(req.pg_id),
                  object_name,
                  le64toh(req.offset),
                  data);
                if (state != err::E_SUCCESS) {
                    if (!write_error_response(
                          client_fd,
                          req_hdr,
                          static_cast<uint32_t>(raw_status_from_errno(state)),
                          _running)) {
                        break;
                    }
                    continue;
                }

                req_hdr.status = htole32(raw_status_ok);
                if (!write_message(client_fd, req_hdr, nullptr, 0, _running)) {
                    break;
                }
                continue;
            }

            if (req_hdr.opcode == raw_op_delete_object) {
                raw_delete_object_req req{};
                std::string object_name{};
                uint16_t object_name_len{};
                int state{};

                if (body.size() < sizeof(req)) {
                    if (!write_error_response(client_fd, req_hdr, raw_status_invalid_request, _running)) {
                        break;
                    }
                    continue;
                }

                std::memcpy(&req, body.data(), sizeof(req));
                object_name_len = le16toh(req.object_name_len);
                if (body.size() != sizeof(req) + object_name_len || object_name_len == 0) {
                    if (!write_error_response(client_fd, req_hdr, raw_status_invalid_request, _running)) {
                        break;
                    }
                    continue;
                }

                object_name.assign(
                  reinterpret_cast<const char*>(body.data() + sizeof(req)),
                  object_name_len);
                state = execute_delete(
                  _service,
                  le32toh(req.pool_id),
                  le32toh(req.pg_id),
                  object_name);
                if (state != err::E_SUCCESS) {
                    if (!write_error_response(
                          client_fd,
                          req_hdr,
                          static_cast<uint32_t>(raw_status_from_errno(state)),
                          _running)) {
                        break;
                    }
                    continue;
                }

                req_hdr.status = htole32(raw_status_ok);
                if (!write_message(client_fd, req_hdr, nullptr, 0, _running)) {
                    break;
                }
                continue;
            }

            if (!write_error_response(client_fd, req_hdr, raw_status_invalid_request, _running)) {
                break;
            }
            continue;
        }

        if (body.size() != sizeof(raw_get_leader_req)) {
            if (!write_error_response(client_fd, req_hdr, raw_status_invalid_request, _running)) {
                break;
            }
            continue;
        }

        raw_get_leader_req req{};
        std::memcpy(&req, body.data(), sizeof(req));
        auto leader = _service->resolve_pg_leader(le32toh(req.pool_id), le32toh(req.pg_id), true);
        if (leader.state != err::E_SUCCESS) {
            if (!write_error_response(
                  client_fd,
                  req_hdr,
                  static_cast<uint32_t>(raw_status_from_errno(leader.state)),
                  _running)) {
                break;
            }
            continue;
        }

        if (leader.leader_port <= 0 ||
            leader.leader_port > std::numeric_limits<uint16_t>::max() ||
            leader.leader_id < 0 ||
            leader.leader_id > static_cast<int32_t>(std::numeric_limits<uint32_t>::max()) ||
            leader.leader_addr.size() > std::numeric_limits<uint16_t>::max()) {
            if (!write_error_response(client_fd, req_hdr, raw_status_internal_error, _running)) {
                break;
            }
            continue;
        }

        std::vector<uint8_t> rsp_body(sizeof(raw_get_leader_rsp) + leader.leader_addr.size());
        raw_get_leader_rsp rsp{};
        rsp.leader_id = htole32(static_cast<uint32_t>(leader.leader_id));
        rsp.leader_port = htole16(static_cast<uint16_t>(leader.leader_port));
        rsp.address_len = htole16(static_cast<uint16_t>(leader.leader_addr.size()));
        std::memcpy(rsp_body.data(), &rsp, sizeof(rsp));
        std::memcpy(
          rsp_body.data() + sizeof(rsp),
          leader.leader_addr.data(),
          leader.leader_addr.size());

        req_hdr.status = htole32(raw_status_ok);
        if (!write_message(client_fd, req_hdr, rsp_body.data(), rsp_body.size(), _running)) {
            break;
        }
    }

    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
    if (conn) {
        conn->fd = -1;
        conn->done.store(true, std::memory_order_release);
    }
    SPDK_NOTICELOG("Closed raw TCP connection on shard %u\n", shard_id);
}
