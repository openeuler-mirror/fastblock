/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include "fastblock/msg/bind_error.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace msg {
namespace sock {

static constexpr uint32_t k_magic = 0x4B534246U; // on-wire bytes 46 42 53 4B
static constexpr uint8_t k_version = 1;

static constexpr uint8_t k_flag_response = 1U << 0;
static constexpr uint8_t k_flag_more = 1U << 1;
static constexpr uint8_t k_flag_known = k_flag_response | k_flag_more;

static constexpr uint32_t default_max_frame_bytes = 5U * 1024U * 1024U;
static constexpr uint32_t default_max_rpc_bytes = 16U * 1024U * 1024U;
static constexpr uint32_t default_ae_max_oob_bytes = 8U * 1024U * 1024U;
static constexpr uint32_t default_max_inflight_bytes = 16U * 1024U * 1024U;
static constexpr uint32_t default_max_process_inflight_bytes = 64U * 1024U * 1024U;
static constexpr uint32_t default_max_conns = 128;
static constexpr uint32_t default_rpc_batch_size = 1024;
static constexpr uint32_t default_connect_queue_depth = 64;
static constexpr int default_connect_workers = 4;
static constexpr int64_t default_connect_timeout_us = 1'000'000;
static constexpr int64_t default_timeout_scan_us = 10'000;
static constexpr uint32_t default_send_buf = 2U * 1024U * 1024U;
static constexpr uint32_t default_recv_buf = 2U * 1024U * 1024U;
static constexpr size_t k_max_read_per_tick = 256U * 1024U;
static constexpr size_t k_recv_shrink = 256U * 1024U;
static constexpr size_t k_proto_hard_cap = 1U * 1024U * 1024U;

struct sock_frame_hdr {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t frame_seq;
    uint64_t correlation;
    uint32_t meta_bytes;
    uint32_t proto_bytes;
    uint32_t payload_bytes;
    uint32_t hdr_crc32;
} __attribute__((packed));
static_assert(sizeof(sock_frame_hdr) == 32, "sock_frame_hdr must be 32 bytes");

struct wire_request_meta {
    char service_name[32];
    uint8_t service_name_size;
    char method_name[32];
    uint8_t method_name_size;
    uint32_t data_size;
    uint32_t payload_size;
} __attribute__((packed));
static_assert(sizeof(wire_request_meta) == 74, "wire_request_meta must be 74 bytes");

struct wire_reply_meta {
    uint8_t reply_status;
    uint8_t _pad[3];
    uint32_t data_size;
    uint32_t payload_size;
} __attribute__((packed));
static_assert(sizeof(wire_reply_meta) == 12, "wire_reply_meta must be 12 bytes");

class bind_error : public ::msg::bind_error {
public:
    explicit bind_error(const std::string& what) : _what(what) {}
    const char* what() const noexcept override { return _what.c_str(); }

private:
    std::string _what;
};

inline uint64_t make_correlation(uint32_t gen, uint32_t idx) noexcept {
    return (static_cast<uint64_t>(gen) << 32) | idx;
}

struct probe {
    uint64_t unknown_key{0};
    uint64_t timeout{0};
    uint64_t exhausted{0};
    uint64_t bad_hdr{0};
    uint64_t complete_ok{0};
    uint64_t complete_fail{0};
    uint64_t conn_teardown{0};
    uint64_t eagain{0};
    uint64_t closure_double{0};
    uint64_t connect_fail{0};
    uint64_t reply_drop{0};
    uint64_t accept_drop{0};
    uint64_t assembly_timeout{0};
};

} // namespace sock
} // namespace msg
