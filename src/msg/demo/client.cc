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
#include "utils/duration_map.h"

#include "ping_pong.pb.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <spdk/event.h>
#include <spdk/string.h>

#include <cassert>
#include <csignal>
#include <iostream>
#include <thread>

SPDK_LOG_REGISTER_COMPONENT(client)

int g_id{-1};
namespace {
char* g_host{nullptr};
uint16_t g_port{0};
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

struct call_stack {
    std::unique_ptr<ping_pong::request> req{std::make_unique<ping_pong::request>()};
    std::unique_ptr<ping_pong::response> resp{std::make_unique<ping_pong::response>()};
    google::protobuf::Closure* cb{nullptr};
    std::chrono::system_clock::time_point start_at{};
};

double g_all_rpc_dur{0.0};
size_t g_rpc_dur_count{0};
size_t g_mempool_cap{4096};
size_t g_io_depth{1};
std::list<std::unique_ptr<call_stack>> g_call_stacks{};
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
    SPDK_NOTICELOG_EX("triger signo(%d)\n", signo);
    ::spdk_app_stop(0);
}

void on_client_close() {
    SPDK_NOTICELOG_EX("Close the rpc client\n");
    std::signal(SIGINT, signal_handler);
}

void on_pong(msg::rdma::rpc_controller* ctrlr, ping_pong::response* reply) {
    if (ctrlr->Failed()) {
        SPDK_ERRLOG_EX("ERROR: exec rpc failed: %s\n", ctrlr->ErrorText().c_str());
        std::raise(SIGINT);
    }

    SPDK_NOTICELOG_EX(
      "received pong size: %ld, conetnt: \"%s\"\n",
      reply->pong().size(), reply->pong().c_str());
    assert((reply->pong() == demo::small_message) or (reply->pong() == demo::big_message));
}

void iter_on_pong(msg::rdma::rpc_controller* ctrlr, ping_pong::response* reply) {
    if (ctrlr->Failed()) {
        SPDK_ERRLOG_EX("ERROR: exec rpc failed: %s\n", ctrlr->ErrorText().c_str());
        g_is_terminated = true;
        std::raise(SIGINT);
    }

    if (g_is_terminated) { return; }

    auto* stack_ptr = g_call_stacks.front().get();
    auto dur = (std::chrono::system_clock::now() - stack_ptr->start_at).count();

    ++g_rpc_dur_count;
    g_all_rpc_dur += static_cast<double>(dur);
    SPDK_INFOLOG_EX(client, "received reply %ld\n", reply->id());
    if (static_cast<size_t>(reply->id()) >= g_iter_count - 1) {
        g_is_terminated = true;
        auto iops_dur = static_cast<double>((std::chrono::system_clock::now() - g_iops_start).count());
        SPDK_ERRLOG_EX(
          "client iteration done, duration count is %lu, mean duration is %lfus, total dur: %lfus, iops: %lf\n",
          g_rpc_dur_count,
          ((g_all_rpc_dur / 1000) / g_rpc_dur_count),
          (g_all_rpc_dur / 1000),
          g_rpc_dur_count / (iops_dur / 1000 / 1000 / 1000));

        SPDK_NOTICELOG_EX("Stop the rpc client\n");
        g_rpc_client->stop([] () {
            SPDK_NOTICELOG_EX("Stop the demo client process\n");
            ::spdk_app_stop(0);
        });

        return;
    }

    g_call_stacks.pop_front();
    auto rpc_stack = std::make_unique<call_stack>();
    rpc_stack->cb = google::protobuf::NewCallback(iter_on_pong, &g_ctrlr, rpc_stack->resp.get());
    rpc_stack->req->set_ping(g_iter_msg);
    rpc_stack->req->set_id(g_current_send_rpc++);
    rpc_stack->start_at = std::chrono::system_clock::now();
    g_stub->ping_pong(&g_ctrlr, rpc_stack->req.get(), rpc_stack->resp.get(), rpc_stack->cb);
    g_call_stacks.push_back(std::move(rpc_stack));
}

void start_rpc_client(void* arg) {
    SPDK_NOTICELOG_EX("Start the rpc client, memory pool capacity is %lu\n", g_mempool_cap);

    ::spdk_cpuset_zero(&g_cpumask);
    auto core_no = ::spdk_env_get_first_core();
    ::spdk_cpuset_set_cpu(&g_cpumask, core_no, true);

    auto opts = msg::rdma::client::make_options(g_pt);
    g_iter_count = g_pt.get_child("iteration_count").get_value<size_t>();
    g_io_depth = g_pt.get_child("io_depth").get_value<size_t>();
    std::string cli_name{"rpc_cli"};
    g_rpc_client = std::make_shared<msg::rdma::client>(cli_name, &g_cpumask, opts);
    g_rpc_client->start();
    g_iter_msg = demo::random_string(4096);
    g_rpc_client->emplace_connection(
      g_pt.get_child("server_address").get_value<std::string>(),
      g_pt.get_child("server_port").get_value<uint16_t>(),
      [] (bool is_ok, std::shared_ptr<msg::rdma::client::connection> conn) {
          if (not is_ok) {
              throw std::runtime_error{"create connection failed"};
          }

          g_conn = conn;
          g_stub = std::make_unique<ping_pong::ping_pong_service_Stub>(g_conn.get());

        //   demo::small_message = demo::random_string(demo::small_message_size);
        //   SPDK_NOTICELOG_EX("Sending small message rpc\n");
        //   g_small_ping.set_ping(demo::small_message);
        //   g_small_done = google::protobuf::NewCallback(on_pong, &g_ctrlr, &g_small_pong);
        //   g_stub->ping_pong(&g_ctrlr, &g_small_ping, &g_small_pong, g_small_done);

        //   demo::big_message = demo::random_string(demo::big_message_size);
        //   SPDK_NOTICELOG_EX("Sending heartbeat message rpc\n");
        //   g_big_ping.set_ping(demo::big_message);
        //   g_big_done = google::protobuf::NewCallback(on_pong, &g_ctrlr, &g_big_pong);
        //   g_stub->heartbeat(&g_ctrlr, &g_big_ping, &g_big_pong, g_big_done);

          // iter test

          SPDK_NOTICELOG_EX("Start sending rpc request %ld times\n", g_iter_count);
          std::this_thread::sleep_for(std::chrono::seconds(5));
          g_iops_start = std::chrono::system_clock::now();
          for (size_t i{0}; i < g_io_depth; ++i) {
              auto rpc_stack = std::make_unique<call_stack>();
              rpc_stack->cb = google::protobuf::NewCallback(iter_on_pong, &g_ctrlr, rpc_stack->resp.get());
              rpc_stack->req->set_ping(g_iter_msg);
              rpc_stack->req->set_id(g_current_send_rpc++);
              rpc_stack->start_at = std::chrono::system_clock::now();
              g_stub->ping_pong(&g_ctrlr, rpc_stack->req.get(), rpc_stack->resp.get(), rpc_stack->cb);
              g_call_stacks.push_back(std::move(rpc_stack));
          }
      });
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
    opts.rpc_addr = "/var/tmp/msg_demo_cli.sock";
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;

    rc = ::spdk_app_start(&opts, start_rpc_client, nullptr);
    if (rc) {
        SPDK_ERRLOG_EX("ERROR: Start spdk app failed\n");
    }

    SPDK_NOTICELOG_EX("Exiting from application\n");
    ::spdk_app_fini();

    return rc;
}
