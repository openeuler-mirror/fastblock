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
#include <cstddef>

/*
 * Pure raw-over-RDMA protocol helpers (no RDMA verbs dependency).
 * Shared by osd_raw_rdma_server and unit tests.
 */

namespace raw_rdma_proto {

constexpr uint32_t magic = 0x46425257U; /* 'FBRW' little-endian layout */
constexpr uint8_t version_major = 1U;
constexpr uint8_t version_minor = 0U;
constexpr uint8_t service_osd = 2U;

constexpr uint8_t op_get_leader = 1U;
constexpr uint8_t op_read_object = 2U;
constexpr uint8_t op_write_object = 3U;
constexpr uint8_t op_delete_object = 4U;

constexpr uint32_t flag_response = 1U << 0;

constexpr uint32_t status_ok = 0U;
constexpr uint32_t status_invalid_request = 1U;
constexpr uint32_t status_not_found = 2U;
constexpr uint32_t status_stale_epoch = 3U;
constexpr uint32_t status_retry_later = 4U;
constexpr uint32_t status_not_leader = 5U;
constexpr uint32_t status_pg_initializing = 6U;
constexpr uint32_t status_osd_down = 7U;
constexpr uint32_t status_internal_error = 8U;

/* raw header (28) + max object body (~4MiB) + margin ceiling for body alone */
constexpr size_t max_body_len = (4U * 1024U * 1024U) + 1024U;

struct __attribute__((packed)) header {
    uint32_t magic;
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t service;
    uint8_t opcode;
    uint32_t flags;
    uint64_t seq;
    uint32_t status;
    uint32_t body_len;
};

static_assert(sizeof(header) == 28, "raw RDMA header must be 28 bytes");

/* True when hdr looks like a client request (not a response). */
bool validate_request_header(const header& hdr) noexcept;

const char* opcode_name(uint8_t op) noexcept;

/* Map localstore/raft errno-style codes to raw status (subset used by OSD). */
uint32_t status_from_errno(int state) noexcept;

header make_response_header(const header& req, uint32_t status,
                            uint32_t body_len) noexcept;

/* True if s is a dotted-quad IPv4 literal (not hostname). */
bool is_ipv4_literal(const char* s) noexcept;

} // namespace raw_rdma_proto
