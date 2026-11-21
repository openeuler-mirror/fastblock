/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include "fastblock/msg/channel.h"
#include "fastblock/msg/completed_key_ring.h"
#include "fastblock/msg/rpc_controller.h"
#include "fastblock/msg/sock/frame.h"
#include "fastblock/msg/sock/options.h"
#include "fastblock/msg/sock/types.h"
#include "fastblock/utils/simple_poller.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct spdk_sock;
struct spdk_sock_group;
struct spdk_thread;

namespace msg {
namespace sock {

class client;
class server;

enum class rpc_fail : uint8_t {
    ok = 0,
    resource_exhausted,
    timeout,
    request_timeout,
    too_large,
    connection_dead,
    unserialize,
    other,
    canceled
};

struct assembled_rpc {
    bool response{false};
    uint64_t correlation{0};
    wire_request_meta req_meta{};
    wire_reply_meta reply_meta{};
    std::string proto;
    std::string oob;
    size_t reserved{0};
};

class connection : public std::enable_shared_from_this<connection>,
                   public ::msg::channel {
public:
    connection(::spdk_sock* sock, ::spdk_sock_group* group, std::shared_ptr<options> opts,
               bool is_server, probe* pr, ::spdk_thread* thread);
    ~connection() noexcept;

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    bool attach();
    void detach_and_close();

    bool is_terminated() noexcept override { return _torn; }
    int async_shutdown() override;

    ::spdk_sock* sock() const noexcept { return _sock; }
    void set_server(server* s) noexcept { _server = s; }
    void set_client(client* c) noexcept { _client = c; }

    void on_readable();
    void pump_send();
    bool scan_timeouts(std::chrono::system_clock::time_point now);

    void CallMethod(const google::protobuf::MethodDescriptor* method,
                    google::protobuf::RpcController* ctrlr,
                    const google::protobuf::Message* request,
                    google::protobuf::Message* response,
                    google::protobuf::Closure* done) override;

    void send_reply(uint64_t correlation, rdma::status st, google::protobuf::Message* body);
    void send_status(uint64_t correlation, rdma::status st);
    void inject_unknown_frame();
    // Drop one server-side inbound RPC from inflight + process quota.
    // Must be called exactly once per reserved inbound (dispatch fail or method done).
    void finish_server_rpc(size_t reserved);

    uint32_t inflight_rpcs() const noexcept { return _inflight_rpcs; }
    size_t inflight_bytes() const noexcept { return _inflight_bytes; }

    void remember_done(uint64_t key) { _done_keys.remember(key); }
    bool is_done_key(uint64_t key) const { return _done_keys.contains(key); }

    using request_cb = std::function<void(connection*, assembled_rpc)>;
    void set_on_request(request_cb cb) { _on_request = std::move(cb); }

private:
    struct pending_rpc {
        uint64_t correlation{0};
        google::protobuf::RpcController* ctrlr{nullptr};
        google::protobuf::Message* response{nullptr};
        google::protobuf::Closure* closure{nullptr};
        std::chrono::system_clock::time_point start_at{};
        std::chrono::microseconds timeout{rdma::default_rpc_timeout_us};
        size_t reserved{0};
        bool completed{false};
        bool priority{false};
        std::vector<encoded_frame> frames;
        size_t next_frame{0};
    };

    struct live_assembly {
        assembly a{};
        std::chrono::system_clock::time_point deadline{};
        size_t reserved{0};
    };

    void complete_rpc(pending_rpc* req, rpc_fail why, const char* detail = nullptr);
    void cancel_rpc(uint64_t correlation);
    void release_inflight(size_t reserved);
    void teardown(const char* why);
    void apply_sock_opts();
    hdr_status ingest_bytes();
    void handle_complete_frame(const sock_frame_hdr& hdr, std::string_view meta,
                               std::string_view proto, std::string_view payload);
    void enqueue_frames(std::vector<encoded_frame> frames, bool priority);
    void submit_one_frame(encoded_frame fr);
    static void on_send_done(void* arg, int err);

    ::spdk_sock* _sock{nullptr};
    ::spdk_sock_group* _group{nullptr};
    std::shared_ptr<options> _opts;
    bool _is_server{false};
    bool _in_group{false};
    bool _torn{false};
    probe* _probe{nullptr};
    ::spdk_thread* _thread{nullptr};
    server* _server{nullptr};
    client* _client{nullptr};

    std::string _recv;
    size_t _recv_off{0};
    bool _have_hdr{false};
    sock_frame_hdr _cur_hdr{};
    uint32_t _cur_need{0};
    bool _partial{false};
    std::chrono::system_clock::time_point _recv_deadline{};

    std::unordered_map<uint64_t, live_assembly> _assemblies;
    std::unordered_map<uint64_t, std::unique_ptr<pending_rpc>> _inflight;
    rdma::completed_key_ring _done_keys{};
    uint32_t _key_gen{0};
    uint32_t _key_idx{1};

    std::deque<encoded_frame> _priority_q;
    std::deque<encoded_frame> _normal_q;
    uint32_t _async_inflight{0};
    uint32_t _inflight_rpcs{0};
    size_t _inflight_bytes{0};

    request_cb _on_request;
};

} // namespace sock
} // namespace msg
