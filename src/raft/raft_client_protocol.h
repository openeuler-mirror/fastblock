#ifndef RAFT_CLIENT_PROTOCOL
#define RAFT_CLIENT_PROTOCOL
#include <google/protobuf/stubs/callback.h>
#include "rpc/connect_cache.h"
#include "rpc/raft_msg.pb.h"
#include "msg/rpc_controller.h"

class raft_server_t;

class appendentries_source{
public:
    appendentries_source(msg_appendentries_t* request,
            raft_server_t *raft)
    : _request(request)        
    , _raft(raft) {}

    ~appendentries_source(){
        if(_request)
            delete _request;
        if(_done)
            delete _done;        
    }

    void process_response();

    void set_done(google::protobuf::Closure* done){
        _done = done;
    }

    msg::rdma::rpc_controller ctrlr;
    msg_appendentries_response_t response;
private:
    msg_appendentries_t* _request;
    google::protobuf::Closure* _done;
    raft_server_t *_raft;
};

class vote_source{
public:
    vote_source(msg_requestvote_t* request,
            raft_server_t *raft)
    : _request(request)    
    , _raft(raft) {}

    ~vote_source(){
        if(_request)
            delete _request;
        if(_done)
            delete _done;        
    }

    void process_response();

    void set_done(google::protobuf::Closure* done){
        _done = done;
    }

    msg::rdma::rpc_controller ctrlr;
    msg_requestvote_response_t response;
private:
    msg_requestvote_t* _request;
    google::protobuf::Closure* _done;
    raft_server_t *_raft;
};

class install_snapshot_source{
public:
    install_snapshot_source(msg_installsnapshot_t* request,
            raft_server_t *raft)
    : _request(request)        
    , _raft(raft) {}

    ~install_snapshot_source(){
        if(_request)
            delete _request;
        if(_done)
            delete _done;        
    }

    void process_response();

    void set_done(google::protobuf::Closure* done){
        _done = done;
    }

    msg::rdma::rpc_controller ctrlr;
    msg_installsnapshot_response_t response;
private:
    msg_installsnapshot_t* _request;
    google::protobuf::Closure* _done;
    raft_server_t *_raft;
};

class raft_client_protocol{
public:
    raft_client_protocol()
    : _cache(connect_cache::get_connect_cache())
    , _shard_cores(get_shard_cores()) {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for(i = 0; i < shard_num; i++){
            _stubs.push_back(std::map<int, std::shared_ptr<rpc_service_raft_Stub>>());
        }
    }

    void create_connect(int node_id, std::string& address, int port){
        uint32_t shard_id = 0;
        for(shard_id = 0; shard_id < _shard_cores.size(); shard_id++){
            SPDK_NOTICELOG("create connect to node %d (address %s, port %d) in core %u\n", 
                    node_id, address.c_str(), port, _shard_cores[shard_id]);
            auto connect = _cache.create_connect(shard_id, node_id, address, port);
            auto &stub = _stubs[shard_id];
            stub[node_id] = std::make_shared<rpc_service_raft_Stub>(connect.get());
        }
    }

    void remove_connect(int node_id){
        uint32_t shard_id = 0;
        for(shard_id = 0; shard_id < _shard_cores.size(); shard_id++){
            auto &stub = _stubs[shard_id];
            stub.erase(node_id);
            _cache.remove_connect(shard_id, node_id);
        }
    }

    void send_appendentries(raft_server_t *raft, int32_t target_node_id, msg_appendentries_t* request){
        auto shard_id = _get_shard_id();
        appendentries_source * source = new appendentries_source(request, raft);

        auto done = google::protobuf::NewCallback(source, &appendentries_source::process_response);
        source->set_done(done);
        auto stub = _get_stub(shard_id, target_node_id);
        stub->append_entries(&source->ctrlr, request, &source->response, done);
    }

    void send_vote(raft_server_t *raft, int32_t target_node_id, msg_requestvote_t *request){
        auto shard_id = _get_shard_id();
        vote_source * source = new vote_source(request, raft);

        auto done = google::protobuf::NewCallback(source, &vote_source::process_response);
        source->set_done(done);
        auto stub = _get_stub(shard_id, target_node_id);
        stub->vote(&source->ctrlr, request, &source->response, done);
    }

    void send_install_snapshot(raft_server_t *raft, int32_t target_node_id, msg_installsnapshot_t *request){
        auto shard_id = _get_shard_id();
        install_snapshot_source * source = new install_snapshot_source(request, raft);

        auto done = google::protobuf::NewCallback(source, &install_snapshot_source::process_response);
        source->set_done(done);
        auto stub = _get_stub(shard_id, target_node_id);
        stub->install_snapshot(&source->ctrlr, request, &source->response, done);
    }

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
        return stubs[node_id];
    }

    connect_cache& _cache;
    std::vector<uint32_t> _shard_cores;
    //每个cpu核上有一个map
    std::vector<std::map<int, std::shared_ptr<rpc_service_raft_Stub>>> _stubs;
};

#endif