/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include "fastblock/msg/rdma/types.h"
#include "fastblock/msg/sock/types.h"

#include <boost/property_tree/ptree.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace msg {
namespace sock {

struct options {
    std::string bind_address{"0.0.0.0"};
    uint16_t port{0};
    const char* impl{"posix"};
    bool zcopy{true};
    uint32_t max_frame_bytes{default_max_frame_bytes};
    uint32_t max_rpc_bytes{default_max_rpc_bytes};
    uint32_t ae_max_oob_bytes{default_ae_max_oob_bytes};
    uint32_t max_inflight_bytes{default_max_inflight_bytes};
    uint32_t max_process_inflight_bytes{default_max_process_inflight_bytes};
    uint32_t max_inflight_rpcs{default_rpc_batch_size};
    uint32_t max_conns{default_max_conns};
    uint32_t send_buf_size{default_send_buf};
    uint32_t recv_buf_size{default_recv_buf};
    uint32_t connect_queue_depth{default_connect_queue_depth};
    int connect_workers{default_connect_workers};
    std::chrono::microseconds connect_timeout{default_connect_timeout_us};
    std::chrono::microseconds timeout_scan{default_timeout_scan_us};
    std::chrono::microseconds rpc_timeout{rdma::default_rpc_timeout_us};
    int64_t rpc_batch_size{default_rpc_batch_size};

    static std::shared_ptr<options> make(const boost::property_tree::ptree& conf) {
        auto opts = std::make_shared<options>();
        if (conf.count("msg_server_bind_address")) {
            opts->bind_address = conf.get<std::string>("msg_server_bind_address");
        }
        if (conf.count("msg_sock_max_frame_bytes")) {
            opts->max_frame_bytes = conf.get<uint32_t>("msg_sock_max_frame_bytes");
        }
        if (conf.count("msg_sock_max_rpc_bytes")) {
            opts->max_rpc_bytes = conf.get<uint32_t>("msg_sock_max_rpc_bytes");
        }
        if (conf.count("msg_sock_ae_max_oob_bytes")) {
            opts->ae_max_oob_bytes = conf.get<uint32_t>("msg_sock_ae_max_oob_bytes");
        }
        if (conf.count("msg_sock_max_inflight_bytes")) {
            opts->max_inflight_bytes = conf.get<uint32_t>("msg_sock_max_inflight_bytes");
        }
        if (conf.count("msg_sock_max_process_inflight_bytes")) {
            opts->max_process_inflight_bytes =
              conf.get<uint32_t>("msg_sock_max_process_inflight_bytes");
        }
        if (conf.count("msg_sock_max_conns")) {
            opts->max_conns = conf.get<uint32_t>("msg_sock_max_conns");
        }
        if (conf.count("msg_sock_recv_buf_size")) {
            opts->recv_buf_size = conf.get<uint32_t>("msg_sock_recv_buf_size");
        }
        if (conf.count("msg_sock_send_buf_size")) {
            opts->send_buf_size = conf.get<uint32_t>("msg_sock_send_buf_size");
        }
        if (conf.count("msg_sock_connect_queue_depth")) {
            opts->connect_queue_depth = conf.get<uint32_t>("msg_sock_connect_queue_depth");
        }
        if (conf.count("msg_sock_connect_workers")) {
            opts->connect_workers = conf.get<int>("msg_sock_connect_workers");
        }
        if (conf.count("msg_sock_connect_timeout_us")) {
            opts->connect_timeout =
              std::chrono::microseconds{conf.get<int64_t>("msg_sock_connect_timeout_us")};
        }
        if (conf.count("msg_sock_timeout_scan_us")) {
            opts->timeout_scan =
              std::chrono::microseconds{conf.get<int64_t>("msg_sock_timeout_scan_us")};
        }
        if (conf.count("msg_client_rpc_timeout_us")) {
            opts->rpc_timeout =
              std::chrono::microseconds{conf.get<int64_t>("msg_client_rpc_timeout_us")};
        }
        if (conf.count("msg_server_rpc_timeout_us")) {
            opts->rpc_timeout =
              std::chrono::microseconds{conf.get<int64_t>("msg_server_rpc_timeout_us")};
        }
        if (conf.count("msg_client_rpc_batch_size")) {
            opts->rpc_batch_size = conf.get<int64_t>("msg_client_rpc_batch_size");
            opts->max_inflight_rpcs = static_cast<uint32_t>(opts->rpc_batch_size);
        }
        return opts;
    }
};

} // namespace sock
} // namespace msg
