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

#include <cstdint>
#include <string>
#include <vector>

/*
 * Lightweight snapshot helpers for osd_raw_rdma_server diagnostics.
 * Kept separate so callers can log without pulling full RDMA headers.
 */
struct raw_rdma_server_stats {
    bool running{false};
    uint32_t shard_count{0};
    size_t connection_count{0};
    uint64_t recv_total{0};
    uint64_t send_total{0};
    uint64_t error_total{0};
    /* Lifetime counters (not reset on connection close). */
    uint64_t accept_total{0};
    uint64_t reject_total{0};
    uint64_t dispatch_error_total{0};
    std::vector<uint16_t> listen_ports{};
};

/* Comma-separated listen ports for NOTICE logs, e.g. "20011,20045". */
std::string format_raw_rdma_listen_ports(const std::vector<uint16_t>& ports);

/* One-line summary suitable for SPDK_NOTICELOG. */
std::string format_raw_rdma_server_stats(const raw_rdma_server_stats& stats);
