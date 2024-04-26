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
#include <google/protobuf/stubs/callback.h>
#include <concepts>

#include "rpc/connect_cache.h"
#include "rpc/raft_msg.pb.h"
#include "msg/rpc_controller.h"
#include "utils/err_num.h"
#include "base/core_sharded.h"

class raft_server_t;
class raft_client_protocol;

void process_appendentries_response(raft_server_t *raft, msg_appendentries_response_t* response,
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id);
void process_requestvote_response(raft_server_t *raft, msg_requestvote_response_t* response,
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id);
void process_timeout_now_response(raft_server_t *raft, timeout_now_response* response,
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id);
void process_snapshot_check_response(raft_server_t *raft, snapshot_check_response* response,
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id);
void process_installsnapshot_response(raft_server_t *raft, installsnapshot_response* response,
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id);

void process_disconnect_rpc(raft_client_protocol* rcp, raft_server_t *raft, uint32_t shard_id, int32_t target_node_id);

template<typename req_type, typename rsp_type>
concept msg_type_valid = (
  (std::is_same_v<req_type, msg_appendentries_t> && std::is_same_v<rsp_type, msg_appendentries_response_t>)
  || (std::is_same_v<req_type, msg_requestvote_t> && std::is_same_v<rsp_type, msg_requestvote_response_t>)
  || (std::is_same_v<req_type, timeout_now_request> && std::is_same_v<rsp_type, timeout_now_response>)
  || (std::is_same_v<req_type, snapshot_check_request> && std::is_same_v<rsp_type, snapshot_check_response>) 
  || (std::is_same_v<req_type, installsnapshot_request> && std::is_same_v<rsp_type, installsnapshot_response>)
);

template<typename request_type, typename response_type>
requires msg_type_valid<request_type, response_type>
class common_msg_source{
public:
    common_msg_source(request_type* request,
            raft_server_t *raft,
            int32_t target_node_id,
            uint32_t shard_id,
            raft_client_protocol* rcp)
    : _request(request)
    , _raft(raft)
    , _target_node_id(target_node_id)
    , _shard_id(shard_id)
    , _rcp(rcp) {}

    ~common_msg_source(){
        if(_request)
            delete _request;
    }

    void process_msg_response(msg_appendentries_response_t* response, msg::rdma::rpc_controller *clr){
        process_appendentries_response(_raft, response, clr, _target_node_id, _rcp, _shard_id);
    }  
    void process_msg_response(msg_requestvote_response_t* response, msg::rdma::rpc_controller *clr){
        process_requestvote_response(_raft, response, clr, _target_node_id, _rcp, _shard_id);
    }    
    void process_msg_response(timeout_now_response* response, msg::rdma::rpc_controller *clr){
        process_timeout_now_response(_raft, response, clr, _target_node_id, _rcp, _shard_id);
    }  
    void process_msg_response(snapshot_check_response* response, msg::rdma::rpc_controller *clr){
        process_snapshot_check_response(_raft, response, clr, _target_node_id, _rcp, _shard_id);
    }
    void process_msg_response(installsnapshot_response* response, msg::rdma::rpc_controller *clr){
        process_installsnapshot_response(_raft, response, clr, _target_node_id, _rcp, _shard_id);
    }  

    void process_response(){
        auto& core_shard = core_sharded::get_core_sharded();
        core_shard.invoke_on(
          _shard_id,
          [this](){
            process_msg_response(&response, &ctrlr);
            delete this;
          }  
        );
    }

    msg::rdma::rpc_controller ctrlr;
    response_type response;
private:
    request_type* _request;
    raft_server_t *_raft;   
    int32_t _target_node_id; 
    uint32_t _shard_id;
    raft_client_protocol* _rcp;
};

class pg_group_t;
class heartbeat_source{
public:
    heartbeat_source(heartbeat_request* request, pg_group_t* group, uint32_t shard_id, 
            int32_t target_node_id, raft_client_protocol* rcp)
    : _request(request)
    , _group(group)
    , _shard_id(shard_id)
    , _target_node_id(target_node_id) 
    , _rcp(rcp){}

    ~heartbeat_source(){
        if(_request)
            delete _request;
    }

    void process_response();

    msg::rdma::rpc_controller ctrlr;
    heartbeat_response response;
private:
    heartbeat_request* _request;
    pg_group_t *_group;
    uint32_t _shard_id;
    int32_t _target_node_id; 
    raft_client_protocol* _rcp;
};

class raft_client_protocol{
public:
    raft_client_protocol(std::shared_ptr<connect_cache> conn_cache)
    : _cache(conn_cache)
    , _shard_cores(get_shard_cores()) {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for(i = 0; i < shard_num; i++){
            _stubs.push_back(std::map<int, std::shared_ptr<rpc_service_raft_Stub>>());
        }
    }

    auto connect_factor() noexcept {
        return _shard_cores.size();
    }

    void create_connect(int node_id, std::string address, int port, auto&& conn_cb) {
        uint32_t shard_id = 0;
        for(shard_id = 0; shard_id < connect_factor() * 1; shard_id++){
            SPDK_INFOLOG_EX(
              pg_group,
              "create connect to node %d (address %s, port %d) in core %u\n",
              node_id, address.c_str(), port, _shard_cores[shard_id]);

            _cache->create_connect(
              shard_id, node_id, address, port, std::move(conn_cb),
              [this, shard_id, node_id] (msg::rdma::client::connection* conn) {
                  auto &stub = _stubs[shard_id];
                  stub[node_id] = std::make_shared<rpc_service_raft_Stub>(conn);
              }
            );
        }
    }

    void remove_connect(int node_id, auto&& conn_cb){
        uint32_t shard_id = 0;
        for(shard_id = 0; shard_id < _shard_cores.size(); shard_id++){
            SPDK_INFOLOG_EX(pg_group, "remove connect to node %d\n", node_id);
            _cache->remove_connect(shard_id, node_id, std::move(conn_cb),
            [this, shard_id, node_id](){
                auto &stub = _stubs[shard_id];
                stub.erase(node_id);
            });
        }
    }

    int send_appendentries(raft_server_t *raft, int32_t target_node_id, msg_appendentries_t* request);
    int send_vote(raft_server_t *raft, int32_t target_node_id, msg_requestvote_t *request);
    int send_heartbeat(int32_t target_node_id, heartbeat_request* request, pg_group_t* group);
    int send_timeout_now(raft_server_t *raft, int32_t target_node_id, timeout_now_request* request);
    int send_snapshot_check(raft_server_t *raft, int32_t target_node_id, snapshot_check_request *request);
    int send_install_snapshot(raft_server_t *raft, int32_t target_node_id, installsnapshot_request *request);


private:
    uint32_t _get_shard_id(){
        auto core_id = spdk_env_get_current_core();
        uint32_t shard_id = 0;

        for(shard_id = 0; shard_id < _shard_cores.size(); shard_id++){
            if(_shard_cores[shard_id] == core_id){
                break;
            }
        }
        return shard_id;
    }

    std::shared_ptr<rpc_service_raft_Stub> _get_stub(uint32_t shard_id, int32_t  node_id) {
        auto &stubs = _stubs[shard_id];
        if(stubs.find(node_id) == stubs.end())
            return nullptr;
        return stubs[node_id];
    }

    std::shared_ptr<connect_cache> _cache;
    std::vector<uint32_t> _shard_cores;
    //每个cpu核上有一个map
    std::vector<std::map<int, std::shared_ptr<rpc_service_raft_Stub>>> _stubs;
};
