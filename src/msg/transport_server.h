#pragma once

#include "memory_pool.h"
#include "rpc_controller.h"
#include "types.h"

#include <spdk/log.h>
#include <spdk/rdma_server.h>
#include <spdk/rdma_client.h>
#include <spdk/thread.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <fmt/core.h>

#include <cstdio>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string_view>

namespace msg {
namespace rdma {

class transport_server;

namespace {
std::unordered_map<uint32_t, std::weak_ptr<transport_server>> g_transport_shard{};
}

void rpc_dispatcher(
  uint32_t opc,
  ::iovec *iovs,
  int iov_cnt,
  int length,
  ::spdk_srv_rpc_dispatcher_cb cb,
  void *cb_arg);

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

        service(pointer s, const value_descriptor_pointer d) : data{s}, descriptor{d} {
            if (not data) {
                SPDK_ERRLOG("ERROR: null service pointer\n");
                throw std::invalid_argument{"null service pointer"};
            }

            if (not descriptor) {
                SPDK_ERRLOG("ERROR: null service descriptor pointer\n");
                throw std::invalid_argument{"null service descriptor pointer"};
            }

            method_descriptor_pointer m{nullptr};
            for (int i{0}; i < descriptor->method_count(); ++i) {
                m = const_cast<method_descriptor_pointer>(descriptor->method(i));
                methods.emplace(m->name(), m);
            }
        }

        service(const service&) = delete;

        service(service&&) = default;

        service& operator=(const service&) = delete;

        service& operator=(service&&) = delete;

        ~service() noexcept = default;

        pointer data{nullptr};
        const value_descriptor_pointer descriptor{nullptr};
        std::unordered_map<std::string_view, method_descriptor_pointer> methods{};
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
            ::spdk_srv_rpc_dispatcher_cb cb{};
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
          ::spdk_srv_rpc_dispatcher_cb cb,
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

            if (_in_pool) {
                _mem_pool->put(data);
                return;
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
        google::protobuf::Closure* done{nullptr};
        std::unique_ptr<char[]> serialized_buf{nullptr};
        transport_server* this_server{nullptr};
    };

public:

    transport_server() = default;

    transport_server(const transport_server&) = delete;

    transport_server(transport_server&&) = default;

    transport_server& operator=(const transport_server&) = delete;

    transport_server& operator=(transport_server&&) = delete;

    ~transport_server() noexcept {
        stop();
    }

public:

    auto destory_transport() noexcept {
        ::spdk_srv_transport_destroy(_transport, nullptr, nullptr);
    }

    void add_service(service::pointer p) {
        auto descriptor = const_cast<google::protobuf::ServiceDescriptor*>(p->GetDescriptor());
        auto service_val = std::make_shared<service>(p, descriptor);
        _services.emplace(service_val->descriptor->name(), service_val);
    }

    void alloc_rpc_task(
      uint32_t opc,
      ::iovec *iovs,
      int iov_cnt,
      int length,
      ::spdk_srv_rpc_dispatcher_cb cb,
      void *cb_arg) {
        auto task = std::make_unique<rpc_task>(
          _task_id_gen++, _mp, opc, iovs, iov_cnt, length, cb, cb_arg);
        auto current_core = ::spdk_env_get_current_core();
        auto task_list_it = _sharded_task_list.find(current_core);
        if (task_list_it == _sharded_task_list.end()) {
            SPDK_ERRLOG("ERROR: Can not find the task list on core %d\n", current_core);
            // TODO: handle error grace
            throw std::runtime_error{
              fmt::format("can not find the task list on core {}", current_core)};
        }
        task_list_it->second.push_back(std::move(task));
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

        g_transport_shard.emplace(cur_core, weak_from_this());
        auto* poller = SPDK_POLLER_REGISTER(poller_dispatch, this, 0);
        std::lock_guard<std::mutex> gurad{_poller_mutex};
        _poller_map.emplace(cur_core, poller);
    }

    void stop_listen() noexcept {
        if (not _tgt or not _trid) {
            return;
        }
        ::spdk_srv_tgt_stop_listen(_tgt, _trid.get());
    }

    void stop() {
        stop_listen();
        for (auto& poller_pair : _poller_map) {
            ::spdk_poller_unregister(&(poller_pair.second));
            SPDK_NOTICELOG("Stop the rpc server poller on core %d\n", poller_pair.first);
        }
        _poller_map.clear();
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

        uint32_t core{0};
        SPDK_ENV_FOREACH_CORE(core) {
            _sharded_task_list.emplace(core, std::list<std::unique_ptr<rpc_task>>{});
        }
        ::spdk_srv_rpc_register_dispatcher((void *)rpc_dispatcher, SPDK_CLIENT_SUBMIT_CONTING);
    }

    void start_listen(const std::string host, const uint16_t port) {
        start_listen(host.c_str(), port);
    }

    void start_listen(const char* host, const uint16_t port) {
        _trid = std::make_unique<::spdk_srv_transport_id>();
        _trid->trtype = SPDK_SRV_TRANSPORT_RDMA;
        _trid->adrfam = SPDK_SRV_ADRFAM_IPV4;
        _trid->priority = 0;
        auto name = fmt::format("listener_{}", ::spdk_env_get_current_core());
        ::strncpy(_trid->traddr, host, INET_ADDRSTRLEN);
        ::strncpy(_trid->trstring, name.c_str(), SPDK_SRV_TRSTRING_MAX_LEN);
        auto port_str = std::to_string(port);
        ::strncpy(_trid->trsvcid, port_str.c_str(), port_str.size());

        auto ret = ::spdk_srv_transport_listen(_transport, _trid.get(), nullptr);
        if (ret != 0) {
            SPDK_ERRLOG("ERROR: server bound on %s:%d failed\n", _trid->traddr, port);
            throw std::runtime_error{fmt::format("server bound on {}:{} failed", host, port)};
        }
    }

    void send_reply(reply_record* reply, status s) {
        auto reply_status = std::make_unique<reply_meta>(
          static_cast<std::underlying_type_t<status>>(s));
        size_t serialize_size{reply_meta_size};

        switch (s) {
        case status::success: {
            serialize_size += reply->response_body->ByteSizeLong();
            break;
        }
        default:
            break;
        }

        SPDK_DEBUGLOG(
          msg,
          "serialize size %ld, request id %ld\n",
          serialize_size, reply->task->id);

        reply->serialized_buf = std::make_unique<char[]>(serialize_size);
        auto* serialized_buf = reply->serialized_buf.get();
        std::memcpy(serialized_buf, reply_status.get(), reply_meta_size);
        if (request_meta_size == serialize_size) {
            return;
        }

        auto offset = reply_meta_size;
        reply->response_body->SerializeToArray(
          serialized_buf + offset,
          reply->response_body->ByteSizeLong());

        reply->task->data->cb(
          reply->task->data->cb_arg,
          static_cast<uint32_t>(s),
          serialized_buf,
          serialize_size,
          on_reply,
          reply);
    }

    void on_response(reply_record* reply_rd) {
        if (reply_rd->rpc_ctrlr->Failed()) {
            SPDK_ERRLOG(
              "ERROR: exec rpc failed: %s\n",
              reply_rd->rpc_ctrlr->ErrorText().c_str());
            send_reply(reply_rd, status::server_error);

            return;
        }

        if (not reply_rd->response_body) {
            SPDK_ERRLOG(
              "ERROR: empty rpc response body of request id %ld\n",
              reply_rd->task->id);
            send_reply(reply_rd, status::server_error);

            return;
        }

        SPDK_INFOLOG(msg,
                     "Sending rpc response body of request id %ld\n",
                     reply_rd->task->id);
        send_reply(reply_rd, status::success);
    }

    void dispatch_method(request_meta* meta, std::unique_ptr<rpc_task> task) {
        std::string_view service_name{meta->service_name, meta->service_name_size};
        auto reply_key = task->id;
        auto task_ptr = task.get();
        auto reply_rd = std::make_unique<reply_record>(
          std::move(task), nullptr, nullptr, nullptr, nullptr, nullptr, this);

        auto service_it = _services.find(service_name);
        if (service_it == _services.end()) {
            SPDK_ERRLOG("ERROR: can not find service %s\n", meta->service_name);
            auto reply_ptr = reply_rd.get();
            _reply_records.emplace(reply_key, std::move(reply_rd));
            send_reply(reply_ptr, status::service_not_found);
            return;
        }

        std::string_view method_name{meta->method_name, meta->method_name_size};
        auto method_it = service_it->second->methods.find(method_name);
        if (method_it == service_it->second->methods.end()) {
            SPDK_ERRLOG("ERROR: can not find method %s\n", meta->method_name);
            auto reply_ptr = reply_rd.get();
            _reply_records.emplace(reply_key, std::move(reply_rd));
            send_reply(reply_ptr, status::method_not_found);
            return;
        }

        std::unique_ptr<google::protobuf::Message> request_body{
          service_it->second->data->GetRequestPrototype(method_it->second).New()};
        if (task_ptr->data->iov_cnt == 1) {
            request_body->ParseFromArray(
              reinterpret_cast<char*>(task_ptr->data->iovs->iov_base) + request_meta_size,
              task_ptr->data->length - request_meta_size);
        } else {
            auto unserialized_req_body = std::make_unique<char[]>(task_ptr->data->length);
            size_t offset{0};
            for (int i{0}; i < task_ptr->data->iov_cnt; ++i) {
                std::memcpy(
                  unserialized_req_body.get() + offset,
                  task_ptr->data->iovs[i].iov_base,
                  task_ptr->data->iovs[i].iov_len);
                offset += task_ptr->data->iovs[i].iov_len;
            }
            request_body->ParseFromArray(
              unserialized_req_body.get() + request_meta_size,
              task_ptr->data->length - request_meta_size);
        }

        std::unique_ptr<google::protobuf::Message> response_body{
          service_it->second->data->GetResponsePrototype(method_it->second).New()};
        auto ctrlr = std::make_unique<rpc_controller>();
        auto* raw_ctrlr = ctrlr.get();
        auto* raw_request = request_body.get();
        auto* raw_response = response_body.get();
        reply_rd->request_body = std::move(request_body);
        reply_rd->response_body = std::move(response_body);
        reply_rd->rpc_ctrlr = std::move(ctrlr);
        auto done = google::protobuf::NewCallback(
          this, &transport_server::on_response, reply_rd.get());
        reply_rd->done = done;
        _reply_records.emplace(reply_key, std::move(reply_rd));
        service_it->second->data->CallMethod(
          method_it->second,
          raw_ctrlr,
          raw_request,
          raw_response,
          done);
    }

    void erase_reply_record(rpc_task::id_type id) {
        SPDK_DEBUGLOG(msg, "Erase rpc request with id %ld\n", id);
        _reply_records.erase(id);
    }

    bool process_task_once() {
        auto current_core = ::spdk_env_get_current_core();

        auto task_list_it = _sharded_task_list.find(current_core);
        if (task_list_it->second.empty()) { return false; }
        auto task_it = task_list_it->second.begin();
        auto *task = task_it->get();

        auto* meta = reinterpret_cast<request_meta*>(task->data->iovs->iov_base);
        SPDK_DEBUGLOG(
          msg,
          "parsed rpc meta, service name is %s, method name is %s\n",
          meta->service_name,
          meta->method_name);
        dispatch_method(meta, std::move(*task_it));
        task_list_it->second.erase(task_it);

        return true;
    }

public:

    static void on_reply(void* arg, int status) {
        auto* rd = reinterpret_cast<reply_record*>(arg);
        rd->this_server->erase_reply_record(rd->task->id);
    }

    static int poller_dispatch(void* arg) {
        auto* server = reinterpret_cast<transport_server*>(arg);
        auto ret = server->process_task_once();
        return ret ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
    }

private:

    // TODO: use configuration
    constexpr static size_t _mem_pool_count{4096};

private:
    rpc_task::id_type _task_id_gen{};
    std::shared_ptr<memory_pool> _mp{nullptr};
    std::unordered_map<std::string_view, std::shared_ptr<service>> _services{};
    std::unordered_map<rpc_task::id_type, std::unique_ptr<reply_record>> _reply_records{};
    ::spdk_srv_tgt *_tgt{nullptr};
    ::spdk_srv_transport_opts _opts{};
    ::spdk_srv_transport *_transport{nullptr};
    std::unique_ptr<::spdk_srv_transport_id> _trid;
    std::mutex _poller_mutex{};
    std::unordered_map<uint32_t, ::spdk_poller*> _poller_map{};
    std::unordered_map<uint32_t, std::list<std::unique_ptr<rpc_task>>> _sharded_task_list{};
};

void rpc_dispatcher(
  uint32_t opc,
  ::iovec *iovs,
  int iov_cnt,
  int length,
  ::spdk_srv_rpc_dispatcher_cb cb,
  void *cb_arg) {
    auto cur_core = ::spdk_env_get_current_core();
    assert(g_transport_shard.contains(cur_core));
    auto& weak_server = g_transport_shard[cur_core];
    if (weak_server.expired()) {
        SPDK_ERRLOG(
          "ERROR: the instance of transport_server on core %d has been deleted\n",
          cur_core);
        g_transport_shard.erase(cur_core);
        return;
    }

    weak_server.lock()->alloc_rpc_task(opc, iovs, iov_cnt, length, cb, cb_arg);
}

} // namespace rdma
} // namespace msg
