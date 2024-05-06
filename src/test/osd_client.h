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

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"

#include "rpc/connect_cache.h"
#include "rpc/osd_msg.pb.h"
#include "msg/rpc_controller.h"
#include "utils/err_num.h"
#include <google/protobuf/stubs/callback.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

static int global_osd_id = 0;
static const char *g_osd_addr = "127.0.0.1";
static int g_osd_port = 8888;
static uint64_t g_pool_id = 0;
static uint64_t g_pg_id = 0;

typedef struct
{
    /* the server's node ID */
    int node_id;
    std::string osd_addr;
    int osd_port;
    uint64_t pool_id;
    uint64_t pg_id;
    boost::property_tree::ptree pt{};
} server_t;

class osd_client;
using get_leader_done_func = std::function<void ()>;

class get_leader_source{
public:
    get_leader_source(int32_t node_id, osd::pg_leader_request* request,
            osd_client *client, get_leader_done_func fun)
    : _node_id(node_id)
    , _request(request)
    , _client(client)
    , _fun(fun) {}

    ~get_leader_source(){
        if(_request)
            delete _request;
    }

    void process_response();

    msg::rdma::rpc_controller ctrlr;
    osd::pg_leader_response response;
private:
    int32_t _node_id;
    osd::pg_leader_request* _request;
    osd_client *_client;
    get_leader_done_func _fun;
};

template<typename request_type, typename response_type>
class change_membership_source{
public:
    change_membership_source(request_type* request)
    : _request(request){}

    ~change_membership_source(){
        if(_request)
            delete _request;
    }

    void process_response(){
        SPDK_NOTICELOG_EX("change membership in the pg %lu.%lu result: %d\n", _request->pool_id(), _request->pg_id(), response.state());
        delete this;        
    }

    msg::rdma::rpc_controller ctrlr;
    response_type response;
private:
    request_type* _request;
};

class create_pg_source{
public:
    create_pg_source(osd::create_pg_request* request)
    : _request(request){}

    ~create_pg_source(){
        if(_request)
            delete _request;
    }

    void process_response(){
        SPDK_NOTICELOG_EX("create pg %lu.%lu result: %d\n", _request->pool_id(), _request->pg_id(), response.state());
        delete this;        
    }

    msg::rdma::rpc_controller ctrlr;
    osd::create_pg_response response;
private:
    osd::create_pg_request* _request;
};

class osd_client {
public:
    osd_client(server_t *s, ::spdk_cpuset* cpumask, std::shared_ptr<msg::rdma::client::options> opts)
        : _cache(cpumask, opts), _shard_cores(get_shard_cores()), _s(s)
    {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for (i = 0; i < shard_num; i++)
        {
            _stubs.push_back(std::map<int, std::shared_ptr<osd::rpc_service_osd_Stub>>());
        }
    }

    void create_connect(const std::string &addr, int port, int node_id, auto&& conn_cb)
    {
        uint32_t shard_id = 0;
        for (shard_id = 0; shard_id < _shard_cores.size(); shard_id++)
        {
            SPDK_NOTICELOG_EX("create connect to node %d (address %s, port %d) in core %u\n",
                           node_id, addr.c_str(), port, _shard_cores[shard_id]);
            _cache.create_connect(0, node_id, addr, port, std::move(conn_cb), 
            [this, shard_id, node_id](msg::rdma::client::connection* connect){
              auto &stub = _stubs[shard_id];
              stub[node_id] = std::make_shared<osd::rpc_service_osd_Stub>(connect);
            });
        }
    }

    int get_leader(int32_t target_node_id, uint64_t pool_id, uint64_t pg_id, get_leader_done_func fun){
        auto shard_id = 0;
        osd::pg_leader_request* request = new osd::pg_leader_request();
        request->set_pool_id(pool_id);
        request->set_pg_id(pg_id);
        get_leader_source * source = new get_leader_source(target_node_id, request, this, fun);

        auto done = google::protobuf::NewCallback(source, &get_leader_source::process_response);
        auto stub = _get_stub(shard_id, target_node_id);
        if(!stub){
            return  err::RAFT_ERR_NO_CONNECTED;
        }        
        stub->process_get_leader(&source->ctrlr, request, &source->response, done);
        return err::E_SUCCESS;
    }

    int add_node(uint64_t pool_id, uint64_t pg_id, int32_t node_id, const std::string& addr, int32_t port){
        auto shard_id = 0;
        osd::add_node_request* request = new osd::add_node_request();
        request->set_pool_id(pool_id);
        request->set_pg_id(pg_id);
        auto info = request->mutable_node();
        info->set_node_id(node_id);
        info->set_addr(addr);
        info->set_port(port);
        auto * source = new change_membership_source<osd::add_node_request, osd::add_node_response>(request);

        auto done = google::protobuf::NewCallback(source, 
                &change_membership_source<osd::add_node_request, osd::add_node_response>::process_response);
        auto stub = _get_stub(shard_id, _leader_id);
        if(!stub){
            return  err::RAFT_ERR_NO_CONNECTED;
        }        
        stub->process_add_node(&source->ctrlr, request, &source->response, done);
        return err::E_SUCCESS;        
    }

    int remove_node(uint64_t pool_id, uint64_t pg_id, int32_t node_id, const std::string& addr, int32_t port){
        auto shard_id = 0;
        osd::remove_node_request* request = new osd::remove_node_request();
        request->set_pool_id(pool_id);
        request->set_pg_id(pg_id);
        auto info = request->mutable_node();
        info->set_node_id(node_id);
        info->set_addr(addr);
        info->set_port(port);
        auto * source = new change_membership_source<osd::remove_node_request, osd::remove_node_response>(request);

        auto done = google::protobuf::NewCallback(source, 
                &change_membership_source<osd::remove_node_request, osd::remove_node_response>::process_response);
        auto stub = _get_stub(shard_id, _leader_id);
        if(!stub){
            return  err::RAFT_ERR_NO_CONNECTED;
        }        
        stub->process_remove_node(&source->ctrlr, request, &source->response, done);
        return err::E_SUCCESS;                
    }

    int change_nodes(uint64_t pool_id, uint64_t pg_id, std::vector<raft_node_info> node_infos){
        auto shard_id = 0;
        osd::change_nodes_request* request = new osd::change_nodes_request();
        request->set_pool_id(pool_id);
        request->set_pg_id(pg_id);
        for(auto& node_info : node_infos){
            auto node = request->add_new_nodes();
            *node = node_info;
        }
        auto * source = new change_membership_source<osd::change_nodes_request, osd::change_nodes_response>(request);

        auto done = google::protobuf::NewCallback(source, 
                &change_membership_source<osd::change_nodes_request, osd::change_nodes_response>::process_response);
        auto stub = _get_stub(shard_id, _leader_id);
        if(!stub){
            return  err::RAFT_ERR_NO_CONNECTED;
        }        
        stub->process_change_nodes(&source->ctrlr, request, &source->response, done);
        return err::E_SUCCESS;                
    }

    int create_pg(int32_t target_node_id, uint64_t pool_id, uint64_t pg_id, int64_t pool_version){
        auto shard_id = 0;
        osd::create_pg_request* request = new osd::create_pg_request(); 
        request->set_pool_id(pool_id);
        request->set_pg_id(pg_id);
        request->set_vision_id(pool_version);     

        auto * source = new create_pg_source(request);
        auto done = google::protobuf::NewCallback(source, &create_pg_source::process_response);
        auto stub = _get_stub(shard_id, target_node_id);
        if(!stub){
            return  err::RAFT_ERR_NO_CONNECTED;
        }        

        stub->process_create_pg(&source->ctrlr, request, &source->response, done);
        return err::E_SUCCESS;                                
    }

    void set_leader_id(int32_t leader_id){
        _leader_id = leader_id;
    }
private:
    std::shared_ptr<osd::rpc_service_osd_Stub> _get_stub(uint32_t shard_id, int32_t node_id)
    {
        auto &stubs = _stubs[shard_id];
        return stubs[node_id];
    }
    connect_cache _cache;
    server_t *_s;
    std::vector<uint32_t> _shard_cores;
    int32_t _leader_id;

    // 每个cpu核上有一个map
    std::vector<std::map<int, std::shared_ptr<osd::rpc_service_osd_Stub>>> _stubs;
};