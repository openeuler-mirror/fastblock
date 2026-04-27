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

#include "fastblock/rpc/connect_cache.h"
#include "fastblock/rpc/osd_msg.pb.h"
#include "fastblock/msg/rpc_controller.h"
#include "fastblock/utils/err_num.h"
#include <google/protobuf/stubs/callback.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <functional>

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
        if (ctrlr.Failed()) {
            response.set_state(err::RAFT_ERR_NO_CONNECTED);
            SPDK_ERRLOG("change membership in the pg %lu.%lu rpc failed: %s\n",
              _request->pool_id(), _request->pg_id(), ctrlr.ErrorText().c_str());
        } else {
            SPDK_NOTICELOG("change membership in the pg %lu.%lu result: %d\n",
              _request->pool_id(), _request->pg_id(), response.state());
        }
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
        if (ctrlr.Failed()) {
            response.set_state(err::RAFT_ERR_NO_CONNECTED);
            SPDK_ERRLOG("create pg %lu.%lu rpc failed: %s\n",
              _request->pool_id(), _request->pg_id(), ctrlr.ErrorText().c_str());
        } else {
            SPDK_NOTICELOG("create pg %lu.%lu result: %d\n",
              _request->pool_id(), _request->pg_id(), response.state());
        }
        delete this;
    }

    msg::rdma::rpc_controller ctrlr;
    osd::create_pg_response response;
private:
    osd::create_pg_request* _request;
};

class write_object_source{
public:
    using callback_type = std::function<void(int32_t)>;

    write_object_source(osd::write_request* request, callback_type cb)
    : _request(request)
    , _cb(std::move(cb)) {}

    ~write_object_source(){
        if(_request)
            delete _request;
    }

    void process_response(){
        int32_t state = response.state();
        if (ctrlr.Failed()) {
            state = err::RAFT_ERR_NO_CONNECTED;
            SPDK_ERRLOG("write object for pg %lu.%lu rpc failed: %s\n",
              _request->pool_id(), _request->pg_id(), ctrlr.ErrorText().c_str());
        } else {
            SPDK_NOTICELOG("write object for pg %lu.%lu result: %d\n",
              _request->pool_id(), _request->pg_id(), state);
        }
        _cb(state);
        delete this;
    }

    msg::rdma::rpc_controller ctrlr;
    osd::write_reply response;
private:
    osd::write_request* _request;
    callback_type _cb;
};

class read_object_source{
public:
    using callback_type = std::function<void(int32_t, const std::string&)>;

    read_object_source(osd::read_request* request, callback_type cb)
    : _request(request)
    , _cb(std::move(cb)) {}

    ~read_object_source(){
        if(_request)
            delete _request;
    }

    void process_response(){
        int32_t state = response.state();
        std::string data = response.data();
        if (ctrlr.Failed()) {
            state = err::RAFT_ERR_NO_CONNECTED;
            SPDK_ERRLOG("read object for pg %lu.%lu rpc failed: %s\n",
              _request->pool_id(), _request->pg_id(), ctrlr.ErrorText().c_str());
            data.clear();
        } else {
            SPDK_NOTICELOG("read object for pg %lu.%lu result: %d, size: %lu\n",
              _request->pool_id(), _request->pg_id(), state, data.size());
        }
        _cb(state, data);
        delete this;
    }

    msg::rdma::rpc_controller ctrlr;
    osd::read_reply response;
private:
    osd::read_request* _request;
    callback_type _cb;
};

class osd_client {
public:
    osd_client(server_t *s, ::spdk_cpuset* cpumask, std::shared_ptr<msg::rdma::client::options> opts)
        : _cache(cpumask, opts)
        , _shard_cores(core_sharded::get_shard_cores())
        , _s(s)
        , _shard(core_sharded::get_core_sharded())
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
        auto shard_num = _shard_cores.size();
        utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, std::move(conn_cb), nullptr);
        uint32_t shard_id = 0;

        for (shard_id = 0; shard_id < shard_num; shard_id++)
        {
            _shard.invoke_on(
              shard_id,
              [this, node_id, addr, port, shard_id, complete]()
              {
                SPDK_NOTICELOG(
                  "create connect to node %d (address %s, port %d) in core %u\n",
                  node_id, addr.c_str(), port, _shard_cores[shard_id]);

                  _cache.create_connect(
                    shard_id, node_id, addr, port, complete,
                    [this, shard_id, node_id] (msg::rdma::client::connection* conn) {
                        auto &stub = _stubs[shard_id];
                        stub[node_id] = std::make_shared<osd::rpc_service_osd_Stub>(conn);
                    }
                  );
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

    int write_object(uint64_t pool_id, uint64_t pg_id, const std::string& object_name,
                     uint64_t offset, const std::string& data,
                     write_object_source::callback_type cb){
        auto shard_id = 0;
        osd::write_request* request = new osd::write_request();
        request->set_pool_id(pool_id);
        request->set_pg_id(pg_id);
        request->set_object_name(object_name);
        request->set_offset(offset);
        request->set_data(data);

        auto * source = new write_object_source(request, std::move(cb));
        auto done = google::protobuf::NewCallback(source, &write_object_source::process_response);
        auto stub = _get_stub(shard_id, _leader_id);
        if(!stub){
            delete source;
            return err::RAFT_ERR_NO_CONNECTED;
        }

        stub->process_write(&source->ctrlr, request, &source->response, done);
        return err::E_SUCCESS;
    }

    int read_object(uint64_t pool_id, uint64_t pg_id, const std::string& object_name,
                    uint64_t offset, uint64_t length,
                    read_object_source::callback_type cb){
        auto shard_id = 0;
        osd::read_request* request = new osd::read_request();
        request->set_pool_id(pool_id);
        request->set_pg_id(pg_id);
        request->set_object_name(object_name);
        request->set_offset(offset);
        request->set_length(length);

        auto * source = new read_object_source(request, std::move(cb));
        auto done = google::protobuf::NewCallback(source, &read_object_source::process_response);
        auto stub = _get_stub(shard_id, _leader_id);
        if(!stub){
            delete source;
            return err::RAFT_ERR_NO_CONNECTED;
        }

        stub->process_read(&source->ctrlr, request, &source->response, done);
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
    core_sharded &_shard;
};
