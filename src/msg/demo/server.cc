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

#include "base/core_sharded.h"
#include "common.h"

#include "fastblock/msg/rdma/server.h"

#include "ping_pong.pb.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <spdk/event.h>
#include <spdk/string.h>

#include <chrono>
#include <csignal>

namespace {
class demo_ping_pong_service : public ping_pong::ping_pong_service {
public:
    void ping_pong(
      google::protobuf::RpcController* controller,
      const ::ping_pong::request* request,
      ::ping_pong::response* response,
      ::google::protobuf::Closure* done) override {
        // response->set_id(request->id());
        done->Run();
    }

    void heartbeat(
      google::protobuf::RpcController* controller,
      const ::ping_pong::request* request,
      ::ping_pong::response* response,
      ::google::protobuf::Closure* done) override {
        // response->set_id(request->id());
        done->Run();
    }
};

::spdk_cpuset g_cpumask{};
char* g_json_conf{nullptr};

std::shared_ptr<msg::rdma::server> g_rpc_server{nullptr};

demo_ping_pong_service g_rpc_service{};
std::vector<std::unique_ptr<msg::rdma::server>> g_rpc_servers{};
boost::property_tree::ptree g_pt{};
size_t g_rpc_srv_close_counter{0};
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

void on_server_closed(void* arg1, void* arg2) {
    ++g_rpc_srv_close_counter;
    if (g_rpc_srv_close_counter == g_rpc_servers.size()) {
        SPDK_NOTICELOG("All rpc servers closed\n");
        ::spdk_app_stop(0);
    }
}

void on_server_close() {
    SPDK_NOTICELOG("Close the rpc server\n");
    for (auto it = g_rpc_servers.begin(); it != g_rpc_servers.end(); ++it) {
        it->get()->stop([] () {
            auto* evt = ::spdk_event_allocate(::spdk_env_get_first_core(), on_server_closed, nullptr, nullptr);
            ::spdk_event_call(evt);
        });
    }
}

void handle_sigint(int) {
    on_server_close();
}

void start_rpc_server(void* arg) {
    std::vector<uint16_t> ports;
    for (auto& port : g_pt.get_child("bind_ports")) {
        ports.push_back(port.second.get_value<uint16_t>());
    }

    if (static_cast<size_t>(::spdk_env_get_core_count()) < ports.size()) {
        SPDK_ERRLOG(
          "The number of bind ports(%ld) should be equal to the number of cores(%d)\n",
          ports.size(), ::spdk_env_get_core_count());
        std::raise(SIGINT);
    }

    g_rpc_servers.resize(ports.size());
    size_t counter{0};
    for (auto it = core_sharded::system::begin(); it != core_sharded::system::end(); ++it) {
        if (counter >= ports.size()) {
            return;
        }

        auto cpumask = core_sharded::make_cpumake(*it);
        auto opts = msg::rdma::server::make_options(g_pt);
        opts->port = ports.at(counter);
        try {
            auto rpc_srv = std::make_unique<msg::rdma::server>(
              FMT_1("rpc_srv_%1%", *it), cpumask.get(), opts);
            rpc_srv->add_service(&g_rpc_service);
            rpc_srv->create_listener(opts->port);
            rpc_srv->start();
            g_rpc_servers.at(counter) = std::move(rpc_srv);
            counter++;
            SPDK_NOTICELOG("rpc server on core %d, port %d started\n", *it, opts->port);
        } catch (const std::exception& e) {
            SPDK_ERRLOG("Error: Create rpc server failed on core %d, %s\n", *it, e.what());
            std::raise(SIGINT);
            return;
        }
    }
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
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;

    std::signal(SIGINT, handle_sigint);

    rc = ::spdk_app_start(&opts, start_rpc_server, nullptr);
    if (rc) {
        SPDK_ERRLOG("ERROR: Start spdk app failed\n");
    }

    SPDK_NOTICELOG("Exiting from application\n");
    ::spdk_app_fini();

    return rc;
}
