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

#include "common.h"

#include "msg/rpc_controller.h"
#include "msg/rdma/client.h"
#include "msg/rdma/server.h"
#include "utils/duration_map.h"

#include "ping_pong.pb.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/string.h>

#include <cassert>
#include <csignal>
#include <iostream>

SPDK_LOG_REGISTER_COMPONENT(ping_pong)

int g_id{-1};
namespace {
class demo_ping_pong_service : public ping_pong::ping_pong_service {
public:
    void ping_pong(
      google::protobuf::RpcController* controller,
      const ::ping_pong::request* request,
      ::ping_pong::response* response,
      ::google::protobuf::Closure* done) override {
        response->set_pong(request->ping());
        response->set_id(request->id());
        done->Run();
    }

    void heartbeat(
      google::protobuf::RpcController* controller,
      const ::ping_pong::request* request,
      ::ping_pong::response* response,
      ::google::protobuf::Closure* done) override {
        response->set_pong(request->ping());
        response->set_id(request->id());
        done->Run();
    }
};

struct endpoint {
    int index{-1};
    std::string host{""};
    uint16_t port{};
};

struct ping_pong_context {
    bool use_different_core{false};
    demo_ping_pong_service rpc_service{};
    std::list<endpoint> endpoints{};
};

char* g_json_conf{nullptr};
int g_index{0};
boost::property_tree::ptree g_pt{};

ping_pong_context ctx{};
std::list<std::unique_ptr<msg::rdma::server>> rpc_servers{};
std::list<std::unique_ptr<msg::rdma::client>> rpc_clients{};
}

void usage() {
    std::cout << "-C json configuration file path" << std::endl;
}

int parse_arg(int ch, char* arg) {
    switch (ch) {
    case 'C':
        g_json_conf = arg;
        break;
    case 'I':
        g_index = std::stoi(arg);
        break;
    default:
        throw std::invalid_argument{"Unknown options"};
    }

    return 0;
}

void read_endpoints() {
    auto& conf_eps = g_pt.get_child("endpoints");
    int current_index{-1};
    size_t server_count{0};
    size_t client_count{0};
    bool is_valid_index{false};
    auto eps_it = conf_eps.begin();
    for (; eps_it != conf_eps.end(); ++eps_it) {
        auto& eps_list = conf_eps.get_child("list");
        current_index = eps_it->second.get_child("index").get_value<int>();
        if (current_index == g_index) {
            is_valid_index = true;
            server_count = eps_list.size();
        } else {
            client_count += eps_list.size();
        }
        auto list_it = eps_list.begin();
        for (; list_it != eps_list.end(); ++list_it) {
            ctx.endpoints.emplace_back(
              current_index,
              list_it->second.get_child("host").get_value<std::string>(),
              list_it->second.get_child("port").get_value<uint16_t>());
        }
    }

    if (not is_valid_index) {
        SPDK_ERRLOG_EX("Invalid endpoint index %d\n", g_index);
        ::exit(-1);
    }

    auto cpu_count = ::spdk_env_get_core_count();
    SPDK_DEBUGLOG_EX(
      ping_pong,
      "current_index: %d, server_count: %lu, client_count: %lu, core_count: %u\n",
      current_index, server_count, client_count, cpu_count);

    if (ctx.use_different_core) {
        if (static_cast<size_t>(cpu_count) < client_count + server_count) {
            SPDK_ERRLOG_EX(
              "not enough cpu cores, available %u, at least %lu\n",
              cpu_count, server_count + client_count);
            std::exit(-1);
        }
    }
}

void on_ping_pong_close() {
    SPDK_NOTICELOG_EX("Close the ping_pong\n");
}

/**
 * 如果 use_different_core 为 true，那么 server 和 client 总是从第一颗核开始，
 * 分别创建 server_core_count 和 client_core_count 个 spdk_thread
 *
 * 如果 use_different_core 为 false，那么 server 使用前 server_core_count 个 核心，client 使用剩余的
*/
void start_ping_pong_server() {
    auto opts = msg::rdma::server::make_options(g_pt);
    ::spdk_cpuset cpu_mask{};
    uint32_t core_no{0};
    msg::rdma::server* current_srv{nullptr};

    auto ep_begin_it = std::find_if(
      ctx.endpoints.begin(),
      ctx.endpoints.end(),
      [] (const endpoint& ep) { return ep.index == g_index; });

    auto ep_end_it = std::find_if(
      ep_begin_it,
      ctx.endpoints.end(),
      [] (const endpoint& ep) { return ep.index != g_index });

    SPDK_ENV_FOREACH_CORE(core_no) {
        if (ep_begin_it == ep_end_it) {
            break;
        }

        ::spdk_cpuset_zero(&cpu_mask);
        ::spdk_cpuset_set_cpu(&cpu_mask, core_no, true);
        std::string srv_name{FMT_1("rpc_srv_%1%", core_no)};
        try {
            auto srv = std::make_unique<msg::rdma::server>(srv_name, cpu_mask, opts);
            current_srv = srv.get();
            rpc_servers.push_back(std::move(srv));
        } catch (const std::exception& e) {
            SPDK_ERRLOG_EX("Error: Create rpc server failed, %s\n", e.what());
            ::exit(-1);
        }
        SPDK_NOTICELOG_EX("Start rpc server on %dth core\n", core_no);
        current_srv->add_service(&(ctx.rpc_service));
        current_srv->start();

        ++ep_begin_it;
    }
}

void on_ping_pong_start(void* arg) {
    SPDK_NOTICELOG_EX("Starting ping_pong\n");
    read_endpoints();
}

int main(int argc, char** argv) {
    ::spdk_app_opts opts{};
    ::spdk_app_opts_init(&opts, sizeof(opts));

    int rc{0};
    if ((rc = ::spdk_app_parse_args(argc, argv, &opts, "C:I:", nullptr, parse_arg, usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        ::exit(rc);
    }

    SPDK_NOTICELOG_EX("run ping_pong with index %d\n", g_index);
    boost::property_tree::read_json(std::string(g_json_conf), g_pt);
    ctx.use_different_core = g_pt.get_child("use_different_core").get_value<bool>();
    ctx.server_core_count = g_pt.get_child("server_core_count").get_value<int>();
    ctx.client_core_count = g_pt.get_child("client_core_count").get_value<int>();
    if (ctx.use_different_core and ::spdk_env_get_core_count() < ctx.server_core_count + ctx.client_core_count) {
        SPDK_ERRLOG_EX("Core count should >= %d\n", ctx.server_core_count + ctx.client_core_count);
        ::exit(-1);
    }

    if (not ctx.use_different_core and
       ::spdk_env_get_core_count() < std::max(ctx.server_core_count, ctx.client_core_count)) {
        SPDK_ERRLOG_EX(
          "Not enough cpu cores, at least %d\n",
          std::max(ctx.server_core_count, ctx.client_core_count));
        ::exit(-1);
    }
    read_endpoints();

    opts.name = "ping_pong";
    opts.shutdown_cb = on_ping_pong_close;
    opts.rpc_addr = FMT_1("/var/tmp/ping_pong_%1%.sock", g_index).c_str();
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;

    rc = ::spdk_app_start(&opts, on_ping_pong_start, &ctx);
    if (rc) {
        SPDK_ERRLOG_EX("ERROR: Start spdk app failed\n");
    }

    SPDK_NOTICELOG_EX("Exiting from application\n");
    ::spdk_app_fini();

    return rc;
}
