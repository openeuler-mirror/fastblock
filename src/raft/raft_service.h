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
#include <pthread.h>
#include "rpc/raft_msg.pb.h"
#include "utils/utils.h"
#include "utils/err_num.h"
#include "raft/raft.h"
#include "base/core_sharded.h"

template<class PartitionManager>
class raft_service : public rpc_service_raft{
public:
    raft_service(PartitionManager* pm)
    : _pm(pm) {}

    void append_entries(google::protobuf::RpcController* controller,
                 const msg_appendentries_t* request,
                 msg_appendentries_response_t* response,
                 google::protobuf::Closure* done) override;

    void vote(google::protobuf::RpcController* controller,
                 const msg_requestvote_t* request,
                 msg_requestvote_response_t* response,
                 google::protobuf::Closure* done) override;

    void heartbeat(google::protobuf::RpcController* controller,
                       const heartbeat_request* request,
                       heartbeat_response* response,
                       google::protobuf::Closure* done) override;

    void timeout_now(google::protobuf::RpcController* controller,
                       const timeout_now_request* request,
                       timeout_now_response* response,
                       google::protobuf::Closure* done) override;

    void snapshot_check(google::protobuf::RpcController* controller,
                       const snapshot_check_request* request,
                       snapshot_check_response* response,
                       google::protobuf::Closure* done) override;
    void install_snapshot(google::protobuf::RpcController* controller,
                       const installsnapshot_request* request,
                       installsnapshot_response* response,
                       google::protobuf::Closure* done) override;    
private:
    PartitionManager* _pm;
};

struct append_entries_complete : public utils::context{
    google::protobuf::Closure* done;

    append_entries_complete(google::protobuf::Closure* _done)
    : done(_done) {}

    void finish(int ) override {
        done->Run();
    }
};

template<typename PartitionManager>
void raft_service<PartitionManager>::append_entries(google::protobuf::RpcController* controller,
             const msg_appendentries_t* request,
             msg_appendentries_response_t* response,
             google::protobuf::Closure* done) {
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
        response->set_node_id(_pm->get_current_node_id());
        response->set_success(err::RAFT_ERR_NOT_FOUND_PG);
        done->Run();
        return;
    }

    _pm->get_shard().invoke_on(
      shard_id,
      [this, shard_id, done, request, response](){
        auto raft = _pm->get_pg(shard_id, request->pool_id(), request->pg_id());
        if(!raft){
            SPDK_WARNLOG("not find pg %lu.%lu\n", request->pool_id(), request->pg_id());
            response->set_node_id(_pm->get_current_node_id());
            response->set_success(err::RAFT_ERR_NOT_FOUND_PG);
            done->Run();
            return;
        }

        auto err_num = raft_state_to_errno(raft->raft_get_op_state());
        if(err_num != err::E_SUCCESS){
            SPDK_WARNLOG("handle append entry request of pg %lu.%lu failed: %s\n", 
                    request->pool_id(), request->pg_id(), err::string_status(err_num));
            response->set_node_id(_pm->get_current_node_id());
            response->set_success(err_num);
            done->Run();
            return;            
        }

        append_entries_complete *complete = new append_entries_complete(done);
        raft->append_entries_to_buffer(request, response, complete);
      });
}

template<typename PartitionManager>
void raft_service<PartitionManager>::vote(google::protobuf::RpcController* controller,
             const msg_requestvote_t* request,
             msg_requestvote_response_t* response,
             google::protobuf::Closure* done) {
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
        response->set_node_id(_pm->get_current_node_id());
        response->set_vote_granted(RAFT_REQUESTVOTE_ERR_NOT_GRANTED);
        response->set_prevote(request->prevote());
        response->set_term(0);
        done->Run();
        return;
    }

    _pm->get_shard().invoke_on(
      shard_id,
      [this, shard_id, request, response, done](){
        auto raft = _pm->get_pg(shard_id, request->pool_id(), request->pg_id());
        int err_num;
        if(!raft || ( err_num = raft_state_to_errno(raft->raft_get_op_state()) ) != err::E_SUCCESS){
            if(!raft)
                err_num = err::RAFT_ERR_NOT_FOUND_PG;
            SPDK_WARNLOG("handle vote request of pg %lu.%lu failed: %s\n", 
                    request->pool_id(), request->pg_id(), err::string_status(err_num));
            response->set_node_id(_pm->get_current_node_id());
            response->set_vote_granted(RAFT_REQUESTVOTE_ERR_NOT_GRANTED);
            response->set_prevote(request->prevote());
            response->set_term(0);
            done->Run();
            return;
        }
        raft->raft_recv_requestvote(request->node_id(), request, response);
        done->Run();
      });
}

struct snapshot_complete : public utils::context{
    google::protobuf::Closure* done;

    snapshot_complete(google::protobuf::Closure* _done)
    : done(_done) {}

    void finish(int ) override {
        done->Run();
    }
};

template<typename PartitionManager>
void raft_service<PartitionManager>::snapshot_check(google::protobuf::RpcController* controller,
                   const snapshot_check_request* request,
                   snapshot_check_response* response,
                   google::protobuf::Closure* done) {
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
        done->Run();
        return;        
    }

    _pm->get_shard().invoke_on(
      shard_id, 
      [this, shard_id, done, request, response](){
        auto raft = _pm->get_pg(shard_id, request->pool_id(), request->pg_id());
        if(!raft){
            SPDK_WARNLOG("not find pg %lu.%lu\n", request->pool_id(), request->pg_id());
            done->Run();
            return; 
        }

        auto err_num = raft_state_to_errno(raft->raft_get_op_state());
        if(err_num != err::E_SUCCESS){
            SPDK_WARNLOG("handle install_snapshot request of pg %lu.%lu failed: %s\n", 
                    request->pool_id(), request->pg_id(), err::string_status(err_num));
            done->Run();
            return;            
        }

        snapshot_complete* complete = new snapshot_complete(done);
        raft->raft_recv_snapshot_check(request->node_id(), request, response, complete);
      });
}

template<typename PartitionManager>
void raft_service<PartitionManager>::install_snapshot(google::protobuf::RpcController* controller,
                     const installsnapshot_request* request,
                     installsnapshot_response* response,
                     google::protobuf::Closure* done) {
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
        done->Run();
        return;
    }

    _pm->get_shard().invoke_on(
      shard_id,
      [this, shard_id, done, request, response](){
        auto raft = _pm->get_pg(shard_id, request->pool_id(), request->pg_id());
        if(!raft){
            SPDK_WARNLOG("not find pg %lu.%lu\n", request->pool_id(), request->pg_id());
            done->Run();
            return;
        }

        auto err_num = raft_state_to_errno(raft->raft_get_op_state());
        if(err_num != err::E_SUCCESS){
            SPDK_WARNLOG("handle install_snapshot request of pg %lu.%lu failed: %s\n", 
                    request->pool_id(), request->pg_id(), err::string_status(err_num));
            done->Run();
            return;            
        }

        snapshot_complete* complete = new snapshot_complete(done);
        raft->raft_recv_installsnapshot(request->node_id(), request, response, complete);
      });
}

struct heartbeat_complete : public utils::context{
    google::protobuf::Closure* done;
    std::vector<msg_appendentries_t*> reps;
    int count;
    int num;
    pthread_mutex_t mutex;

    heartbeat_complete(google::protobuf::Closure* _done, int _count, bool needs_delete)
    : utils::context(needs_delete)
    , done(_done)
    , count(_count)
    , num(0)
    , mutex(PTHREAD_MUTEX_INITIALIZER) {}

    void finish_del(int ) override {
        if(_need_mutex())
            pthread_mutex_lock(&mutex);
        num++;
        if(num == count){
            if(_need_mutex())
                pthread_mutex_unlock(&mutex);
            for(int i = 0; i < count; i++){
                if(reps[i]){
                    delete reps[i];
                }
            }
            done->Run();
            delete this;
        }else{
            if(_need_mutex())
                pthread_mutex_unlock(&mutex);
        }
    }

    void finish(int ) override {}

private:
    bool _need_mutex(){
        if(count > 1 && spdk_env_get_core_count() > 1)
            return true;
        return false;
    }
};

template<typename PartitionManager>
void raft_service<PartitionManager>::heartbeat(google::protobuf::RpcController* controller,
                       const heartbeat_request* request,
                       heartbeat_response* response,
                       google::protobuf::Closure* done){
    auto beat_num = request->heartbeats_size();
    heartbeat_complete *complete = new heartbeat_complete(done, beat_num, false);

    for(int i = 0; i < beat_num; i++){
        const heartbeat_metadata& meta = request->heartbeats(i);

        auto rsp = response->add_meta();

        auto pool_id = meta.pool_id();
        auto pg_id = meta.pg_id();
        uint32_t shard_id;
        if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
            SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
            complete->reps.push_back(nullptr);
            rsp->set_node_id(_pm->get_current_node_id());
            rsp->set_success(err::RAFT_ERR_NOT_FOUND_PG);
            complete->complete(err::RAFT_ERR_NOT_FOUND_PG);
            continue;
        }

        msg_appendentries_t* req = new msg_appendentries_t();
        complete->reps.push_back(req);

        SPDK_DEBUGLOG(pg_group, "recv heartbeat from %d pg: %lu.%lu\n", meta.node_id(), pool_id, pg_id);
        _pm->get_shard().invoke_on(
          shard_id,
          [this, &meta, shard_id, complete, req, rsp](){
            auto raft = _pm->get_pg(shard_id, meta.pool_id(), meta.pg_id());
            if(!raft){
                SPDK_WARNLOG("not find pg %lu.%lu\n", meta.pool_id(), meta.pg_id());
                rsp->set_node_id(_pm->get_current_node_id());
                rsp->set_success(err::RAFT_ERR_NOT_FOUND_PG);
                complete->complete(err::RAFT_ERR_NOT_FOUND_PG);
                return;
            }

            auto err_num = raft_state_to_errno(raft->raft_get_op_state());
            if(err_num != err::E_SUCCESS){
                SPDK_WARNLOG("handle heartbeat of pg %lu.%lu failed: %s\n", 
                        meta.pool_id(), meta.pg_id(), err::string_status(err_num));
                rsp->set_node_id(_pm->get_current_node_id());
                rsp->set_success(err_num);
                complete->complete(err_num);
                return;            
            }

            req->set_node_id(meta.node_id());
            req->set_pool_id(meta.pool_id());
            req->set_pg_id(meta.pg_id());
            req->set_term(meta.term());
            req->set_prev_log_idx(meta.prev_log_idx());
            req->set_prev_log_term(meta.prev_log_term());
            req->set_leader_commit(meta.leader_commit());

            raft->append_entries_to_buffer(req, rsp, complete);
          });
    }
}

template<typename PartitionManager>
void raft_service<PartitionManager>::timeout_now(google::protobuf::RpcController* controller,
                   const timeout_now_request* request,
                   timeout_now_response* response,
                   google::protobuf::Closure* done)
{
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
        response->set_node_id(_pm->get_current_node_id());
        response->set_success(err::RAFT_ERR_NOT_FOUND_PG);
        done->Run();
        return;
    }

    _pm->get_shard().invoke_on(
      shard_id, 
      [this, shard_id, done, request, response](){
        auto raft = _pm->get_pg(shard_id, request->pool_id(), request->pg_id());
        if(!raft){
            SPDK_WARNLOG("not find pg %lu.%lu\n", request->pool_id(), request->pg_id());
            response->set_node_id(_pm->get_current_node_id());
            response->set_success(err::RAFT_ERR_NOT_FOUND_PG);
            done->Run();
            return;
        }

        auto err_num = raft_state_to_errno(raft->raft_get_op_state());
        if(err_num != err::E_SUCCESS){
            SPDK_WARNLOG("handle append entry request of pg %lu.%lu failed: %s\n", 
                    request->pool_id(), request->pg_id(), err::string_status(err_num));
            response->set_node_id(_pm->get_current_node_id());
            response->set_success(err_num);
            done->Run();
            return;            
        }

        SPDK_DEBUGLOG(pg_group, "recv timeout_now request from node %d\n", request->node_id());
        raft->raft_set_current_term(raft->raft_get_current_term() + 1);
        raft->raft_election_start();
        response->set_node_id(_pm->get_current_node_id());
        response->set_success(err_num);
        done->Run();        
      });    
}