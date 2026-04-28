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
#include "raft/raft.h"
#include "raft/raft_client_protocol.h"
#include "raft/pg_group.h"
#include "fastblock/rpc/osd_msg.pb.h"

namespace {

class create_pg_msg_source {
public:
    create_pg_msg_source(
      osd::create_pg_request* request,
      uint32_t shard_id,
      int32_t target_node_id,
      std::shared_ptr<msg::rdma::client::connection> conn,
      std::function<void(int)> cb,
      raft_client_protocol* rcp)
      : _request(request)
      , _shard_id(shard_id)
      , _target_node_id(target_node_id)
      , _conn(std::move(conn))
      , _cb(std::move(cb))
      , _rcp(rcp) {}

    ~create_pg_msg_source() {
        if (_request) {
            delete _request;
        }
    }

    void process_response() {
        auto& core_shard = core_sharded::get_core_sharded();
        core_shard.invoke_on(
          _shard_id,
          [this]() {
            int result = response.state();
            if (ctrlr.Failed()) {
                _rcp->remove_connect(_shard_id, _target_node_id, [](void*, int){});
                result = err::RAFT_ERR_NO_CONNECTED;
            }
            if (_cb) {
                _cb(result);
            }
            delete this;
          });
    }

    msg::rdma::rpc_controller ctrlr;
    osd::create_pg_response response;

private:
    osd::create_pg_request* _request;
    uint32_t _shard_id;
    int32_t _target_node_id;
    std::shared_ptr<msg::rdma::client::connection> _conn;
    std::function<void(int)> _cb;
    raft_client_protocol* _rcp;
};

} // namespace

void heartbeat_source::process_response(){
    auto& core_shard = core_sharded::get_core_sharded();
    core_shard.invoke_on(
      _shard_id,
      [this](){
        auto beat_num = _request->heartbeats_size();
        if (ctrlr.Failed()){
            SPDK_INFOLOG(pg_group, "the network connection to %d is disconnected\n", _target_node_id);
            for(int i = 0; i < beat_num; i++){
                const heartbeat_metadata& meta = _request->heartbeats(i);
                auto raft = _group->get_pg(_shard_id, meta.pool_id(), meta.pg_id());
                auto node = raft->raft_get_node(_target_node_id);
                if(node){
                    node->raft_set_suppress_heartbeats(false);
                }
            }
            _rcp->remove_connect(_shard_id, _target_node_id, [](void *, int ){});
            return;
        }
        
        if(response.meta_size() == 0){
            SPDK_INFOLOG(pg_group, "response meta size %d from node: %d\n", response.meta_size(), _target_node_id);
            delete this;
            return;
        }
        for(int i = 0; i < beat_num; i++){
            const heartbeat_metadata& meta = _request->heartbeats(i);
            auto raft = _group->get_pg(_shard_id, meta.pool_id(), meta.pg_id());
            SPDK_DEBUGLOG(pg_group, "heartbeat response from node: %d pg %lu.%lu\n", meta.target_node_id(), meta.pool_id(), meta.pg_id());
            msg_appendentries_response_t *rsp = response.mutable_meta(i);
            // auto node = raft->raft_get_node(rsp->node_id());
            // node->raft_node_set_heartbeating(false);
    
            raft->raft_process_appendentries_reply(rsp, true);
        }
        delete this;
      }
    );
}

void process_disconnect_rpc(raft_client_protocol* rcp, raft_server_t *raft, uint32_t shard_id, int32_t target_node_id){
    SPDK_INFOLOG(pg_group, "pg %lu.%lu in node %d, the network connection to %d is disconnected\n",
                       raft->raft_get_pool_id(), raft->raft_get_pg_id(), raft->raft_get_nodeid(), target_node_id);
    rcp->remove_connect(shard_id, target_node_id, [](void *, int ){});
}

void process_appendentries_response(raft_server_t *raft, msg_appendentries_response_t* response, 
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id){
    if (clr->Failed()){
        process_disconnect_rpc(rcp, raft, shard_id, target_node_id);
        auto node = raft->raft_get_cfg_node(target_node_id);
        if(node){
            node->raft_set_suppress_heartbeats(false);
        }
        return;
    }
    raft->raft_process_appendentries_reply(response);
}

void process_requestvote_response(raft_server_t *raft, msg_requestvote_response_t* response, 
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id){
    if (clr->Failed()){
        process_disconnect_rpc(rcp, raft, shard_id, target_node_id);
        return;
    }
    raft->raft_process_requestvote_reply(response);
}

void process_timeout_now_response(raft_server_t *raft, timeout_now_response* response, 
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id){
    if (clr->Failed()){
        process_disconnect_rpc(rcp, raft, shard_id, target_node_id);
        return;
    }
    raft->raft_process_timeout_now_reply(response);
}

void process_snapshot_check_response(raft_server_t *raft, snapshot_check_response* response, 
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id){
    if (clr->Failed()){
        process_disconnect_rpc(rcp, raft, shard_id, target_node_id);
        return;
    }
    raft->raft_process_snapshot_check_reply(response);
}

void process_installsnapshot_response(raft_server_t *raft, installsnapshot_response* response, 
        msg::rdma::rpc_controller *clr, int32_t target_node_id, raft_client_protocol* rcp, uint32_t shard_id){
    if (clr->Failed()){
        process_disconnect_rpc(rcp, raft, shard_id, target_node_id);
        return;
    }
    raft->raft_process_installsnapshot_reply(response);
}

int raft_client_protocol::send_appendentries(raft_server_t *raft, int32_t target_node_id, msg_appendentries_t* request){
    auto shard_id = _get_shard_id();
    auto source = new common_msg_source<msg_appendentries_t, msg_appendentries_response_t>(request, raft, 
            target_node_id, shard_id, this);
    auto done = google::protobuf::NewCallback(source, 
            &common_msg_source<msg_appendentries_t, msg_appendentries_response_t>::process_response);
    auto stub = _get_stub(shard_id, target_node_id);
    if(!stub){
        SPDK_INFOLOG(pg_group, "pg %lu.%lu in node %d not connect to node %d\n",
                           raft->raft_get_pool_id(), raft->raft_get_pg_id(), raft->raft_get_nodeid(), target_node_id);
        return err::RAFT_ERR_NO_CONNECTED;
    }
    stub->append_entries(&source->ctrlr, request, &source->response, done);
    return err::E_SUCCESS;
}

int raft_client_protocol::send_create_pg(
  int32_t target_node_id,
  uint64_t pool_id,
  uint64_t pg_id,
  int64_t revision_id,
  std::function<void(int)> cb) {
    auto shard_id = _get_shard_id();
    auto conn = _cache->get_connect(shard_id, target_node_id);
    if (!conn) {
        SPDK_INFOLOG(pg_group, "pg %lu.%lu not connect to node %d in shard %u\n",
                     pool_id, pg_id, target_node_id, shard_id);
        return err::RAFT_ERR_NO_CONNECTED;
    }

    auto* request = new osd::create_pg_request();
    request->set_pool_id(pool_id);
    request->set_pg_id(pg_id);
    request->set_vision_id(revision_id);

    auto* source = new create_pg_msg_source(request, shard_id, target_node_id, conn, std::move(cb), this);
    auto* done = google::protobuf::NewCallback(source, &create_pg_msg_source::process_response);
    osd::rpc_service_osd_Stub stub(conn.get());
    stub.process_create_pg(&source->ctrlr, request, &source->response, done);
    return err::E_SUCCESS;
}

int raft_client_protocol::send_vote(raft_server_t *raft, int32_t target_node_id, msg_requestvote_t *request)
{
    auto shard_id = _get_shard_id();
    auto source = new common_msg_source<msg_requestvote_t, msg_requestvote_response_t>(request, raft,
                                                                                       target_node_id, shard_id, this);
    auto done = google::protobuf::NewCallback(source,
                                              &common_msg_source<msg_requestvote_t, msg_requestvote_response_t>::process_response);
    auto stub = _get_stub(shard_id, target_node_id);
    if (!stub)
    {
        SPDK_INFOLOG(pg_group, "pg %lu.%lu in node %d not connect to node %d\n",
                           raft->raft_get_pool_id(), raft->raft_get_pg_id(), raft->raft_get_nodeid(), target_node_id);
        return  err::RAFT_ERR_NO_CONNECTED;
    }
    stub->vote(&source->ctrlr, request, &source->response, done);
    return err::E_SUCCESS;
}

int raft_client_protocol::send_heartbeat(int32_t target_node_id, heartbeat_request* request, pg_group_t* group){
    auto shard_id = _get_shard_id();
    heartbeat_source * source = new heartbeat_source(request, group, shard_id, target_node_id, this);
    SPDK_INFOLOG(pg_group, "heartbeat msg contains %d raft groups, to osd %d\n", request->heartbeats_size(), target_node_id);
    auto done = google::protobuf::NewCallback(source, &heartbeat_source::process_response);
    auto stub = _get_stub(shard_id, target_node_id);
    if(!stub){
        SPDK_INFOLOG(pg_group, "not connect to node %d\n", target_node_id);
        return  err::RAFT_ERR_NO_CONNECTED;
    }
    stub->heartbeat(&source->ctrlr, request, &source->response, done);
    return err::E_SUCCESS;
}

int raft_client_protocol::send_timeout_now(raft_server_t *raft, int32_t target_node_id, timeout_now_request* request){
    auto shard_id = _get_shard_id();
    auto source = new common_msg_source<timeout_now_request, timeout_now_response>(request, raft, 
            target_node_id, shard_id, this);
    auto done = google::protobuf::NewCallback(source, 
            &common_msg_source<timeout_now_request, timeout_now_response>::process_response);
    auto stub = _get_stub(shard_id, target_node_id);
    if(!stub){
        SPDK_INFOLOG(pg_group, "pg %lu.%lu in node %d not connect to node %d\n",
                           raft->raft_get_pool_id(), raft->raft_get_pg_id(), raft->raft_get_nodeid(), target_node_id);
        return err::RAFT_ERR_NO_CONNECTED;
    }
    stub->timeout_now(&source->ctrlr, request, &source->response, done);
    return err::E_SUCCESS;
}

int raft_client_protocol::send_snapshot_check(raft_server_t *raft, int32_t target_node_id, snapshot_check_request *request)
{
    auto shard_id = _get_shard_id();
    auto source = new common_msg_source<snapshot_check_request, snapshot_check_response>(request, raft,
                                                                                         target_node_id, shard_id, this);
    auto done = google::protobuf::NewCallback(source,
                                              &common_msg_source<snapshot_check_request, snapshot_check_response>::process_response);
    auto stub = _get_stub(shard_id, target_node_id);
    if (!stub)
    {
        SPDK_INFOLOG(pg_group, "pg %lu.%lu in node %d not connect to node %d\n",
                           raft->raft_get_pool_id(), raft->raft_get_pg_id(), raft->raft_get_nodeid(), target_node_id);
        return  err::RAFT_ERR_NO_CONNECTED;
    }
    stub->snapshot_check(&source->ctrlr, request, &source->response, done);       
    return err::E_SUCCESS;
}

int raft_client_protocol::send_install_snapshot(raft_server_t *raft, int32_t target_node_id, installsnapshot_request *request){
    auto shard_id = _get_shard_id();
    auto source = new common_msg_source<installsnapshot_request, installsnapshot_response>(request, raft, 
            target_node_id, shard_id, this);
    auto done = google::protobuf::NewCallback(source, 
            &common_msg_source<installsnapshot_request, installsnapshot_response>::process_response);
    auto stub = _get_stub(shard_id, target_node_id);
    if(!stub){
        SPDK_INFOLOG(pg_group, "pg %lu.%lu in node %d not connect to node %d\n",
                           raft->raft_get_pool_id(), raft->raft_get_pg_id(), raft->raft_get_nodeid(), target_node_id);
        return  err::RAFT_ERR_NO_CONNECTED;
    }
    stub->install_snapshot(&source->ctrlr, request, &source->response, done);
    return err::E_SUCCESS;
}