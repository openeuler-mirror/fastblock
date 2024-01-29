/* Copyright (c) 2023 ChinaUnicom
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

#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>

#include <optional>
#include <string>
#include <string_view>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

namespace msg {
namespace rdma {

class endpoint {

public:
    endpoint() = default;

    endpoint(std::string host, uint16_t port)
      : addr{host}
      , port{port} {}

    endpoint(std::string_view host, uint16_t port)
      : addr{host}
      , port{port} {}

    endpoint(boost::property_tree::ptree& conf) {
        resolve_timeout_us =
          conf.get_child("resolve_timeout_us").get_value<decltype(resolve_timeout_us)>();
        poll_cm_event_timeout_us =
          conf.get_child("poll_cm_event_timeout_us").get_value<decltype(poll_cm_event_timeout_us)>();
        max_send_wr = conf.get_child("max_send_wr").get_value<decltype(max_send_wr)>();
        max_send_sge = conf.get_child("max_send_sge").get_value<decltype(max_send_sge)>();
        max_recv_wr = conf.get_child("max_recv_wr").get_value<decltype(max_recv_wr)>();
        max_recv_sge = conf.get_child("max_recv_sge").get_value<decltype(max_recv_sge)>();
        cq_num_entries = conf.get_child("cq_num_entries").get_value<decltype(cq_num_entries)>();
        qp_sig_all = conf.get_child("qp_sig_all").get_value<decltype(qp_sig_all)>();
        auto dev_name = conf.get_child_optional("rdma_device_name");
        if (dev_name.has_value()) {
            device_name = dev_name.value().get_value<std::string>();
        }
    }

    endpoint(const endpoint&) = default;

public:

    std::string addr{""};
    uint16_t port{0};
    bool passive{false};
    int backlog{1024};
    int resolve_timeout_us{2000};
    int poll_cm_event_timeout_us{1000000}; // 1s

    uint32_t max_send_wr{1024};
    uint32_t max_recv_wr{1024};
    uint32_t max_send_sge{64};
    uint32_t max_recv_sge{64};
    uint32_t max_inline_data{64};

    int cq_num_entries{16};
    bool qp_sig_all{false};
    std::optional<std::string> device_name{std::nullopt};
};
} // namespace rdma
} // namespace msg

