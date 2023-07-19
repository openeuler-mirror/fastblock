#pragma once

#include "memory_pool.h"
#include "rpc_controller.h"
#include "types.h"

#include "rpc_meta.pb.h"

#include <spdk/log.h>
#include <spdk/rdma_server.h>
#include <spdk/rdma_client.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/service.h>

#include <fmt/core.h>

#include <cstdio>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>

namespace msg {
namespace rdma {

class transport_server;

std::unordered_map<uint32_t, std::shared_ptr<transport_server>> g_transport_shard{};

void rpc_dispatcher(uint32_t opc, ::iovec *iovs, int iov_cnt, int length, ::spdk_srv_rpc_dispatcher_iovs_cb cb, void *cb_arg) {
    auto cur_core = ::spdk_env_get_current_core();
    assert(g_transport_shard.contains(cur_core));
    g_transport_shard[cur_core]->alloc_rpc_task(opc, iovs, iov_cnt, length, cb, cb_arg);
}

class transport_server : public std::enable_shared_from_this<transport_server> {

private:

    class service {
    public:
        using value = google::protobuf::Service;
        using pointer = value*;
        using value_descriptor_type = google::protobuf::ServiceDescriptor;
        using value_descriptor_pointer = value_descriptor_type*;
        using method_descriptor_type = google::protobuf::MethodDescriptor;
        using method_descriptor_pointer = method_descriptor_type*;

        service() = delete;

        service(pointer s, value_descriptor_pointer d) : data{s}, descriptor{d} {
            if (not data) {
                SPDK_ERRLOG("ERROR: null service pointer\n");
                throw std::invalid_argument{"null service pointer"};
            }

            if (not descriptor) {
                SPDK_ERRLOG("ERROR: null service descriptor pointer\n");
                throw std::invalid_argument{"null service descriptor pointer"};
            }

            for (int i{0}; i < descriptor->method_count(); ++i) {
                methods.emplace(descriptor->name(), descriptor->method(i));
            }
        }

        service(const service&) = delete;

        service(service&&) = default;

        service& operator=(const service&) = delete;

        service& operator=(service&&) = delete;

        ~service() noexcept = default;

        pointer data{nullptr};
        const value_descriptor_pointer descriptor{nullptr};
        std::unordered_map<std::string, const method_descriptor_pointer> methods{};
    };

public:

    class rpc_task {
    public:
        using id_type = uint64_t;
        struct value_type {
            uint32_t opc{0};
            ::iovec *iovs{nullptr};
            int iov_cnt{0};
            int length{0};
            ::spdk_srv_rpc_dispatcher_iovs_cb cb{};
            void *cb_arg{nullptr};
        };
        using pointer = value_type*;

    public:

        rpc_task(
          id_type id,
          std::shared_ptr<memory_pool> mp,
          uint32_t opc,
          ::iovec *iovs,
          int iov_cnt,
          int length,
          ::spdk_srv_rpc_dispatcher_iovs_cb cb,
          void *cb_arg) : id{id}, _mem_pool{mp} {
            data = _mem_pool->get<value_type>();
            if (data) {
                std::memset(data, 0, sizeof(value_type));
                _in_pool = true;
            } else {
                data = reinterpret_cast<pointer>(::spdk_zmalloc(
                    sizeof(value_type), 64, nullptr, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE));
                _in_pool = false;
            }

            data->opc = opc;
            data->iovs = iovs;
            data->iov_cnt = iov_cnt;
            data->length = length;
            data->cb = cb;
            data->cb_arg = cb_arg;
        }

        rpc_task(const rpc_task&) = delete;

        rpc_task(rpc_task&& v) : data{std::exchange(v.data, nullptr)} {}

        rpc_task& operator=(const rpc_task&) = delete;

        rpc_task&& operator=(rpc_task&&) = delete;

        ~rpc_task() noexcept {
            if (not data) { return; }

            if (not _in_pool) {
                _mem_pool->put(data);
            }

            ::spdk_free(data);
        }

    public:
        id_type id{};
        pointer data{nullptr};

    private:
        bool _in_pool{false};
        std::shared_ptr<memory_pool> _mem_pool{nullptr};
    };

    struct reply_record {
        std::unique_ptr<rpc_task> task{nullptr};
        std::unique_ptr<google::protobuf::Message> request_body{nullptr};
        std::unique_ptr<google::protobuf::Message> response_body{nullptr};
        std::unique_ptr<rpc_controller> rpc_ctrlr{nullptr};
        std::unique_ptr<google::protobuf::Closure> done{nullptr};
        std::unique_ptr<char[]> serialized_buf{nullptr};
        std::unique_ptr<::iovec[]> serialized_iovec{nullptr};
        transport_server* this_server{nullptr};
    };

public:

    transport_server() = delete;

    transport_server(const transport_server&) = delete;

    transport_server(transport_server&&) = default;

    transport_server& operator=(const transport_server&) = delete;

    transport_server& operator=(transport_server&&) = delete;

    ~transport_server() noexcept = default;

public:

    auto destory_transport() noexcept {
        ::spdk_srv_transport_destroy(_transport, nullptr, nullptr);
    }

    void add_service(service::pointer p) {
        auto service_val = std::make_shared<service>(p, p->GetDescriptor());
        _services.emplace(service_val->descriptor->name(), service_val);
    }

    auto& get_task_list() noexcept { return _inflight_tasks; }

    void alloc_rpc_task(uint32_t opc, ::iovec *iovs, int iov_cnt, int length, ::spdk_srv_rpc_dispatcher_iovs_cb cb, void *cb_arg) {
        auto task = std::make_unique<rpc_task>(
          _task_id_gen++, _mp, opc, iovs, iov_cnt, length, cb, cb_arg);
        _inflight_tasks.push_back(std::move(task));
    }

    void start() {
        auto cur_core = ::spdk_env_get_current_core();
        SPDK_NOTICELOG("Starting rdma server on core %d...\n", cur_core);

        if (not _mp) {
            // FIXME: hard code prefix
            auto mem_pool_name = fmt::format("mem_pool_{}", cur_core);
            _mp = std::make_shared<memory_pool>(
                mem_pool_name.c_str(),
                _mem_pool_count,
                sizeof(rpc_task::value_type),
                SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
                SPDK_ENV_SOCKET_ID_ANY);

            if (not _mp->get()) {
                SPDK_ERRLOG("ERROR: create memory pool failed\n");
                throw std::runtime_error{"create memory pool failed"};
            }
        }

        g_transport_shard.emplace(cur_core, shared_from_this());
    }

    void prepare() {
        // FIXME: hard code
        ::spdk_srv_target_opts t_opts{"rdma_transport_server"};
        _tgt = ::spdk_srv_tgt_create(&t_opts);
        if (not _tgt) {
            SPDK_ERRLOG("ERROR: create server target failed\n");
            throw std::runtime_error{"create server target failed"};
        }
        SPDK_NOTICELOG("Created server target\n");

        ::spdk_srv_transport_opts_init("RDMA", &_opts, sizeof(_opts));
        _transport = spdk_srv_transport_create("RDMA", &_opts);
        if (not _transport) {
            SPDK_ERRLOG("ERROR: create server transport failed\n");
            throw std::runtime_error{"create server transport failed"};
        }
        SPDK_NOTICELOG("Created server transport\n");

        auto on_tgt_add_transport = [] (void *cb_arg, int status) mutable {
            if (status == 0) { return; }
            auto server = reinterpret_cast<transport_server*>(cb_arg);
            SPDK_ERRLOG("ERROR: failed to add transport to tgt.(%d)\n", status);
            server->destory_transport();
        };
        ::spdk_srv_tgt_add_transport(_tgt, _transport, on_tgt_add_transport, this);
        spdk_srv_rpc_register_dispatcher((void *)rpc_dispatcher, SPDK_CLIENT_SUBMIT_IOVES);
    }

    void start_listen(const std::string host, const uint16_t port) {
        ::spdk_srv_transport_id tid{"", SPDK_SRV_TRANSPORT_RDMA, SPDK_SRV_ADRFAM_IPV4, "", "", 0};
        auto name = fmt::format("listener_{}", ::spdk_env_get_current_core());
        ::strncpy(tid.traddr, host.c_str(), INET_ADDRSTRLEN);
        ::strncpy(tid.trstring, name.c_str(), SPDK_SRV_TRSTRING_MAX_LEN);
        auto port_str = std::to_string(port);
        ::strncpy(tid.trsvcid, port_str.c_str(), port_str.size());

        auto ret = ::spdk_srv_transport_listen(_transport, &tid, nullptr);
        if (ret != 0) {
            SPDK_ERRLOG("ERROR: server bound on %s:%d failed\n", tid.traddr, port);
            throw std::runtime_error{fmt::format("server bound on {}:{} failed", host, port)};
        }
    }

    void send_reply(reply_record* reply, status s) {
        auto reply_status = std::make_unique<reply_meta>();
        reply_status->set_status(static_cast<google::protobuf::int32>(s));
        int serialize_size{reply_status->ByteSize()};

        switch (s) {
        case status::success: {
            serialize_size += reply->response_body->ByteSize();
            break;
        }
        default:
            break;
        }

        SPDK_DEBUGLOG(
          rdma,
          "serialize size %d, request id %d\n",
          serilaize_size, reply->task->id);

        reply->serialized_buf = std::make_unique<char[]>(serialize_size);
        auto* serialized_buf = reply->serialized_buf.get();
        auto reply_status_ser_size = reply_status->ByteSize();
        reply_status->SerializeToArray(serialized_buf, reply_status_ser_size);
        if (reply_status_ser_size == serialize_size) {
            return;
        }

        std::ptrdiff_t offset{reply_status_ser_size};
        reply->response_body->SerializeToArray(
          serialized_buf + offset,
          reply->response_body->ByteSize());

        auto reply_iovcnt = static_cast<int>(std::ceil(
          static_cast<double>(serialize_size) / SPDK_SRV_MEMORY_POOL_ELEMENT_SIZE));
        reply->serialized_iovec = std::make_unique<::iovec[]>(reply_iovcnt);
        SPDK_DEBUGLOG(rdma, "reply iovcnt is %d\n", reply_iovcnt);
        auto iovs_ptr = reply->serialized_iovec.get();
        for (int i{0}; i < reply_iovcnt; ++i) {
            iovs_ptr[i].iov_base = serialized_buf + i * SPDK_SRV_MEMORY_POOL_ELEMENT_SIZE;
            iovs_ptr[i].iov_len = SPDK_SRV_MEMORY_POOL_ELEMENT_SIZE;
        }

        if (reply_iovcnt * SPDK_SRV_MEMORY_POOL_ELEMENT_SIZE != serialize_size) {
            iovs_ptr[reply_iovcnt - 1].iov_len =
              serialize_size - (reply_iovcnt - 1) * SPDK_SRV_MEMORY_POOL_ELEMENT_SIZE;
        }

        reply->task->data->cb(
          reply->task->data->cb_arg,
          static_cast<uint32_t>(s),
          iovs_ptr, reply_iovcnt,
          serialize_size, on_reply, reply);
    }

    void on_response(reply_record* reply_rd) {
        if (not reply_rd->response_body) {
            SPDK_ERRLOG(
              "ERROR: empty rpc response body of request id %d\n",
              reply_rd->task->id);
            send_reply(reply_rd, status::server_error);

            return;
        }

        SPDK_NOTICELOG(
          "Sending rpc response body of request id %d\n",
          reply_rd->task->id);
        send_reply(reply_rd, status::success);
    }

    void dispatch_method(std::unique_ptr<rpc_meta> meta, std::unique_ptr<rpc_task> task) {
        auto service_name = meta->service_name();
        auto service_it = _services.find(service_name);
        if (service_it == _services.end()) {
            SPDK_ERRLOG("ERROR: can not find service %s\n", service_name.c_str());
            // FIXME: reply error status
            throw std::invalid_argument{fmt::format("cant find service {}", service_name)};
        }

        auto method_it = service_it->second->methods.find(meta->method_name());
        if (method_it == service_it->second->methods.end()) {
            SPDK_ERRLOG("ERROR: can not find method %s\n", meta->method_name().c_str());
            // FIXME: reply error status
            throw std::invalid_argument{fmt::format("can not find method {}", meta->method_name())};
        }

        std::unique_ptr<google::protobuf::Message> request_body{
          service_it->second->data->GetRequestPrototype(method_it->second).New()};
        if (task->data->length != request_body->ByteSize()) {
            SPDK_ERRLOG(
              "ERROR: received request body length error, length from transport layer is %d, length from protocol message is %d\n",
              task->data->length, request_body->ByteSize());
            throw std::runtime_error{fmt::format(
              "received request body length error, length from transport layer is {}, length from protocol message is {}", task->data->length, request_body->ByteSize())};
        }
        auto unserialized_req_body = std::make_unique<char[]>(
          SPDK_SRV_MEMORY_POOL_ELEMENT_SIZE * task->data->iov_cnt);
        for (int i{0}; i < task->data->iov_cnt; ++i) {
            std::memcpy(
              &(unserialized_req_body[i]),
              &(task->data->iovs),
              SPDK_SRV_MEMORY_POOL_ELEMENT_SIZE);
        }
        request_body->ParseFromArray(unserialized_req_body.get(), request_body->ByteSize());
        std::unique_ptr<google::protobuf::Message> response_body{
          service_it->second->data->GetResponsePrototype(method_it->second).New()};
        auto ctrlr = std::make_unique<rpc_controller>();
        auto* raw_ctrlr = ctrlr.get();
        auto* raw_request = request_body.get();
        auto* raw_response = response_body.get();
        auto reply_key = task->id;
        auto reply_rd = std::make_unique<reply_record>(
          std::move(task),
          std::move(request_body),
          std::move(response_body),
          std::move(ctrlr),
          nullptr, nullptr, nullptr, this);
        std::unique_ptr<google::protobuf::Closure> done{google::protobuf::NewCallback(
          this, &transport_server::on_response, reply_rd.get())};
        auto* raw_done = done.get();
        reply_rd->done = std::move(done);
        _reply_records.emplace(reply_key, std::move(reply_rd));
        service_it->second->data->CallMethod(
          method_it->second,
          raw_ctrlr,
          raw_request,
          raw_response,
          raw_done);
    }

    void erase_reply_record(rpc_task::id_type id) {
        _reply_records.erase(id);
    }

public:

    static void on_reply(void* arg, int status) {
        auto* rd = reinterpret_cast<reply_record*>(arg);
        rd->this_server->erase_reply_record(rd->task->id);
    }

    static int poller_dispatch(void* arg) {
        auto* server = reinterpret_cast<transport_server*>(arg);
        auto& inflight_tasks = server->get_task_list();
        if (inflight_tasks.empty()) {
            return 0;
        }

        auto task_iter = inflight_tasks.begin();
        auto* task = task_iter->get();

        auto meta = std::make_unique<rpc_meta>();
        meta->ParseFromString(
          std::string{reinterpret_cast<char*>(task->data->iovs),
          meta->ByteSize()});

        SPDK_DEBUGLOG(
          "parsed rpc meta, service name is %s, method name is %s\n",
          meta->service_name().c_str(),
          meta->method_name().c_str());

        server->dispatch_method(std::move(meta), std::move(*task_iter));

        inflight_tasks.erase(task_iter);
    }

private:

    // TODO: use configuration
    constexpr static size_t _mem_pool_count{4096};

private:
    rpc_task::id_type _task_id_gen{};
    std::shared_ptr<memory_pool> _mp{nullptr};
    std::unordered_map<std::string, std::shared_ptr<service>> _services{};
    std::list<std::unique_ptr<rpc_task>> _inflight_tasks{};
    std::unordered_map<rpc_task::id_type, std::unique_ptr<reply_record>> _reply_records{};
    ::spdk_srv_tgt *_tgt{nullptr};
    ::spdk_srv_transport_opts _opts{};
    ::spdk_srv_transport *_transport{nullptr};
};

} // namespace rdma
} // namespace msg
