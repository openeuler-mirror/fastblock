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
#include <google/protobuf/service.h>

#include "base/core_sharded.h"
#include "msg/transport_server.h"

class rpc_server{
public:
    using transport_server_ptr = std::shared_ptr<msg::rdma::transport_server>;

    rpc_server(const rpc_server&) = delete;
    rpc_server& operator=(const rpc_server&) = delete;

    void register_service(google::protobuf::Service* service){
        _transport->add_service(service);
    }

    void start(std::string& address, int port){
        _transport->start_listen(address, port);
        auto shard_num = _shard.count();
        for(uint32_t i = 0; i < shard_num; i++){
            _shard.invoke_on(
              i, 
              [this](){
                SPDK_NOTICELOG("start transport in core %u\n", spdk_env_get_current_core());
                _transport->start();
              });
        }
    }

    static rpc_server& get_server(core_sharded &shard){
        static rpc_server s_server(shard);
        return s_server;
    }
private:
    rpc_server(core_sharded &shard)
    : _shard(shard) {
        _transport = std::make_unique<msg::rdma::transport_server>();
        _transport->prepare();
    }

    transport_server_ptr _transport;
    core_sharded &_shard;
};