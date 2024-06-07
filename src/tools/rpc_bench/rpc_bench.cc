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

#include "msg/demo/common.h"

#include "msg/rpc_controller.h"
#include "msg/rdma/client.h"
#include "msg/rdma/server.h"
#include "utils/duration_map.h"

#include "tools/rpc_bench/rpc_bench.pb.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/string.h>

#include <cassert>
#include <csignal>
#include <iostream>
#include <ranges>

SPDK_LOG_REGISTER_COMPONENT(rpc_bench)

int g_id{-1};
namespace {
class rpc_bench_service : public rpc_bench::rpc_bench_service {
public:
    void rpc_bench(
      google::protobuf::RpcController* controller,
      const ::rpc_bench::request* request,
      ::rpc_bench::response* response,
      ::google::protobuf::Closure* done) override {
        response->set_data(request->data());
        response->set_id(request->id());
        done->Run();
    }
};

struct endpoint {
    int index{-1};
    std::string host{""};
    uint16_t port{};
};

struct rpc_bench_context {
    bool rpc_client_same_core{false};
    rpc_bench_service rpc_service{};
    std::list<endpoint> endpoints{};
    size_t io_depth{0};
    size_t io_count{0};
    size_t server_count{1};
    size_t peer_count{1};
};

struct call_stack {
    std::unique_ptr<rpc_bench::request> req{std::make_unique<rpc_bench::request>()};
    std::unique_ptr<rpc_bench::response> resp{std::make_unique<rpc_bench::response>()};
    std::unique_ptr<msg::rdma::rpc_controller> ctrlr{std::make_unique<msg::rdma::rpc_controller>()};
    google::protobuf::Closure* cb{nullptr};
    std::chrono::system_clock::time_point start_at{};
    void* conn_context{nullptr};
};

struct connection_context {
    std::shared_ptr<msg::rdma::client::connection> conn{nullptr};
    std::unique_ptr<rpc_bench::rpc_bench_service_Stub> stub{nullptr};
    size_t io_counter{0};
    std::list<std::unique_ptr<call_stack>> call_stacks{};
    int64_t call_id{0};
    double acc_dur{0.0};
    size_t index{0};
};

char* g_json_conf{nullptr};
std::string sock_path{};
int g_ep_index{0};
boost::property_tree::ptree g_pt{};
std::string rpc_msg{};
size_t done_counter{0};
bool is_terminated{false};
rpc_bench_context ctx{};
std::list<std::unique_ptr<msg::rdma::server>> rpc_servers{};
std::list<std::shared_ptr<msg::rdma::client>> rpc_clients{};
std::list<std::unique_ptr<connection_context>> conn_ctxs{};

::spdk_thread* rpc_bench_thread{nullptr};
}

void usage() {
    std::cout << "-C json configuration file path" << std::endl;
    std::cout << "-I endpoint index" << std::endl;
}

int parse_arg(int ch, char* arg) {
    switch (ch) {
    case 'C':
        g_json_conf = arg;
        break;
    case 'I':
        g_ep_index = std::stoi(arg);
        break;
    default:
        throw std::invalid_argument{"Unknown options"};
    }

    return 0;
}

void read_conf() {
    auto& conf_eps = g_pt.get_child("endpoints");
    int current_index{-1};
    bool is_valid_index{false};
    auto eps_it = conf_eps.begin();
    for (; eps_it != conf_eps.end(); ++eps_it) {
        auto& eps_list = eps_it->second.get_child("list");
        current_index = eps_it->second.get_child("index").get_value<int>();
        if (current_index == g_ep_index) {
            is_valid_index = true;
            ctx.server_count = eps_list.size();
        }

        auto list_it = eps_list.begin();
        for (; list_it != eps_list.end(); ++list_it) {
            ctx.endpoints.emplace_back(
              current_index,
              list_it->second.get_child("host").get_value<std::string>(),
              list_it->second.get_child("port").get_value<uint16_t>());
        }
    }
    ctx.peer_count = ctx.endpoints.size() - ctx.server_count;

    if (not is_valid_index) {
        SPDK_ERRLOG("Invalid endpoint index %d\n", g_ep_index);
        ::exit(-1);
    }

    auto cpu_count = ::spdk_env_get_core_count();
    SPDK_DEBUGLOG(
      rpc_bench,
      "current_index: %d, server_count: %lu, peer_count: %lu, core_count: %u\n",
      current_index, ctx.server_count, ctx.peer_count, cpu_count);

    if (ctx.rpc_client_same_core) {
        if (static_cast<size_t>(cpu_count) < ctx.server_count + 1) {
            SPDK_ERRLOG(
              "not enough cpu cores, available %u, at least %lu\n",
              cpu_count, ctx.server_count + 1);
            std::exit(-1);
        }
    } else {
        if (static_cast<size_t>(cpu_count) < ctx.peer_count + ctx.server_count) {
            SPDK_ERRLOG(
              "not enough cpu cores, available %u, at least %lu\n",
              ::spdk_env_get_core_count(), ctx.peer_count + ctx.server_count);
            std::exit(-1);
        }
    }

    ctx.io_depth = g_pt.get_child("io_depth").get_value<size_t>();
    ctx.io_count = g_pt.get_child("io_count").get_value<size_t>();
    auto io_size = g_pt.get_child("io_size").get_value<size_t>();
    rpc_msg = demo::random_string(io_size);
}

/**
 * 如果 rpc_client_same_core 为 true，那么 client 各占一颗核，
 * 与其他 rpc_bench 进程建立的连接都归在同一个 client 实例下。
 *
 * 如果 rpc_client_same_core 为 false，假如有 8 个 rpc_bench 进程，则每个 rpc_bench 进程的
 * client 实例各占一颗核。
*/
void start_rpc_bench_server() {
    auto opts = msg::rdma::server::make_options(g_pt);
    ::spdk_cpuset cpu_mask{};
    uint32_t core_no{0};
    msg::rdma::server* current_srv{nullptr};

    auto ep_begin_it = std::find_if(
      ctx.endpoints.begin(),
      ctx.endpoints.end(),
      [] (const endpoint& ep) { return ep.index == g_ep_index; });

    auto ep_end_it = std::find_if(
      ep_begin_it,
      ctx.endpoints.end(),
      [] (const endpoint& ep) { return ep.index != g_ep_index; });

    SPDK_ENV_FOREACH_CORE(core_no) {
        if (ep_begin_it == ep_end_it) {
            break;
        }

        ::spdk_cpuset_zero(&cpu_mask);
        ::spdk_cpuset_set_cpu(&cpu_mask, core_no, true);
        std::string srv_name{FMT_1("rpc_srv_%1%", core_no)};
        opts->port = ep_begin_it->port;
        opts->bind_address = ep_begin_it->host;
        try {
            SPDK_NOTICELOG(
              "Starting rpc server on %s:%d with index %d\n",
              opts->bind_address.c_str(),
              opts->port,
              ep_begin_it->index);
            auto srv = std::make_unique<msg::rdma::server>(srv_name, cpu_mask, opts);
            current_srv = srv.get();
            rpc_servers.push_back(std::move(srv));
        } catch (const std::exception& e) {
            SPDK_ERRLOG("Error: Create rpc server failed, %s\n", e.what());
            ::exit(-1);
        }
        SPDK_NOTICELOG("Start rpc server on %dth core\n", core_no);
        current_srv->add_service(&(ctx.rpc_service));
        current_srv->start();

        ++ep_begin_it;
    }
}

void on_server_stopped(void* arg) {
    ++done_counter;
    SPDK_INFOLOG(
      rpc_bench,
      "done_counter is %ld, rpc_servers.size() is %ld\n",
      done_counter, rpc_servers.size());
    if (done_counter == rpc_servers.size()) {
        SPDK_NOTICELOG("all rpc servers have been stopped\n");
        ::spdk_thread_exit(rpc_bench_thread);
        ::spdk_app_stop(0);
    }
}

void on_client_stopped(void* arg) {
    ++done_counter;
    SPDK_INFOLOG(
      rpc_bench,
      "done_counter is %ld, rpc_clients.size() is %ld\n",
      done_counter, rpc_clients.size());
    if (done_counter == rpc_clients.size()) {
        SPDK_NOTICELOG("all rpc clients have been stopped\n");
        done_counter = 0;
        for (auto& srv : rpc_servers) {
            srv->stop([] () {
                ::spdk_thread_send_msg(rpc_bench_thread, on_server_stopped, nullptr);
            });
        }
    }
}

void on_client_io_done(void* arg) {
    ++done_counter;
    if (done_counter == conn_ctxs.size()) {
        is_terminated = true;
        done_counter = 0;
        SPDK_NOTICELOG("all rpc finished, stop the app\n");
        for (auto& cli : rpc_clients) {
            cli->stop([] () {
                ::spdk_thread_send_msg(rpc_bench_thread, on_client_stopped, nullptr);
            });
        }
    }
}

void on_pong(call_stack* stack_ptr) {
    if (stack_ptr->ctrlr->Failed()) {
        SPDK_ERRLOG("rpc failed, %s\n", stack_ptr->ctrlr->ErrorText().c_str());
        is_terminated = true;
        std::raise(SIGINT);
    }

    if (is_terminated) { return; }

    auto* conn_ctx = reinterpret_cast<connection_context*>(stack_ptr->conn_context);
    auto index = conn_ctx->index;
    auto resp_id = stack_ptr->resp->id();
    SPDK_NOTICELOG(
      "[%ld] received pong id %ld, total %ld\n",
      index,
      resp_id, ctx.io_count - 1);
    auto dur = (std::chrono::system_clock::now() - stack_ptr->start_at).count();
    auto* call_stacks = &(conn_ctx->call_stacks);
    conn_ctx->call_stacks.pop_front();
    if (static_cast<size_t>(resp_id) >= ctx.io_count - 1 and call_stacks->empty()) {
        SPDK_NOTICELOG("[%ld] all rpc finished\n", index);
        ::spdk_thread_send_msg(rpc_bench_thread, on_client_io_done, nullptr);
        return;
    }

    auto rpc_stack = std::make_unique<call_stack>();
    rpc_stack->req->set_data(rpc_msg);
    rpc_stack->req->set_id(conn_ctx->call_id++);
    rpc_stack->conn_context = conn_ctx;
    rpc_stack->cb = google::protobuf::NewCallback(on_pong, rpc_stack.get());
    rpc_stack->start_at = std::chrono::system_clock::now();
    conn_ctx->stub->rpc_bench(
      rpc_stack->ctrlr.get(),
      rpc_stack->req.get(),
      rpc_stack->resp.get(),
      rpc_stack->cb);
    SPDK_INFOLOG(
      rpc_bench,
      "[%ld] sent rpc id %ld\n",
      conn_ctx->index, conn_ctx->call_id - 1);
    conn_ctx->call_stacks.push_back(std::move(rpc_stack));
}

void on_rpc_bench_close() {
    SPDK_NOTICELOG("Close the rpc_bench\n");
    ::spdk_thread_send_msg(rpc_bench_thread, on_client_stopped, nullptr);
}

void start_ping_client() {
    uint32_t core_no{::spdk_env_get_first_core()};
    for (size_t i{0}; i < ctx.server_count - 1; ++i) {
        core_no = ::spdk_env_get_next_core(core_no);
    }

    auto opts = msg::rdma::client::make_options(g_pt);
    ::spdk_cpuset cpu_mask{};
    auto ep_it = ctx.endpoints.begin();

    int last_index = -1;
    msg::rdma::client* current_cli{nullptr};
    for (; ep_it != ctx.endpoints.end(); ++ep_it) {
        if (ep_it->index == g_ep_index) {
            continue;
        }

        if ((not current_cli) or (not ctx.rpc_client_same_core and ep_it->index != last_index)) {
            last_index = ep_it->index;
            core_no = spdk_env_get_next_core(core_no);
            ::spdk_cpuset_zero(&cpu_mask);
            ::spdk_cpuset_set_cpu(&cpu_mask, core_no, true);
            auto rpc_cli_name = FMT_1("rpc_cli_%1%", core_no);
            auto rpc_cli = std::make_shared<msg::rdma::client>(rpc_cli_name, &cpu_mask, opts);
            current_cli = rpc_cli.get();
            rpc_clients.push_back(rpc_cli);

            current_cli->start();
            SPDK_NOTICELOG("Start rpc client on %dth core\n", core_no);
        }

        SPDK_NOTICELOG(
          "start connecting to %s:%d, with index %d\n",
          ep_it->host.c_str(), ep_it->port, ep_it->index);

        current_cli->emplace_connection(
          ep_it->host, ep_it->port,
          [core_no, ep_it] (bool is_ok, std::shared_ptr<msg::rdma::client::connection> conn) {
              if (not is_ok) {
                  throw std::runtime_error{"create connection failed"};
              }
              SPDK_NOTICELOG(
                "connected to %s:%d, with index %d\n",
                ep_it->host.c_str(), ep_it->port, ep_it->index);
              auto stub = std::make_unique<rpc_bench::rpc_bench_service_Stub>(conn.get());
              auto conn_ctx = std::make_unique<connection_context>(conn, std::move(stub));
              conn_ctx->index = ep_it->index;
              auto conn_ctx_ptr = conn_ctx.get();
              conn_ctxs.push_back(std::move(conn_ctx));
              for (size_t i{0}; i < ctx.io_depth; ++i) {
                  auto rpc_stack = std::make_unique<call_stack>();
                  rpc_stack->req->set_data(rpc_msg);
                  rpc_stack->req->set_id(conn_ctx_ptr->call_id++);
                  rpc_stack->conn_context = conn_ctx_ptr;
                  rpc_stack->cb = google::protobuf::NewCallback(on_pong, rpc_stack.get());
                  rpc_stack->start_at = std::chrono::system_clock::now();
                  conn_ctx_ptr->stub->rpc_bench(
                    rpc_stack->ctrlr.get(),
                    rpc_stack->req.get(),
                    rpc_stack->resp.get(),
                    rpc_stack->cb);
                  SPDK_INFOLOG(
                    rpc_bench,
                    "[%d] sent rpc id %ld\n",
                    ep_it->index,
                    conn_ctx_ptr->call_id - 1);
                  conn_ctx_ptr->call_stacks.push_back(std::move(rpc_stack));
              }
          }
        );
    }
}

void on_rpc_bench_start(void* arg) {
    SPDK_NOTICELOG("Starting rpc_bench with index %d\n", g_ep_index);
    read_conf();
    uint32_t core_no{::spdk_env_get_first_core()};
    ::spdk_cpuset cpu_mask{};
    ::spdk_cpuset_zero(&cpu_mask);
    ::spdk_cpuset_set_cpu(&cpu_mask, core_no, true);
    rpc_bench_thread = ::spdk_thread_create("rpc_bench", &cpu_mask);

    start_rpc_bench_server();
    start_ping_client();
}

int main(int argc, char** argv) {
    ::spdk_app_opts opts{};
    ::spdk_app_opts_init(&opts, sizeof(opts));

    int rc{0};
    if ((rc = ::spdk_app_parse_args(argc, argv, &opts, "C:I:", nullptr, parse_arg, usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        ::exit(rc);
    }

    sock_path = FMT_1("/var/tmp/rpc_bench_%1%.sock", g_ep_index);
    boost::property_tree::read_json(std::string(g_json_conf), g_pt);
    ctx.rpc_client_same_core = g_pt.get_child("rpc_client_same_core").get_value<bool>();

    opts.name = "rpc_bench";
    opts.shutdown_cb = on_rpc_bench_close;
    opts.rpc_addr = sock_path.c_str();
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;

    rc = ::spdk_app_start(&opts, on_rpc_bench_start, nullptr);
    if (rc) {
        SPDK_ERRLOG("ERROR: Start spdk app failed\n");
    }

    SPDK_NOTICELOG("Exiting from application\n");
    ::spdk_app_fini();

    return rc;
}
