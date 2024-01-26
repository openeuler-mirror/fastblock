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

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/histogram_data.h"

#include "rpc/connect_cache.h"
#include "rpc/osd_msg.pb.h"
#include "msg/rpc_controller.h"
#include <google/protobuf/stubs/callback.h>
#include <random>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

static const char *g_pid_path = nullptr;
static int global_osd_id = 0;
static const char *g_osd_addr = "127.0.0.1";
static int g_osd_port = 8888;
static int g_bench_type = 0;
static int g_io_size = 4;
static int g_queue_depth = 1;
static int g_total_seconds = 0;
static int g_counter = 0;
static int g_total_ios = 0;
static int g_seconds = 0;
static int g_counter_last_value = 0;
static spdk_poller *poller_printer;
static struct spdk_histogram_data *g_histogram;
static uint64_t g_latency_min = -1;
static uint64_t g_latency_max = 0;
static uint64_t g_first_io_tsc = 0;

typedef struct
{
    /* the server's node ID */
    int node_id;
    std::string osd_addr;
    int osd_port;
    boost::property_tree::ptree pt{};
} server_t;
class client;

static const double _cutoffs[] = {
    0.1,
    0.5,
    0.90,
    0.95,
    0.99,
    0.999,
    -1,
};
static void process_response(client *c, uint64_t id);

static void
_check_cutoff(void *ctx, uint64_t start, uint64_t end, uint64_t count,
              uint64_t total, uint64_t so_far)
{
    double so_far_pct;
    double **cutoff = (double **)ctx;

    if (count == 0)
    {
        return;
    }

    so_far_pct = (double)so_far / total;
    while (so_far_pct >= **cutoff && **cutoff > 0)
    {
        printf("%f:%fus ", **cutoff * 100, (double)end * 1000 * 1000 / spdk_get_ticks_hz());
        (*cutoff)++;
    }
}

class client
{
public:
    client(server_t *s, int type, ::spdk_cpuset* cpumask, std::shared_ptr<msg::rdma::client::options> opts)
        : _cache(cpumask, opts), _shard_cores(get_shard_cores()), _s(s), _type(type)
    {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for (i = 0; i < shard_num; i++)
        {
            _stubs.push_back(std::map<int, std::shared_ptr<osd::rpc_service_osd_Stub>>());
        }
    }

    void create_connect(std::string &addr, int port, int node_id, auto&& conn_cb)
    {
        uint32_t shard_id = 0;
        for (shard_id = 0; shard_id < _shard_cores.size(); shard_id++)
        {
            SPDK_NOTICELOG("create connect to node %d (address %s, port %d) in core %u\n",
                           node_id, addr.c_str(), port, _shard_cores[shard_id]);
            _cache.create_connect(0, node_id, addr, port, std::move(conn_cb),
            [this, shard_id, node_id](msg::rdma::client::connection* connect){
              auto &stub = _stubs[shard_id];
              stub[node_id] = std::make_shared<osd::rpc_service_osd_Stub>(connect);
            });
        }
    }

    void send_request(int32_t target_node_id)
    {
        if (_inflight_ops_counter < g_queue_depth)
        {
            auto shard_id = 0;
            auto stub = _get_stub(shard_id, target_node_id);
            auto _done = google::protobuf::NewCallback(process_response, this, _last_submitted_id);
            if (_type == 1)
            {
                osd::write_request *request = new osd::write_request();
                request->set_pool_id(1);
                request->set_pg_id(0);
                char oname[32];
                sprintf(oname, "%s%d", "objectname", g_counter);
                request->set_object_name(oname);
                request->set_offset(0);
                char sz[g_io_size + 1];
                memset(sz, 0x55, g_io_size);
                sz[g_io_size] = 0;
                request->set_data(sz);
                stub->process_write(&ctrlr, request, &_wr, _done);
            }
            else if (_type == 0)
            {
                osd::bench_request *request = new osd::bench_request();
                char sz[g_io_size + 1];
                memset(sz, 0x55, g_io_size);
                sz[g_io_size] = 0;
                request->set_req(sz);
                stub->process_rpc_bench(&ctrlr, request, &_br, _done);
            }
            _op_submit_tsc[_last_submitted_id] = spdk_get_ticks();
            _last_submitted_id++;
            _inflight_ops_counter++;
            send_request(target_node_id);
        }
    }
    int get_type()
    {
        return _type;
    }

    uint64_t get_op_tsc(uint64_t id)
    {
        return _op_submit_tsc[id];
    }

    void remove_op_from_inflight_list(uint64_t id)
    {
        _op_submit_tsc.erase(id);
    }

    auto get_wr()
    {
        return _wr;
    }
    auto get_br()
    {
        return _br;
    }

    auto get_server()
    {
        return _s;
    }

    void decrease_inflight_ops()
    {
        _inflight_ops_counter--;
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
    msg::rdma::rpc_controller ctrlr;
    osd::write_reply _wr;
    osd::bench_response _br;
    int _inflight_ops_counter = 0;
    uint64_t _last_submitted_id = 0;

    // ops tick
    std::map<uint64_t, uint64_t> _op_submit_tsc;

    int _type;
    // 每个cpu核上有一个map
    std::vector<std::map<int, std::shared_ptr<osd::rpc_service_osd_Stub>>> _stubs;
};
