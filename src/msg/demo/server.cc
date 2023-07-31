#include "common.h"

#include "msg/transport_server.h"

#include "ping_pong.pb.h"

#include <spdk/event.h>
#include <spdk/string.h>

#include <csignal>

namespace {
class demo_ping_pong_service : public ping_pong::ping_pong_service {
public:
    void ping_pong(
      google::protobuf::RpcController* controller,
      const ::ping_pong::request* request,
      ::ping_pong::response* response,
      ::google::protobuf::Closure* done) override {
        SPDK_NOTICELOG("Received ping: \"%s\"\n", request->ping().c_str());
        response->set_pong(request->ping());
        done->Run();
    }

    void heartbeat(
      google::protobuf::RpcController* controller,
      const ::ping_pong::request* request,
      ::ping_pong::response* response,
      ::google::protobuf::Closure* done) override {
        SPDK_NOTICELOG("Received ping: \"%s\"\n", request->ping().c_str());
        response->set_pong(request->ping());
        done->Run();
    }
};

struct rpc_context {
    std::shared_ptr<msg::rdma::transport_server>* server{nullptr};
    demo_ping_pong_service* rpc_service{nullptr};
};

char* g_host{};
uint16_t g_port{};
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

void on_server_close() {
    SPDK_NOTICELOG("Close the rpc server\n");
    std::signal(SIGINT, signal_handler);
}

void on_thread_received_msg(void* arg) {
    auto ctx = reinterpret_cast<rpc_context*>(arg);
    SPDK_NOTICELOG("Start RPC server on core %d\n", ::spdk_env_get_current_core());
    std::shared_ptr<msg::rdma::transport_server> srv = *(ctx->server);
    srv->start();
}

void start_rpc_server(void* arg) {
    SPDK_NOTICELOG("Start the rpc server\n");

    auto ctx = reinterpret_cast<rpc_context*>(arg);
    auto rpc_server = *(ctx->server);
    rpc_server->prepare();
    rpc_server->add_service(ctx->rpc_service);
    rpc_server->start_listen(g_host, g_port);

    ::spdk_cpuset tmp_cpumask{};
    uint32_t core_no{0};
    ::spdk_thread* thread{};
    SPDK_ENV_FOREACH_CORE(core_no) {
        ::spdk_cpuset_zero(&tmp_cpumask);
        ::spdk_cpuset_set_cpu(&tmp_cpumask, core_no, true);
        std::string thread_name{fmt::format("rpc_server_poller_{}", core_no)};
        thread = ::spdk_thread_create(thread_name.c_str(), &tmp_cpumask);
        assert(!!thread);
        ::spdk_thread_send_msg(thread, on_thread_received_msg, arg);
        SPDK_NOTICELOG("Setting callback on core %d\n", core_no);
    }
}

int main(int argc, char** argv) {
    ::spdk_app_opts opts{};
    ::spdk_app_opts_init(&opts, sizeof(opts));

    int rc{0};
    if ((rc = ::spdk_app_parse_args(argc, argv, &opts, "H:P:", nullptr, parse_arg, usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        ::exit(rc);
    }

    opts.name = "rdma server";
    opts.shutdown_cb = on_server_close;
    opts.rpc_addr = "/var/tmp/spdk_srv.sock";
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;
    ::spdk_log_set_flag("rdma");
    ::spdk_log_set_flag("transport_server");

    auto rpc_server = std::make_shared<msg::rdma::transport_server>();
    demo_ping_pong_service rpc_service{};
    rpc_context ctx{&rpc_server, &rpc_service};
    rc = ::spdk_app_start(&opts, start_rpc_server, &ctx);
    if (rc) {
        SPDK_ERRLOG("ERROR: Start spdk app failed\n");
    }

    SPDK_NOTICELOG("Exiting from application\n");
    ::spdk_app_fini();

    return rc;
}
