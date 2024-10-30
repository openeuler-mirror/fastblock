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
#include "msg/rpc_controller.h"
#include "msg/rdma/client.h"
#include "utils/duration_map.h"

#include "ping_pong.pb.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <spdk/event.h>
#include <spdk/string.h>

#include <algorithm>
#include <cassert>
#include <csignal>
#include <iostream>

SPDK_LOG_REGISTER_COMPONENT(demo_cli)

namespace {

struct call_stack {
    std::unique_ptr<ping_pong::request> req{std::make_unique<ping_pong::request>()};
    std::unique_ptr<ping_pong::response> resp{std::make_unique<ping_pong::response>()};
    google::protobuf::Closure* cb{nullptr};
    std::chrono::system_clock::time_point start_at{};
};

struct rpc_client_context {
    std::shared_ptr<msg::rdma::client> client{nullptr};
    std::shared_ptr<msg::rdma::client::connection> conn{nullptr};
    std::unique_ptr<ping_pong::ping_pong_service_Stub> stub{nullptr};
    std::list<std::unique_ptr<call_stack>> call_stacks{};
    double acc_duration{0.0};
    int64_t count{0};
    int64_t done_count{0};
    int64_t total_iter_size{0};
    std::chrono::system_clock::time_point iops_start_at{};
    bool done{false};
};

std::vector<rpc_client_context> g_rpc_clients{};
size_t g_done_counter{0};


std::shared_ptr<msg::rdma::client> g_rpc_client{nullptr};
std::shared_ptr<msg::rdma::client::connection> g_conn{nullptr};
ping_pong::request g_small_ping{};
ping_pong::response g_small_pong{};
ping_pong::request g_big_ping{};
ping_pong::response g_big_pong{};
std::unique_ptr<ping_pong::ping_pong_service_Stub> g_stub{nullptr};
google::protobuf::Closure* g_small_done{nullptr};
google::protobuf::Closure* g_big_done{nullptr};
msg::rdma::rpc_controller g_ctrlr{};
std::string g_ping_message(4096, ' ');
size_t g_iter_count{0};
long g_current_send_rpc{0};
::spdk_cpuset g_cpumask{};
std::string g_iter_msg{};
bool g_is_terminated{false};



double g_all_rpc_dur{0.0};
size_t g_rpc_dur_count{0};
size_t g_io_depth{1};
std::chrono::system_clock::time_point g_iops_start{};
char* g_json_conf{nullptr};
boost::property_tree::ptree g_pt{};
}

void usage() {
    std::cout << "-C json configuration file path" << std::endl;
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

void signal_handler(int signo) noexcept {
    SPDK_NOTICELOG("triger signo(%d)\n", signo);
    ::spdk_app_stop(0);
}

void on_client_close() {
    SPDK_NOTICELOG("Close the rpc client\n");
    std::signal(SIGINT, signal_handler);
}

void on_pong(msg::rdma::rpc_controller* ctrlr, ping_pong::response* reply) {
    if (ctrlr->Failed()) {
        SPDK_ERRLOG("ERROR: exec rpc failed: %s\n", ctrlr->ErrorText().c_str());
        std::raise(SIGINT);
    }
}

void on_rpc_client_closed(void* arg1, void* arg2) {
    ++g_done_counter;
    if (g_done_counter != g_rpc_clients.size()) {
        return;
    }

    SPDK_NOTICELOG("Stop the demo client process\n");
    ::spdk_app_stop(0);
}

void on_core_iter_done(void* arg1, void* arg2) {
    ++g_done_counter;
    if (g_done_counter == g_rpc_clients.size()) {
        auto now = std::chrono::system_clock::now();
        auto min_start_at_it = std::min_element(
          g_rpc_clients.begin(),
          g_rpc_clients.end(),
          [] (const rpc_client_context& ctx_a, const rpc_client_context& ctx_b) {
              return ctx_a.iops_start_at < ctx_b.iops_start_at;
          }
        );

        auto iops_dur = static_cast<double>((now - min_start_at_it->iops_start_at).count());
        auto iops_count = g_rpc_clients.at(0).total_iter_size * g_rpc_clients.size();

        double mean{0.0};
        for (auto it = g_rpc_clients.begin(); it != g_rpc_clients.end(); ++it) {
            mean += it->acc_duration;
        }
        mean /= iops_count;

        SPDK_ERRLOG("==============================================================================================\n");
        SPDK_ERRLOG(
          "core count: %d, duration count: %ld, mean duration is %lfus, total dur: %lfs, iops: %lf, iodepth: %ld\n",
          ::spdk_env_get_core_count(),
          iops_count,
          mean / 1000,
          iops_dur / 1000 / 1000 / 1000,
          iops_count / (iops_dur / 1000 / 1000 / 1000), g_io_depth);
        SPDK_ERRLOG("==============================================================================================\n");

        g_done_counter = 0;
        for  (auto it = g_rpc_clients.begin(); it != g_rpc_clients.end(); ++it) {
            it->client->stop([] () {
                auto* evt = ::spdk_event_allocate(::spdk_env_get_first_core(), on_rpc_client_closed, nullptr, nullptr);
                ::spdk_event_call(evt);
            });
        }
    }
}

void iter_on_pong(rpc_client_context* ctx) {
    if (g_ctrlr.Failed()) {
        SPDK_ERRLOG("ERROR: exec rpc failed: %s\n", g_ctrlr.ErrorText().c_str());
        g_is_terminated = true;
        std::raise(SIGINT);
    }

    if (ctx->done) { return; }

    auto* stack_ptr = ctx->call_stacks.front().get();
    auto dur = (std::chrono::system_clock::now() - stack_ptr->start_at).count();
    ctx->acc_duration += static_cast<double>(dur);
    ctx->done_count += 1;

    SPDK_DEBUGLOG(
      demo_cli,
      "received reply id %ld on core %d\n",
      stack_ptr->resp->id(), ::spdk_env_get_current_core());
    g_all_rpc_dur += static_cast<double>(dur);
    if (ctx->done_count >= ctx->total_iter_size - 1) {
        ctx->done = true;
        auto* evt = ::spdk_event_allocate(::spdk_env_get_first_core(), on_core_iter_done, nullptr, nullptr);
        ::spdk_event_call(evt);

        return;
    }

    ctx->call_stacks.pop_front();
    auto rpc_stack = std::make_unique<call_stack>();
    rpc_stack->cb = google::protobuf::NewCallback(iter_on_pong, ctx);
    rpc_stack->req->set_ping(g_iter_msg);
    rpc_stack->req->set_id((ctx->count)++);
    rpc_stack->start_at = std::chrono::system_clock::now();
    ctx->stub->ping_pong(&g_ctrlr, rpc_stack->req.get(), rpc_stack->resp.get(), rpc_stack->cb);
    ctx->call_stacks.push_back(std::move(rpc_stack));
}

void start_rpc_client(void* arg) {
    std::vector<uint16_t> ports{};
    for (auto& port : g_pt.get_child("server_ports")) {
        ports.push_back(port.second.get_value<uint16_t>());
    }

    if (::spdk_env_get_core_count() < ports.size()) {
        SPDK_ERRLOG("ERROR: core count not match with server ports\n");
        std::raise(SIGINT);
    }

    g_io_depth = g_pt.get_child("io_depth").get_value<size_t>();
    g_iter_msg = demo::random_string(4096);
    auto srv_addr = g_pt.get_child("server_address").get_value<std::string>();

    size_t counter{0};
    g_rpc_clients.resize(ports.size());
    for (auto core_it = core_sharded::system::begin(); core_it != core_sharded::system::end(); ++core_it) {
        if (counter >= ports.size()) {
            return;
        }

        auto& ctx = g_rpc_clients.at(counter);
        ctx.total_iter_size = g_pt.get_child("iteration_count").get_value<int64_t>();

        auto opts = msg::rdma::client::make_options(g_pt);
        auto mask = core_sharded::make_cpumake(*core_it);
        ctx.client = std::make_shared<msg::rdma::client>(
          FMT_1("rpc_cli_%d", ::spdk_env_get_current_core()), mask.get(), opts);
        ctx.client->start();
        ctx.client->emplace_connection(
          srv_addr, ports.at(counter),
          [ctx = &ctx] (bool is_ok, std::shared_ptr<msg::rdma::client::connection> conn) {
              if (not is_ok) {
                  throw std::runtime_error{"create connection failed"};
              }

              ctx->conn = conn;
              ctx->stub = std::make_unique<ping_pong::ping_pong_service_Stub>(conn.get());

              SPDK_NOTICELOG(
                "Start sending rpc request %ld times on core %d\n",
                ctx->total_iter_size, ::spdk_env_get_current_core());
              ctx->iops_start_at = std::chrono::system_clock::now();
              for (size_t i{0}; i < g_io_depth; ++i) {
                  auto rpc_stack = std::make_unique<call_stack>();
                  rpc_stack->cb = google::protobuf::NewCallback(iter_on_pong, ctx);
                  rpc_stack->req->set_ping(g_iter_msg);
                  rpc_stack->req->set_id((ctx->count)++);
                  rpc_stack->start_at = std::chrono::system_clock::now();
                  ctx->stub->ping_pong(&g_ctrlr, rpc_stack->req.get(), rpc_stack->resp.get(), rpc_stack->cb);
                  ctx->call_stacks.push_back(std::move(rpc_stack));
              }
          }
        );

        counter++;
    }


    // ::spdk_cpuset_zero(&g_cpumask);
    // auto core_no = ::spdk_env_get_first_core();
    // ::spdk_cpuset_set_cpu(&g_cpumask, core_no, true);

    // auto opts = msg::rdma::client::make_options(g_pt);
    // g_iter_count = g_pt.get_child("iteration_count").get_value<size_t>();
    // g_io_depth = g_pt.get_child("io_depth").get_value<size_t>();
    // std::string cli_name{"rpc_cli"};
    // g_rpc_client = std::make_shared<msg::rdma::client>(cli_name, &g_cpumask, opts);
    // g_rpc_client->start();
    // g_iter_msg = demo::random_string(4096);
    // g_rpc_client->emplace_connection(
    //   g_pt.get_child("server_address").get_value<std::string>(),
    //   g_pt.get_child("server_port").get_value<uint16_t>(),
    //   [] (bool is_ok, std::shared_ptr<msg::rdma::client::connection> conn) {
    //       if (not is_ok) {
    //           throw std::runtime_error{"create connection failed"};
    //       }

    //       g_conn = conn;
    //       g_stub = std::make_unique<ping_pong::ping_pong_service_Stub>(g_conn.get());

    //       SPDK_NOTICELOG("Start sending rpc request %ld times\n", g_iter_count);
    //       g_iops_start = std::chrono::system_clock::now();
    //       for (size_t i{0}; i < g_io_depth; ++i) {
    //           auto rpc_stack = std::make_unique<call_stack>();
    //           rpc_stack->cb = google::protobuf::NewCallback(iter_on_pong, &g_ctrlr, rpc_stack->resp.get());
    //           rpc_stack->req->set_ping(g_iter_msg);
    //           rpc_stack->req->set_id(g_current_send_rpc++);
    //           rpc_stack->start_at = std::chrono::system_clock::now();
    //           g_stub->ping_pong(&g_ctrlr, rpc_stack->req.get(), rpc_stack->resp.get(), rpc_stack->cb);
    //           g_call_stacks.push_back(std::move(rpc_stack));
    //       }
    //  });
}

int main(int argc, char** argv) {
    ::spdk_app_opts opts{};
    ::spdk_app_opts_init(&opts, sizeof(opts));

    int rc{0};
    if ((rc = ::spdk_app_parse_args(argc, argv, &opts, "C:", nullptr, parse_arg, usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        ::exit(rc);
    }

    boost::property_tree::read_json(std::string(g_json_conf), g_pt);

    opts.name = "demo_client";
    opts.shutdown_cb = on_client_close;
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;

    rc = ::spdk_app_start(&opts, start_rpc_client, nullptr);
    if (rc) {
        SPDK_ERRLOG("ERROR: Start spdk app failed\n");
    }

    SPDK_NOTICELOG("Exiting from application\n");
    ::spdk_app_fini();

    return rc;
}
