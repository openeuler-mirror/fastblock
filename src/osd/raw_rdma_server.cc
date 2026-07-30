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

#include <spdk/log.h>

osd_raw_rdma_server::osd_raw_rdma_server(osd_service* service)
  : _service(service) {}

osd_raw_rdma_server::~osd_raw_rdma_server() noexcept {
    stop();
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
    _listeners.assign(shard_count, listener_context{});
    /* CM listen wiring lands in follow-up commits. */
    SPDK_NOTICELOG("raw RDMA server stub start on %s shards=%u (not listening yet)\n",
                   _bind_address.c_str(), shard_count);
    _running.store(true, std::memory_order_release);
    return true;
}

void osd_raw_rdma_server::stop() noexcept {
    if (!_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    _listeners.clear();
    _bind_address.clear();
    SPDK_NOTICELOG("raw RDMA server stopped\n");
}

uint16_t osd_raw_rdma_server::listen_port(uint32_t shard_id) const noexcept {
    if (shard_id >= _listeners.size()) {
        return 0;
    }
    return _listeners[shard_id].port;
}
