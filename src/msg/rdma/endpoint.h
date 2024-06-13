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

#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>

#include <optional>
#include <string>
#include <string_view>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#define ep_conf_or(key_name) conf_or_s(conf, "msg_rdma_"#key_name, this, key_name)

namespace msg {
namespace rdma {

class endpoint {

public:
    endpoint() = default;

    endpoint(std::string host, uint16_t port)
      : addr{host}
      , port{port} {}

    endpoint(std::optional<std::string> host, uint16_t port)
      : addr{host}
      , port{port} {}

    endpoint(std::string_view host, uint16_t port)
      : addr{host}
      , port{port} {}

    endpoint(boost::property_tree::ptree& conf) {
        ep_conf_or(resolve_timeout_us);
        ep_conf_or(poll_cm_event_timeout_us);
        ep_conf_or(max_send_wr);
        ep_conf_or(max_send_sge);
        ep_conf_or(max_recv_wr);
        ep_conf_or(max_recv_sge);
        ep_conf_or(cq_num_entries);
        ep_conf_or(qp_sig_all);

        auto dev_name = conf.get_child_optional("rdma_device_name");
        if (dev_name.has_value()) {
            auto dev_name_str = dev_name.value().get_value<std::string>();
            if (not dev_name_str.empty()) {
                device_name = dev_name_str;
            }
        }

        auto dev_port = conf.get_child_optional("rdma_device_port");
        if (dev_port.has_value()) {
            device_port = dev_port.value().get_value<uint8_t>();
        }
    }

    endpoint(const endpoint&) = default;

public:

    std::optional<std::string> addr{""};
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
    std::optional<uint8_t> device_port{std::nullopt};
};
} // namespace rdma
} // namespace msg

