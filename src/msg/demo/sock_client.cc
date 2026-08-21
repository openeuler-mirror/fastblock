/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/rpc_controller.h"
#include "fastblock/msg/sock/client.h"

#include "ping_pong.pb.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <google/protobuf/stubs/callback.h>
#include <spdk/event.h>
#include <spdk/log.h>

#include <chrono>
#include <iostream>
#include <memory>

namespace {

char* g_json{nullptr};
boost::property_tree::ptree g_pt;
std::shared_ptr<msg::sock::client> g_cli;
std::shared_ptr<msg::sock::client::connection> g_conn;
std::unique_ptr<ping_pong::ping_pong_service_Stub> g_stub;
int g_pending{0};
int g_failed{0};

void usage() { std::cout << "-C json configuration file path\n"; }

int parse_arg(int ch, char* arg) {
    if (ch == 'C') {
        g_json = arg;
        return 0;
    }
    return -EINVAL;
}

void done_one() {
    if (--g_pending == 0) {
        SPDK_NOTICELOG("sock client finished, failed=%d\n", g_failed);
        g_cli->stop([]() { ::spdk_app_stop(g_failed ? 1 : 0); });
    }
}

struct call {
    ping_pong::request req;
    ping_pong::response resp;
    msg::rdma::rpc_controller ctrl;
};

void fire(const std::string& ping, int64_t id, bool expect_fail = false) {
    auto* c = new call{};
    c->req.set_id(id);
    c->req.set_ping(ping);
    ++g_pending;
    g_stub->ping_pong(
      &c->ctrl, &c->req, &c->resp,
      google::protobuf::NewCallback(+[](call* c, bool expect_fail) {
          if (c->ctrl.Failed()) {
              if (!expect_fail) {
                  SPDK_ERRLOG("rpc failed: %s\n", c->ctrl.ErrorText().c_str());
                  ++g_failed;
              }
          } else if (expect_fail) {
              SPDK_ERRLOG("expected failure for id %ld\n", c->req.id());
              ++g_failed;
          } else if (c->resp.id() != c->req.id()) {
              SPDK_ERRLOG("id mismatch\n");
              ++g_failed;
          }
          delete c;
          done_one();
      }, c, expect_fail));
}

void on_connected(bool ok, std::shared_ptr<msg::sock::connection> conn) {
    if (!ok || !conn) {
        SPDK_ERRLOG("connect failed\n");
        ::spdk_app_stop(1);
        return;
    }
    g_conn = std::move(conn);
    g_stub = std::make_unique<ping_pong::ping_pong_service_Stub>(g_conn.get());

    fire("hello", 1);
    fire(std::string(4U * 1024U * 1024U, 'Z'), 2);
    for (int i = 0; i < 32; ++i) {
        fire("qd", 100 + i);
    }
    fire("hold", 3, true);
    g_conn->inject_unknown_frame();
    ++g_pending;
    auto* hb = new call{};
    hb->req.set_id(4);
    hb->req.set_ping("hb");
    g_stub->heartbeat(&hb->ctrl, &hb->req, &hb->resp,
                      google::protobuf::NewCallback(+[](call* hb) {
                          if (hb->ctrl.Failed()) {
                              ++g_failed;
                          }
                          delete hb;
                          done_one();
                      }, hb));
}

void start(void*) {
    auto opts = msg::sock::options::make(g_pt);
    opts->rpc_timeout = std::chrono::milliseconds{200};
    g_cli = std::make_shared<msg::sock::client>("sock_cli", ::spdk_get_thread(), opts);
    g_cli->start([opts]() {
        const auto addr = g_pt.get<std::string>("server_address", "127.0.0.1");
        const auto port = g_pt.get<uint16_t>("server_port", 18080);
        g_cli->emplace_connection(addr, port, on_connected);
    });
}

void on_close() {
    if (g_cli) {
        g_cli->stop([]() { ::spdk_app_stop(g_failed ? 1 : 0); });
        return;
    }
    ::spdk_app_stop(0);
}

} // namespace

int main(int argc, char** argv) {
    ::spdk_app_opts opts{};
    ::spdk_app_opts_init(&opts, sizeof(opts));
    if (::spdk_app_parse_args(argc, argv, &opts, "C:", nullptr, parse_arg, usage) !=
        SPDK_APP_PARSE_ARGS_SUCCESS) {
        return 1;
    }
    if (g_json) {
        boost::property_tree::read_json(g_json, g_pt);
    }
    opts.name = "sock_client";
    opts.shutdown_cb = on_close;
    // Distinct from sock_server (0x1) so both can run on one machine.
    opts.reactor_mask = "0x2";
    const int rc = ::spdk_app_start(&opts, start, nullptr);
    ::spdk_app_fini();
    return rc;
}
