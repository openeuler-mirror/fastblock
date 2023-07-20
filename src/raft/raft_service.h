#ifndef RAFT_SERVICE_H_
#define RAFT_SERVICE_H_
#include "rpc/raft_msg.pb.h"

template<typename PartitionManager>
class raft_service : public rpc_service_raft{
public:
    raft_service(PartitionManager* pm)
    : _pm(pm) {}

    void append_entries(::google::protobuf::RpcController* controller,
                 const ::msg_appendentries_t* request,
                 ::msg_appendentries_response_t* response,
                 ::google::protobuf::Closure* done) override{
        auto pool_id = request->pool_id();
        auto pg_id = request->pg_id();
        uint32_t core_id;
        _pm->get_pg_core(pool_id, pg_id, core_id);
        auto pg = _pm->get_pg(core_id, pool_id, pg_id);

        pg->raft->raft_recv_appendentries(request->node_id(), request, response);
        done->Run();
    }

    void vote(::google::protobuf::RpcController* controller,
                 const ::msg_requestvote_t* request,
                 ::msg_requestvote_response_t* response,
                 ::google::protobuf::Closure* done) override{
        (void)controller;
        (void)request;
        (void)response;
        (void)done;        
    }

    void install_snapshot(::google::protobuf::RpcController* controller,
                         const msg_installsnapshot_t* request,
                         msg_installsnapshot_response_t* response,
                         ::google::protobuf::Closure* done) override{
        (void)controller;
        (void)request;
        (void)response;
        (void)done;        
    } 

private:
    PartitionManager* _pm;
};

#endif