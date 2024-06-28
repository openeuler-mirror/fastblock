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

    rpc_server(const core_sharded::core_id_type core_no, std::shared_ptr<msg::rdma::server::options> srv_opts) {
        auto mask = core_sharded::make_cpumake(core_no);
        auto sockid = ::spdk_env_get_socket_id(core_no);
        _transport = std::make_shared<msg::rdma::server>(
          FMT_1("rpc_srv_%1%",
          utils::random_string(3)),
          mask.get(), srv_opts, sockid);
        _transport->start();
    }

    rpc_server& operator=(const rpc_server&) = delete;

    ~rpc_server() noexcept = default;

public:

    void register_service(google::protobuf::Service* service){
        _transport->add_service(service);
    }

    void stop(auto&&... args) noexcept { _transport->stop(std::forward<decltype(args)>(args)...); }

private:

    transport_server_ptr _transport{nullptr};
};
