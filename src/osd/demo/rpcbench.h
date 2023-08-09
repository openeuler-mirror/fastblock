#pragma once

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "rpc/connect_cache.h"
#include "rpc/osd_msg.pb.h"
#include "msg/rpc_controller.h"
#include <google/protobuf/stubs/callback.h>
#include <random>

static const char *g_pid_path = nullptr;
static int global_osd_id = 0;
static const char *g_osd_addr = "127.0.0.1";
static int g_osd_port = 8888;
static int g_io_size = 4;
static int g_counter = 0;
typedef struct
{
    /* the server's node ID */
    int node_id;
    std::string osd_addr;
    int osd_port;
} server_t;
class client;

class rpcbench_source
{
public:
    rpcbench_source(osd::bench_request *request, server_t *s, client *c)
        : _request(request), _s(s), _c(c) {}

    ~rpcbench_source()
    {
        if (_request)
            delete _request;
        if (_done)
            delete _done;
    }

    void process_response();

    void set_done(google::protobuf::Closure *done)
    {
        _done = done;
    }

    msg::rdma::rpc_controller ctrlr;
    osd::bench_response response;

private:
    server_t *_s;
    client *_c;
    osd::bench_request *_request;
    google::protobuf::Closure *_done;
};

class client
{
public:
    client(server_t *s)
        : _cache(connect_cache::get_connect_cache()), _shard_cores(get_shard_cores()), _s(s)
    {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for (i = 0; i < shard_num; i++)
        {
            _stubs.push_back(std::map<int, std::shared_ptr<osd::rpc_service_osd_Stub>>());
        }
    }

    void create_connect(std::string &addr, int port, int node_id)
    {
        uint32_t shard_id = 0;
        for (shard_id = 0; shard_id < _shard_cores.size(); shard_id++)
        {
            SPDK_NOTICELOG("create connect to node %d (address %s, port %d) in core %u\n",
                           node_id, addr.c_str(), port, _shard_cores[shard_id]);
            auto connect = _cache.create_connect(0, node_id, addr, port);
            auto &stub = _stubs[shard_id];
            stub[node_id] = std::make_shared<osd::rpc_service_osd_Stub>(connect.get());
        }
    }

    void send_bench_request(int32_t target_node_id, osd::bench_request *request)
    {
        // SPDK_NOTICELOG("send bench request\n");
        auto shard_id = 0;
        rpcbench_source *source = new rpcbench_source(request, _s, this);

        auto done = google::protobuf::NewCallback(source, &rpcbench_source::process_response);
        source->set_done(done);
        auto stub = _get_stub(shard_id, target_node_id);
        stub->process_rpc_bench(&source->ctrlr, request, &source->response, done);
    }

private:
    std::shared_ptr<osd::rpc_service_osd_Stub> _get_stub(uint32_t shard_id, int32_t node_id)
    {
        auto &stubs = _stubs[shard_id];
        return stubs[node_id];
    }
    connect_cache &_cache;
    server_t *_s;
    std::vector<uint32_t> _shard_cores;
    // 每个cpu核上有一个map
    std::vector<std::map<int, std::shared_ptr<osd::rpc_service_osd_Stub>>> _stubs;
};

std::string random_string(const size_t length)
{
    static std::string chars{
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "1234567890"
        "!@#$%^&*()"
        "`~-_=+[{]}\\|;:'\",<.>/? "};

    std::random_device rd{};
    std::uniform_int_distribution<decltype(chars)::size_type> index_dist{0, chars.size() - 1};
    std::string ret(length, ' ');
    for (size_t i{0}; i < length; ++i)
    {
        ret[i] = chars[index_dist(rd)];
    }

    return ret;
}
