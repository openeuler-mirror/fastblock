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

#include "msg/rdma/server.h"

#include "ping_pong.pb.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <spdk/event.h>
#include <spdk/string.h>

#include <csignal>

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

struct rpc_context {
    std::shared_ptr<msg::rdma::server> server{nullptr};
    demo_ping_pong_service* rpc_service{nullptr};
};

::spdk_cpuset g_cpumask{};
char* g_json_conf{nullptr};
std::shared_ptr<msg::rdma::server> g_rpc_server{nullptr};
boost::property_tree::ptree g_pt{};
}

void usage() {
    ::printf(" -C json conf file path\n");
}

int parse_arg(int ch, char* arg) {
    switch (ch) {
    case 'C':
        g_json_conf = arg;
        break;
    default:
        throw std::invalid_argument{"Unknown options"};
    }

    return 0;
}

void on_server_close() {
    SPDK_NOTICELOG_EX("Close the rpc server\n");
    if (g_rpc_server) {
        g_rpc_server->stop([] () {
            ::spdk_app_stop(0);
        });
    } else {
        ::spdk_app_stop(0);
    }
}

void handle_sigint(int) {
    on_server_close();
}

void start_rpc_server(void* arg) {
    auto ctx = reinterpret_cast<rpc_context*>(arg);

    ::spdk_cpuset_zero(&g_cpumask);
    auto core_no = ::spdk_env_get_first_core();
    ::spdk_cpuset_set_cpu(&g_cpumask, core_no, true);

    auto opts = msg::rdma::server::make_options(g_pt);
    opts->port = g_pt.get_child("bind_port").get_value<uint16_t>();
    std::string srv_name{"rpc_srv"};
    try {
        g_rpc_server = std::make_shared<msg::rdma::server>(srv_name, g_cpumask, opts);
    } catch (const std::exception& e) {
        SPDK_ERRLOG_EX("Error: Create rpc server failed, %s\n", e.what());
        std::raise(SIGINT);
        return;
    }
    g_rpc_server->add_service(ctx->rpc_service);
    g_rpc_server->start();
}

int main(int argc, char** argv) {
    ::spdk_app_opts opts{};
    ::spdk_app_opts_init(&opts, sizeof(opts));

    int rc{0};
    if ((rc = ::spdk_app_parse_args(argc, argv, &opts, "C:", nullptr, parse_arg, usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        ::exit(rc);
    }

    boost::property_tree::read_json(std::string(g_json_conf), g_pt);

    opts.name = "demo_server";
    opts.shutdown_cb = on_server_close;
    opts.rpc_addr = "/var/tmp/spdk_srv.sock";
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;

    std::signal(SIGINT, handle_sigint);

    demo_ping_pong_service rpc_service{};
    rpc_context ctx{nullptr, &rpc_service};
    rc = ::spdk_app_start(&opts, start_rpc_server, &ctx);
    if (rc) {
        SPDK_ERRLOG_EX("ERROR: Start spdk app failed\n");
    }

    SPDK_NOTICELOG_EX("Exiting from application\n");
    ::spdk_app_fini();

    return rc;
}
