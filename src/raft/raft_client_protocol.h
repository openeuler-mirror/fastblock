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
    }

    void process_response();

    msg::rdma::rpc_controller ctrlr;
    msg_appendentries_response_t response;
private:
    msg_appendentries_t* _request;
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
    }

    void process_response();

    msg::rdma::rpc_controller ctrlr;
    msg_requestvote_response_t response;
private:
    msg_requestvote_t* _request;
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
    }

    void process_response();

    msg::rdma::rpc_controller ctrlr;
    msg_installsnapshot_response_t response;
private:
    msg_installsnapshot_t* _request;
    raft_server_t *_raft;
};

class pg_group_t;
class heartbeat_source{
public:
    heartbeat_source(heartbeat_request* request, pg_group_t* group, uint32_t shard_id)
    : _request(request)
    , _group(group)
    , _shard_id(shard_id){}

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

    auto connect_factor() noexcept {
        return _shard_cores.size();
    }

    void create_connect(int node_id, std::string& address, int port, std::optional<std::function<void()>> cb = std::nullopt){
        uint32_t shard_id = 0;
        for(shard_id = 0; shard_id < connect_factor() * 1; shard_id++){
            SPDK_NOTICELOG("create connect to node %d (address %s, port %d) in core %u\n",
                    node_id, address.c_str(), port, _shard_cores[shard_id]);
            auto connect = _cache.create_connect(shard_id, node_id, address, port, cb);
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
        auto stub = _get_stub(shard_id, target_node_id);
        stub->append_entries(&source->ctrlr, request, &source->response, done);
    }

    void send_vote(raft_server_t *raft, int32_t target_node_id, msg_requestvote_t *request){
        auto shard_id = _get_shard_id();
        vote_source * source = new vote_source(request, raft);

        auto done = google::protobuf::NewCallback(source, &vote_source::process_response);
        auto stub = _get_stub(shard_id, target_node_id);
        stub->vote(&source->ctrlr, request, &source->response, done);
    }

    void send_install_snapshot(raft_server_t *raft, int32_t target_node_id, msg_installsnapshot_t *request){
        auto shard_id = _get_shard_id();
        install_snapshot_source * source = new install_snapshot_source(request, raft);

        auto done = google::protobuf::NewCallback(source, &install_snapshot_source::process_response);
        auto stub = _get_stub(shard_id, target_node_id);
        stub->install_snapshot(&source->ctrlr, request, &source->response, done);
    }

    void send_heartbeat(int32_t target_node_id, heartbeat_request* request, pg_group_t* group){
        auto shard_id = _get_shard_id();
        heartbeat_source * source = new heartbeat_source(request, group, shard_id);

        auto done = google::protobuf::NewCallback(source, &heartbeat_source::process_response);
        auto stub = _get_stub(shard_id, target_node_id);
        stub->heartbeat(&source->ctrlr, request, &source->response, done);
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