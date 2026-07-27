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

#include "raw_rdma_stats.h"

#include <sstream>

std::string format_raw_rdma_listen_ports(const std::vector<uint16_t>& ports) {
    std::ostringstream oss;
    for (size_t i = 0; i < ports.size(); ++i) {
        if (i) {
            oss << ',';
        }
        oss << ports[i];
    }
    return oss.str();
}

std::string format_raw_rdma_server_stats(const raw_rdma_server_stats& stats) {
    std::ostringstream oss;
    oss << "running=" << (stats.running ? 1 : 0)
        << " shards=" << stats.shard_count
        << " conns=" << stats.connection_count
        << " recv=" << stats.recv_total
        << " send=" << stats.send_total
        << " err=" << stats.error_total
        << " accept=" << stats.accept_total
        << " reject=" << stats.reject_total
        << " dispatch_err=" << stats.dispatch_error_total
        << " ports=[" << format_raw_rdma_listen_ports(stats.listen_ports) << ']';
    return oss.str();
}
