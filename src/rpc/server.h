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
#include <google/protobuf/service.h>

#include "base/core_sharded.h"
#include "msg/rdma/server.h"

class rpc_server{
public:
    using transport_server_ptr = std::shared_ptr<msg::rdma::server>;

    rpc_server(const rpc_server&) = delete;

    rpc_server(const uint32_t core_no, std::shared_ptr<msg::rdma::server::options> srv_opts) {
        ::spdk_cpuset cpumask{};
        ::spdk_cpuset_zero(&cpumask);
        ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
        _transport = std::make_shared<msg::rdma::server>(cpumask, srv_opts);
        _transport->start();
    }

    rpc_server& operator=(const rpc_server&) = delete;

    ~rpc_server() noexcept = default;

public:

    void register_service(google::protobuf::Service* service){
        _transport->add_service(service);
    }

    void stop() noexcept { _transport->stop(); }

private:

    transport_server_ptr _transport{nullptr};
};
