#include "common.h"

#include "msg/rpc_controller.h"
#include "msg/transport_client.h"

#include "ping_pong.pb.h"

#include <spdk/event.h>
#include <spdk/string.h>

#include <cassert>
#include <csignal>

namespace {
char* g_host{nullptr};
uint16_t g_port{0};
std::unique_ptr<msg::rdma::transport_client> g_rpc_client{nullptr};
msg::rdma::transport_client::connection::id_type g_id{0};
std::shared_ptr<msg::rdma::transport_client::connection> g_conn{nullptr};
ping_pong::request g_small_ping{};
ping_pong::response g_small_pong{};
ping_pong::request g_big_ping{};
ping_pong::response g_big_pong{};
std::unique_ptr<ping_pong::ping_pong_service_Stub> g_stub{nullptr};
google::protobuf::Closure* g_small_done{nullptr};
google::protobuf::Closure* g_big_done{nullptr};
msg::rdma::rpc_controller g_ctrlr{};
std::string g_ping_message(4096, ' ');
}

void usage() {
    ::printf(" -H host_addr\n");
    ::printf(" -P port\n");
}

int parse_arg(int ch, char* arg) {
    switch (ch) {
    case 'H':
        g_host = arg;
        break;
    case 'P':
        g_port = static_cast<uint16_t>(::spdk_strtol(arg, 10));
        break;
    default:
        throw std::invalid_argument{"Unknown options"};
    }

    return 0;
}

void signal_handler(int signo) noexcept {
    SPDK_NOTICELOG("triger signo(%d)", signo);
    ::spdk_app_stop(0);
}

void on_client_close() {
    SPDK_NOTICELOG("Close the rpc server\n");
    std::signal(SIGINT, signal_handler);
}

void on_pong(msg::rdma::rpc_controller* ctrlr, ping_pong::response* reply) {
    if (ctrlr->Failed()) {
        SPDK_ERRLOG("ERROR: exec rpc failed: %s\n", ctrlr->ErrorText().c_str());
        std::raise(SIGINT);
    }

    SPDK_NOTICELOG(
      "received pong size: %ld, conetnt: \"%s\"\n",
      reply->pong().size(), reply->pong().c_str());
    assert((reply->pong() == demo::small_message) or (reply->pong() == demo::big_message));
}

void start_rpc_client(void* arg) {
    SPDK_NOTICELOG("Start the rpc client\n");

    g_rpc_client = std::make_unique<msg::rdma::transport_client>();
    g_rpc_client->start();
    // FIXME: hard code
    g_conn = g_rpc_client->emplace_connection(g_id, std::string{g_host}, g_port);
    g_stub = std::make_unique<ping_pong::ping_pong_service_Stub>(g_conn.get());

    demo::small_message = demo::random_string(demo::small_message_size);
    SPDK_NOTICELOG(
      "Send small ping message size: %ld, content: \"%s\"\n",
      demo::small_message.size(), demo::small_message.c_str());
    g_small_ping.set_ping(demo::small_message);
    g_small_done = google::protobuf::NewCallback(on_pong, &g_ctrlr, &g_small_pong);
    g_stub->ping_pong(&g_ctrlr, &g_small_ping, &g_small_pong, g_small_done);

    demo::big_message = demo::random_string(demo::big_message_size);
    SPDK_NOTICELOG(
      "Send heartbeat message size: %ld, content: \"%s\"\n",
      demo::big_message.size(), demo::big_message.c_str());
    g_big_ping.set_ping(demo::big_message);
    g_big_done = google::protobuf::NewCallback(on_pong, &g_ctrlr, &g_big_pong);
    g_stub->heartbeat(&g_ctrlr, &g_big_ping, &g_big_pong, g_big_done);
}

int main(int argc, char** argv) {
    ::spdk_app_opts opts{};
    ::spdk_app_opts_init(&opts, sizeof(opts));

    int rc{0};
    if ((rc = ::spdk_app_parse_args(argc, argv, &opts, "H:P:", nullptr, parse_arg, usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        ::exit(rc);
    }

    opts.name = "rdma client";
    opts.shutdown_cb = on_client_close;
    opts.rpc_addr = "/var/tmp/spdk_cli.sock";
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;
    ::spdk_log_set_flag("rdma");
    ::spdk_log_set_flag("transport_client");
    ::spdk_log_set_print_level(SPDK_LOG_DEBUG);

    rc = ::spdk_app_start(&opts, start_rpc_client, nullptr);
    if (rc) {
        SPDK_ERRLOG("ERROR: Start spdk app failed\n");
    }

    SPDK_NOTICELOG("Exiting from application\n");
    ::spdk_app_fini();

    return rc;
}
