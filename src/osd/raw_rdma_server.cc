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

#include <rdma/rdma_cma.h>
#include <spdk/log.h>

#include <arpa/inet.h>
#include <netinet/in.h>

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
    /* listen lands in follow-up commits. */
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
    /* Event loop lands in follow-up commits. */
    (void)*_listeners[shard_id];
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
