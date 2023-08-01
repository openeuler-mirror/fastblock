#ifndef RAFT_SERVICE_H_
#define RAFT_SERVICE_H_
#include "rpc/raft_msg.pb.h"
#include "utils/utils.h"
#include "raft/raft_private.h"
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

    void install_snapshot(google::protobuf::RpcController* controller,
                         const msg_installsnapshot_t* request,
                         msg_installsnapshot_response_t* response,
                         google::protobuf::Closure* done) override;

private:
    PartitionManager* _pm;
};

struct append_entries_complete : public context{
    google::protobuf::Closure* done;

    append_entries_complete(google::protobuf::Closure* _done)
    : done(_done) {}

    void finish(int ) override {
        SPDK_NOTICELOG("append_entries_complete\n");
        done->Run();
    }
};

class append_entries_context : public core_context{
public:
    append_entries_context(std::shared_ptr<raft_server_t> _raft, append_entries_complete *_complete, 
            const msg_appendentries_t* _request, msg_appendentries_response_t* _response)
    : raft(_raft)
    , complete(_complete)
    , request(_request) 
    , response(_response) {}

    void run_task() override {
        SPDK_NOTICELOG("raft_recv_appendentries in core %u\n", spdk_env_get_current_core());
        int ret = raft->raft_recv_appendentries(request->node_id(), request, response, complete);
        if(ret != 0){
            complete->complete(-1);
        }               
    }
    std::shared_ptr<raft_server_t> raft;
    append_entries_complete *complete;
    const msg_appendentries_t* request;
    msg_appendentries_response_t* response;
};

template<typename PartitionManager>
void raft_service<PartitionManager>::append_entries(google::protobuf::RpcController* controller,
             const msg_appendentries_t* request,
             msg_appendentries_response_t* response,
             google::protobuf::Closure* done) {
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    _pm->get_pg_shard(pool_id, pg_id, shard_id);
    auto pg = _pm->get_pg(shard_id, pool_id, pg_id);

    append_entries_complete* complete = new append_entries_complete(done);
    append_entries_context* context = new append_entries_context(pg->raft, complete, request, response);
    _pm->invoke_on(shard_id, context);
}

class vote_context : public core_context{
public:
    vote_context(std::shared_ptr<raft_server_t> _raft, const msg_requestvote_t* _request, 
            msg_requestvote_response_t* _response, google::protobuf::Closure* _done)
    : raft(_raft)
    , request(_request) 
    , response(_response)
    , done(_done) {}

    void run_task() override {
        SPDK_NOTICELOG("raft_recv_requestvote in core %u\n", spdk_env_get_current_core());
        raft->raft_recv_requestvote(request->node_id(), request, response);
        done->Run();        
    }
    std::shared_ptr<raft_server_t> raft;
    const msg_requestvote_t* request;
    msg_requestvote_response_t* response;
    google::protobuf::Closure* done;
};

template<typename PartitionManager>
void raft_service<PartitionManager>::vote(google::protobuf::RpcController* controller,
             const msg_requestvote_t* request,
             msg_requestvote_response_t* response,
             google::protobuf::Closure* done) {
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    _pm->get_pg_shard(pool_id, pg_id, shard_id);
    auto pg = _pm->get_pg(shard_id, pool_id, pg_id);

    vote_context* context = new vote_context(pg->raft, request, response, done);
    _pm->invoke_on(shard_id, context);
}

struct install_snapshot_complete : public context{
    google::protobuf::Closure* done;

    install_snapshot_complete(google::protobuf::Closure* _done)
    : done(_done) {}

    void finish(int ) override {
        done->Run();
    }
};

class install_snapshot_context : public core_context{
public:
    install_snapshot_context(std::shared_ptr<raft_server_t> _raft, install_snapshot_complete *_complete, 
            const msg_installsnapshot_t* _request, msg_installsnapshot_response_t* _response)
    : raft(_raft)
    , complete(_complete)
    , request(_request) 
    , response(_response) {}

    void run_task() override {
        SPDK_NOTICELOG("raft_recv_installsnapshot in core %u\n", spdk_env_get_current_core());
        int ret = raft->raft_recv_installsnapshot(request->node_id(), request, response, complete);
        if(ret != 0){
            complete->complete(-1);
        }               
    }
    std::shared_ptr<raft_server_t> raft;
    install_snapshot_complete *complete;
    const msg_installsnapshot_t* request;
    msg_installsnapshot_response_t* response;
};

template<typename PartitionManager>
void raft_service<PartitionManager>::install_snapshot(google::protobuf::RpcController* controller,
                     const msg_installsnapshot_t* request,
                     msg_installsnapshot_response_t* response,
                     google::protobuf::Closure* done) {
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    _pm->get_pg_shard(pool_id, pg_id, shard_id);
    auto pg = _pm->get_pg(shard_id, pool_id, pg_id);

    install_snapshot_complete* complete = new install_snapshot_complete(done);
    install_snapshot_context* context = new install_snapshot_context(pg->raft, complete, request, response);
    _pm->invoke_on(shard_id, context);
}

#endif