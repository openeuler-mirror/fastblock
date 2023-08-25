#ifndef RAFT_SERVICE_H_
#define RAFT_SERVICE_H_
#include <pthread.h>
#include "rpc/raft_msg.pb.h"
#include "utils/utils.h"
#include "utils/err_num.h"
#include "raft/raft_private.h"
#include "base/core_sharded.h"

SPDK_LOG_REGISTER_COMPONENT(raft_service)

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

    void install_snapshot(google::protobuf::RpcController* controller,
                         const msg_installsnapshot_t* request,
                         msg_installsnapshot_response_t* response,
                         google::protobuf::Closure* done) override;

    void heartbeat(google::protobuf::RpcController* controller,
                       const heartbeat_request* request,
                       heartbeat_response* response,
                       google::protobuf::Closure* done);

private:
    PartitionManager* _pm;
};

struct append_entries_complete : public context{
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
        response->set_node_id(_pm->get_current_node_id());
        response->set_success(err::RAFT_ERR_NOT_FOUND_PG);
        done->Run();
        return;
    }
    auto raft = _pm->get_pg(shard_id, pool_id, pg_id);

    _pm->get_shard().invoke_on(
      shard_id, 
      [this, raft, done, request, response](){
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
        response->set_node_id(_pm->get_current_node_id());
        response->set_vote_granted(0);
        response->set_prevote(request->prevote());
        done->Run();
        return;
    }
    auto raft = _pm->get_pg(shard_id, pool_id, pg_id);

    _pm->get_shard().invoke_on(
      shard_id, 
      [this, raft, request, response, done](){
        raft->raft_recv_requestvote(request->node_id(), request, response);
        done->Run();                
      });
}

struct install_snapshot_complete : public context{
    google::protobuf::Closure* done;

    install_snapshot_complete(google::protobuf::Closure* _done)
    : done(_done) {}

    void finish(int ) override {
        done->Run();
    }
};

template<typename PartitionManager>
void raft_service<PartitionManager>::install_snapshot(google::protobuf::RpcController* controller,
                     const msg_installsnapshot_t* request,
                     msg_installsnapshot_response_t* response,
                     google::protobuf::Closure* done) {
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        done->Run();
        return;        
    }
    auto raft = _pm->get_pg(shard_id, pool_id, pg_id);

    install_snapshot_complete* complete = new install_snapshot_complete(done);
    _pm->get_shard().invoke_on(
      shard_id, 
      [this, raft, complete, request, response](){
        int ret = raft->raft_recv_installsnapshot(request->node_id(), request, response, complete);
        if(ret != 0){
            complete->complete(-1);
        }               
      });
}

struct heartbeat_complete : public context{
    google::protobuf::Closure* done;
    std::vector<msg_appendentries_t*> reps;
    int count;
    int num;
    pthread_mutex_t mutex;

    heartbeat_complete(google::protobuf::Closure* _done, int _count, bool needs_delete)
    : context(needs_delete)
    , done(_done)
    , count(_count)
    , num(0)
    , mutex(PTHREAD_MUTEX_INITIALIZER) {}

    void finish_del(int ) override {
        if(count > 1)
            pthread_mutex_lock(&mutex);
        num++;
        if(num == count){
            if(count > 1)
                pthread_mutex_unlock(&mutex);
            for(int i = 0; i < count; i++){
                if(reps[i]){
                    delete reps[i];
                }
            }
            done->Run();
            delete this;
        }else{
            if(count > 1)
                pthread_mutex_unlock(&mutex);
        }
    }

    void finish(int ) override {}
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
            complete->reps.push_back(nullptr);
            rsp->set_node_id(_pm->get_current_node_id());
            rsp->set_success(err::RAFT_ERR_NOT_FOUND_PG); 
            complete->complete(err::RAFT_ERR_NOT_FOUND_PG);
            continue;           
        }
        auto raft = _pm->get_pg(shard_id, pool_id, pg_id);
        msg_appendentries_t* req = new msg_appendentries_t();
        req->set_node_id(meta.node_id());
        req->set_pool_id(meta.pool_id());
        req->set_pg_id(meta.pg_id());
        req->set_term(meta.term());
        req->set_prev_log_idx(meta.prev_log_idx());
        req->set_prev_log_term(meta.prev_log_term());
        req->set_leader_commit(meta.leader_commit());
        complete->reps.push_back(req);

        SPDK_DEBUGLOG(raft_service, "recv heartbeat from %d pg: %lu.%lu\n", meta.node_id(), pool_id, pg_id);
        _pm->get_shard().invoke_on(
          shard_id, 
          [this, raft, complete, req, rsp](){
            raft->append_entries_to_buffer(req, rsp, complete);
          });
    }   
}

#endif