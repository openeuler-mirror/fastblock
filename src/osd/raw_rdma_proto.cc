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

#include "raw_rdma_proto.h"

#include <arpa/inet.h>
#include <cstring>
#include <endian.h>
#include <netinet/in.h>

#include "fastblock/utils/err_num.h"

namespace raw_rdma_proto {

bool validate_request_header(const header& hdr) noexcept {
    if (le32toh(hdr.magic) != magic) {
        return false;
    }
    if (hdr.version_major != version_major) {
        return false;
    }
    /* Minor is soft: accept any minor for forward compatibility. */
    if (hdr.service != service_osd) {
        return false;
    }
    if ((le32toh(hdr.flags) & flag_response) != 0) {
        return false;
    }
    if (le32toh(hdr.body_len) > max_body_len) {
        return false;
    }
    return true;
}

const char* opcode_name(uint8_t op) noexcept {
    switch (op) {
    case op_get_leader:
        return "GET_LEADER";
    case op_read_object:
        return "READ";
    case op_write_object:
        return "WRITE";
    case op_delete_object:
        return "DELETE";
    default:
        return "UNKNOWN";
    }
}

uint32_t status_from_errno(const int state) noexcept {
    switch (state) {
    case err::E_SUCCESS:
        return status_ok;
    case err::E_INVAL:
        return status_invalid_request;
    case err::RAFT_ERR_NOT_FOUND_PG:
    case err::ERR_NOT_FOUND_POOL:
        return status_not_found;
    case err::RAFT_ERR_NOT_FOUND_LEADER:
    case err::RAFT_ERR_NO_CONNECTED:
    case err::RAFT_ERR_MEMBERSHIP_CHANGING:
    case err::RAFT_ERR_SNAPSHOT_WAIT_APPLY:
        return status_retry_later;
    case err::RAFT_ERR_NOT_LEADER:
        return status_not_leader;
    case err::RAFT_ERR_PG_INITIALIZING:
    case err::OSD_STARTING:
        return status_pg_initializing;
    case err::OSD_DOWN:
        return status_osd_down;
    default:
        return status_internal_error;
    }
}

header make_response_header(const header& req, uint32_t status,
                            uint32_t body_len) noexcept {
    header rsp{};
    rsp.magic = htole32(magic);
    rsp.version_major = version_major;
    rsp.version_minor = version_minor;
    rsp.service = req.service;
    rsp.opcode = req.opcode;
    rsp.flags = htole32(flag_response);
    rsp.seq = req.seq;
    rsp.status = htole32(status);
    rsp.body_len = htole32(body_len);
    return rsp;
}

bool is_ipv4_literal(const char* s) noexcept {
    if (!s || !*s) {
        return false;
    }
    in_addr addr{};
    return ::inet_pton(AF_INET, s, &addr) == 1;
}

const char* status_name(uint32_t status) noexcept {
    switch (status) {
    case status_ok:
        return "OK";
    case status_invalid_request:
        return "INVALID_REQUEST";
    case status_not_found:
        return "NOT_FOUND";
    case status_stale_epoch:
        return "STALE_EPOCH";
    case status_retry_later:
        return "RETRY_LATER";
    case status_not_leader:
        return "NOT_LEADER";
    case status_pg_initializing:
        return "PG_INITIALIZING";
    case status_osd_down:
        return "OSD_DOWN";
    case status_internal_error:
        return "INTERNAL_ERROR";
    default:
        return "UNKNOWN_STATUS";
    }
}

bool is_known_opcode(uint8_t op) noexcept {
    switch (op) {
    case op_get_leader:
    case op_read_object:
    case op_write_object:
    case op_delete_object:
        return true;
    default:
        return false;
    }
}

} // namespace raw_rdma_proto
