/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/sock/server.h"

#include "ping_pong.pb.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <spdk/event.h>
#include <spdk/log.h>

#include <csignal>
#include <iostream>

namespace {

class svc : public ping_pong::ping_pong_service {
public:
    void ping_pong(google::protobuf::RpcController*, const ping_pong::request* request,
                   ping_pong::response* response, google::protobuf::Closure* done) override {
        if (request->ping() == "hold") {
            return;
        }
        response->set_id(request->id());
        done->Run();
    }
    void heartbeat(google::protobuf::RpcController*, const ping_pong::request* request,
                   ping_pong::response* response, google::protobuf::Closure* done) override {
        response->set_id(request->id());
        done->Run();
    }
};

char* g_json{nullptr};
boost::property_tree::ptree g_pt;
std::unique_ptr<msg::sock::server> g_srv;
svc g_svc;

void usage() { std::cout << "-C json configuration file path\n"; }

int parse_arg(int ch, char* arg) {
    if (ch == 'C') {
        g_json = arg;
        return 0;
    }
    return -EINVAL;
}

void on_close() {
    if (g_srv) {
        g_srv->stop([]() { ::spdk_app_stop(0); });
        return;
    }
    ::spdk_app_stop(0);
}

void start(void*) {
    auto opts = msg::sock::options::make(g_pt);
    opts->bind_address = g_pt.get<std::string>("bind_address", "127.0.0.1");
    opts->port = g_pt.get<uint16_t>("bind_port", 18080);
    g_srv = std::make_unique<msg::sock::server>(::spdk_get_thread(), opts);
    g_srv->add_service(&g_svc);
    try {
        g_srv->create_listener(opts->port);
    } catch (const std::exception& e) {
        SPDK_ERRLOG("listen failed: %s\n", e.what());
        std::raise(SIGINT);
        return;
    }
    g_srv->start();
    SPDK_NOTICELOG("sock server listening on %s:%u\n", opts->bind_address.c_str(), opts->port);
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
    opts.name = "sock_server";
    opts.shutdown_cb = on_close;
    opts.reactor_mask = "0x1";
    const int rc = ::spdk_app_start(&opts, start, nullptr);
    ::spdk_app_fini();
    return rc;
}
