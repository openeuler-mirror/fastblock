/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#pragma once

#include "msg/types.h"

#include <spdk/log.h>
#include <spdk/rdma_client.h>
#include <spdk/string.h>
#include <spdk/thread.h>

#include <boost/format.hpp>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>

namespace msg {
namespace rdma {

class transport_client {
public:

    class connection : public google::protobuf::RpcChannel, public std::enable_shared_from_this<connection> {
    public:
        using id_type = uint64_t;

        struct option {
            int32_t io_queue_size;
            int32_t io_queue_request;
        };

        struct rpc_request {
            using key_type = uint64_t;

            key_type request_key{};
            const google::protobuf::MethodDescriptor* method{nullptr};
            google::protobuf::RpcController* ctrlr{nullptr};
            const google::protobuf::Message* request{nullptr};
            google::protobuf::Message* response{nullptr};
            google::protobuf::Closure* closure{nullptr};
            std::unique_ptr<char[]> meta{nullptr};
            size_t serialized_size{0};
            std::unique_ptr<char[]> serialized_buf{nullptr};
        };

        struct unresponsed_request {
            std::unique_ptr<rpc_request> request{nullptr};
            connection* this_connection{nullptr};
        };

    public:
        connection() = delete;

        connection(
          const id_type id,
          const std::string host,
          const uint16_t port,
          const option& opt,
          std::shared_ptr<std::list<std::shared_ptr<connection>>> busy_list,
          std::shared_ptr<std::list<std::shared_ptr<connection>>> busy_priority_list)
          : _id{id}
          , _host(host)
          , _port{port}
          , _busy_list{busy_list}
          , _busy_priority_list{busy_priority_list}
          , _opt{opt} {}

        connection(
          const id_type id,
          const std::string host,
          const uint16_t port,
          const int32_t io_queue_size,
          const int32_t io_queue_request,
          std::shared_ptr<std::list<std::shared_ptr<connection>>> busy_list,
          std::shared_ptr<std::list<std::shared_ptr<connection>>> busy_priority_list)
          : _id{id}
          , _host(host)
          , _port{port}
          , _busy_list{busy_list}
          , _busy_priority_list{busy_priority_list}
          , _opt{io_queue_size, io_queue_request} {}

        connection(const connection&) = delete;

        connection(connection&& c)
          : _id{std::exchange(c._id, id_type{})}
          , _connected{std::exchange(c._connected, false)}
          , _host{std::move(c._host)}
          , _port{c._port}
          , _trid{std::move(c._trid)}
          , _conn{std::exchange(c._conn, nullptr)}
          , _pg{std::move(c._pg)} {}

        connection& operator=(const connection&) = delete;

        connection& operator=(connection&&) = delete;

    public:

        static void on_connected(void* arg, int status) {
            SPDK_DEBUGLOG(msg, "connect status is %d\n", status);
            auto conn = reinterpret_cast<connection*>(arg);
            conn->set_connected();
        }

        static void on_response(void* arg, int status, ::iovec* iovs, int iovcnt, int length) {
            auto req = reinterpret_cast<unresponsed_request*>(arg);
            req->this_connection->process_response(req, status, iovs, iovcnt, length);
        }

    private:

        void serialize_request(std::unique_ptr<rpc_request>& req) {
            std::memcpy(req->serialized_buf.get(), req->meta.get(), request_meta_size);
            req->request->SerializeToArray(
              req->serialized_buf.get() + request_meta_size,
              req->request->ByteSizeLong());
        }

        int process_request_once(std::unique_ptr<rpc_request>& req) {
            if (::spdk_client_empty_free_request(_conn)) {
                return -EAGAIN;
            }

            if (not ::spdk_client_ctrlr_has_free_memory(_conn, req->serialized_size)) {
                SPDK_DEBUGLOG(
                  msg,
                  "not enough chunks for request %ld, which needs %ld bytes\n",
                  req->request_key, req->serialized_size);
                return -EAGAIN;
            }
            req->serialized_buf = std::make_unique<char[]>(req->serialized_size);
            SPDK_DEBUGLOG(
              msg,
              "request id is %ld, request serialize size is %ld, request body size is %ld\n",
              req->request_key,
              req->serialized_size - request_meta_size,
              req->request->ByteSizeLong()
            );
            serialize_request(req);

            auto k = req->request_key;
            auto* req_ptr = req.get();
            auto unresponsed_req = std::make_unique<unresponsed_request>(
              std::move(req), this);
            auto cb_arg = unresponsed_req.get();
            _unresponsed_requests.emplace(k, std::move(unresponsed_req));

            SPDK_DEBUGLOG(
              msg,
              "Send rpc request(id: %ld) on %p, with body size %ld\n",
              k, req_ptr->serialized_buf.get(), req_ptr->serialized_size);

            auto rc = ::spdk_client_submit_rpc_request(
              _conn, static_cast<uint32_t>(status::success),
              req_ptr->serialized_buf.get(),
              req_ptr->serialized_size, on_response, cb_arg, false);

            return rc;
        }

        bool is_priority_request(const google::protobuf::MethodDescriptor* m) {
            return m->name() == "heartbeat";
        }

        void enqueue_request(std::unique_ptr<rpc_request> req, bool priority_req) {
            if (priority_req) {
                if (_priority_onflight_requests.empty()) {
                    _busy_priority_list->push_back(shared_from_this());
                }
                _priority_onflight_requests.push_back(std::move(req));
            } else {
                _busy_list->push_back(shared_from_this());
                _onflight_requests.push_back(std::move(req));
            }
        }


    public:

        auto is_connected() noexcept { return _connected; }

        auto id() const noexcept { return _id; }

        void set_connected() noexcept { _connected = true; }

        auto& host() noexcept { return _host; }

        auto port() noexcept { return _port; }

        void connect(std::shared_ptr<::client_poll_group> pg) {
            SPDK_NOTICELOG("Connecting to %s:%d...\n", _host.c_str(), _port);
            auto trans_id = (boost::format("trtype:RDMA adrfam:IPV4 traddr:%1% trsvcid:%2%") %  _host % _port).str();
            _trid = std::make_unique<::spdk_client_transport_id>();
            auto rc = ::spdk_client_transport_id_parse(_trid.get(), trans_id.c_str());
            if (rc != 0) {
                SPDK_ERRLOG(
                  "ERROR: spdk_client_transport_id_parse() failed, errno %d: %s\n",
                  errno, spdk_strerror(errno));
                throw std::runtime_error{"ERROR: spdk_client_transport_id_parse() failed"};
            }

            _pg = pg;
            ::spdk_client_io_qpair_opts opts{};
            opts.io_queue_size = _opt.io_queue_size;
            opts.io_queue_requests = _opt.io_queue_request;
            _conn = ::spdk_client_ctrlr_alloc_io_qpair_async(
              _pg->ctrlr, &opts, 0, _trid.get(), _pg->group, on_connected, this);
            if (not _conn) {
                SPDK_ERRLOG("ERROR: allocate client io qpair failed\n");
                throw std::runtime_error{"allocate client io qpair failed"};
            }
            SPDK_NOTICELOG(
              "Conneting to %s:%d done, conn is %p, ctrlr is %p, group is %p\n",
              _host.c_str(), _port, _conn, _pg->ctrlr, _pg->group);
        }

        virtual void CallMethod(
          const google::protobuf::MethodDescriptor* m,
          google::protobuf::RpcController* ctrlr,
          const google::protobuf::Message* request,
          google::protobuf::Message* response,
          google::protobuf::Closure* c) override {
            auto& service_name = m->service()->name();
            if (service_name.size() > max_rpc_meta_string_size) {
                SPDK_ERRLOG(
                  "ERROR: RPC service name's length(%ld) is beyond the max size(%d)\n",
                  service_name.size(), max_rpc_meta_string_size);
                ctrlr->SetFailed((boost::format(
                  "service name too long, should less than or equal to %1%") % max_rpc_meta_string_size).str());
                c->Run();
                return;
            }

            auto& method_name = m->name();
            if (method_name.size() > max_rpc_meta_string_size) {
                SPDK_ERRLOG(
                  "ERROR: RPC method name's length(%ld) is beyond the max size(%d)\n",
                  method_name.size(), max_rpc_meta_string_size);
                ctrlr->SetFailed((boost::format(
                  "method name is too long, should less than or equal to %1%") % max_rpc_meta_string_size).str());
                c->Run();
                return;
            }

            // avoid align
            auto meta_holder = std::make_unique<char[]>(request_meta_size);
            auto* meta = reinterpret_cast<request_meta*>(meta_holder.get());
            meta->service_name_size =
              static_cast<request_meta::name_size_type>(service_name.size());
            service_name.copy(meta->service_name, meta->service_name_size);
            meta->method_name_size =
              static_cast<request_meta::name_size_type>(method_name.size());
            method_name.copy(meta->method_name, meta->method_name_size);
            meta->data_size = static_cast<request_meta::data_size_type>(request->ByteSizeLong());

            SPDK_DEBUGLOG(
              msg,
              "rpc meta service name is %s, method name is %s, data size is %d\n",
              meta->service_name,
              meta->method_name,
              meta->data_size);

            auto serialized_size = request->ByteSizeLong() + request_meta_size;
            auto req = std::make_unique<rpc_request>(
              _unresponsed_request_key_gen++,
              m, ctrlr, request, response, c,
              std::move(meta_holder), serialized_size, nullptr);

            enqueue_request(std::move(req), is_priority_request(m));
        }

        bool process_priority_rpc_request(std::list<std::shared_ptr<connection>>::iterator busy_it) {
            if (not _connected) {
                return false;
            }

            if (_priority_onflight_requests.empty()) {
                return false;
            }

            if (_on_flight_request_size >= _opt.io_queue_size) {
                return false;
            }

            SPDK_DEBUGLOG(
              msg,
              "process %ld rpc call heartbeat()\n",
              _priority_onflight_requests.size());

            int rc{0};
            auto erase_it_end = _priority_onflight_requests.begin();
            for (auto it = _priority_onflight_requests.begin(); it != _priority_onflight_requests.end(); ++it) {
                if (_on_flight_request_size >= _opt.io_queue_size) {
                    rc = -EAGAIN;
                    erase_it_end = it;
                    break;
                }

                rc = process_request_once(*it);
                if (rc == -EAGAIN) {
                    erase_it_end = it;
                    break;
                } else if (rc != 0) {
                    SPDK_ERRLOG(
                      "ERROR: send priority rpc request(key: %ld) failed\n",
                      it->get()->request_key);
                    throw std::runtime_error{"send priority rpc request failed"};
                }

                ++_on_flight_request_size;
                SPDK_DEBUGLOG(msg, "[++_on_flight_request_size] %d\n", _on_flight_request_size);
                erase_it_end = it;
            }

            if (rc == -EAGAIN) {
                _priority_onflight_requests.erase(
                  _priority_onflight_requests.begin(),
                  erase_it_end);
                return true;
            }

            _priority_onflight_requests.clear();
            _busy_priority_list->erase(busy_it);

            return true;
        }

        bool process_rpc_request(std::list<std::shared_ptr<connection>>::iterator busy_it) {
            if (not _connected) {
                return false;
            }

            if (_onflight_requests.empty()) {
                return false;
            }

            if (_on_flight_request_size >= _opt.io_queue_size) {
                return false;
            }

            auto it = _onflight_requests.begin();
            auto rc = process_request_once(*it);
            if (rc == -EAGAIN) {
                return true;
            } else if (rc != 0) {
                SPDK_ERRLOG(
                  "ERROR: send rpc request(key: %ld) failed\n",
                  it->get()->request_key);
                throw std::runtime_error{"send rpc request failed"};
            }

            ++_on_flight_request_size;
            SPDK_DEBUGLOG(msg, "[++_on_flight_request_size] %d\n", _on_flight_request_size);
            _onflight_requests.erase(it);
            _busy_list->erase(busy_it);
            return true;
        }

        void process_response(
          unresponsed_request* request,
          int raw_status,
          ::iovec* iovs,
          int iovcnt,
          int length) {
            --_on_flight_request_size;
            SPDK_DEBUGLOG(msg, "[--_on_flight_request_size] %d\n", _on_flight_request_size);
            auto e_status = static_cast<std::underlying_type_t<status>>(raw_status);
            SPDK_DEBUGLOG(msg,
              "Received response of request %ld, status is %s, iovec count is %d, length is %d\n",
              request->request->request_key, string_status(e_status), iovcnt, length);

            auto request_it = _unresponsed_requests.find(request->request->request_key);
            if (request_it == _unresponsed_requests.end()) {
                SPDK_ERRLOG(
                  "ERROR: Cant find the request record of request key %ld on connection %ld\n",
                  request->request->request_key, _id);
                throw std::runtime_error{(boost::format(
                  "cant find the request record of request key %1% on connection %2%") % request->request->request_key % _id).str()};
            }

            auto reply_status = reinterpret_cast<reply_meta*>(iovs[0].iov_base);
            status reply_status_e{reply_status->reply_status};

            SPDK_DEBUGLOG(
              msg,
              "reply status of request %ld is %s\n",
              request->request->request_key,
              string_status(reply_status_e));

            switch (reply_status_e) {
            case status::success: {
                auto* response = request->request->response;
                bool is_parsed{false};
                if (iovcnt == 1) {
                    is_parsed = response->ParseFromArray(
                      reinterpret_cast<char*>(iovs[0].iov_base) + reply_meta_size,
                      iovs[0].iov_len - reply_meta_size);
                } else {
                    auto tmp_buf = std::make_unique<char[]>(length);
                    std::ptrdiff_t offset{0};
                    for (int i{0}; i < iovcnt; ++i) {
                        std::memcpy(tmp_buf.get() + offset, iovs[i].iov_base, iovs[i].iov_len);
                        offset += iovs[i].iov_len;
                    }

                    is_parsed = response->ParseFromArray(
                      tmp_buf.get() + reply_meta_size,
                      length - reply_meta_size);
                }

                if (not is_parsed) {
                    SPDK_ERRLOG(
                      "ERROR: Parse response body failed of request %ld\n",
                      request->request->request_key);

                    request->request->ctrlr->SetFailed("unserialize failed");
                    request->request->closure->Run();
                    _unresponsed_requests.erase(request_it);
                    return;
                }

                request->request->closure->Run();
                _unresponsed_requests.erase(request_it);
                return;
            }
            case status::no_content: {
                request->request->closure->Run();
                _unresponsed_requests.erase(request_it);
                return;
            }
            default: {
                SPDK_ERRLOG(
                  "ERROR: RPC call failed of request %ld with reply status %s\n",
                  request->request->request_key,
                  string_status(reply_status_e));
                request->request->ctrlr->SetFailed(
                  (boost::format("rpc call failed with reply status %1%") % string_status(reply_status_e)).str());
                request->request->closure->Run();
                _unresponsed_requests.erase(request_it);

                return;
            }
            }
        }

    private:
        id_type _id{};
        bool _connected{false};
        std::string _host{};
        uint16_t _port;

        std::unique_ptr<::spdk_client_transport_id> _trid{nullptr};
        ::spdk_client_qpair *_conn{};
        std::shared_ptr<::client_poll_group> _pg{nullptr};

        int32_t _on_flight_request_size{0};
        std::list<std::unique_ptr<rpc_request>> _onflight_requests{};
        std::list<std::unique_ptr<rpc_request>> _priority_onflight_requests{};
        rpc_request::key_type _unresponsed_request_key_gen{0};
        std::unordered_map<rpc_request::key_type, std::unique_ptr<unresponsed_request>> _unresponsed_requests{};
        std::shared_ptr<std::list<std::shared_ptr<connection>>> _busy_list{};
        std::shared_ptr<std::list<std::shared_ptr<connection>>> _busy_priority_list{};
        option _opt{};
    };

    struct connect_task_stack {
        std::shared_ptr<connection> conn{nullptr};
        std::optional<std::function<void()>> cb{std::nullopt};
    };

public:

    transport_client(const int32_t io_queue_size = 128, const int32_t io_queue_request = 1024)
      : _conn_opt{io_queue_size, io_queue_request} {
        if (_conn_opt.io_queue_size > _conn_opt.io_queue_request) {
            SPDK_ERRLOG("io_queue_request at least as large as io_queue_size\n");
            throw std::invalid_argument{"io_queue_request at least as large as io_queue_size"};
        }
        SPDK_NOTICELOG("construct transport_client, this is %p\n", this);
    }

    transport_client(const transport_client&) = delete;

    transport_client(transport_client&&) = default;

    transport_client& operator=(const transport_client&) = delete;

    transport_client& operator=(transport_client&&) = delete;

    ~transport_client() noexcept {
        SPDK_DEBUGLOG(msg, "destruct transport_client\n");
        if (_poller) {
            ::spdk_poller_unregister(&_poller);
        }
    }


public:

    auto poll_group() noexcept { return _pg; }

public:

    static int poll_fn(void* arg) {
        auto* this_client = reinterpret_cast<transport_client*>(arg);
        if (this_client->is_terminate()) { return SPDK_POLLER_IDLE; }
        auto ret = this_client->process_connect_task();
        auto pg = this_client->poll_group();
        auto num_completions = ::spdk_client_poll_group_process_completions(
          pg->group, 0, ::client_disconnected_qpair_cb);
        ret |= this_client->process_rpc_requests();
        if (ret) { return SPDK_POLLER_BUSY; }
        return num_completions > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
    }

public:

    void start() {
        ::spdk_client_ctrlr_get_default_ctrlr_opts(&_ops, sizeof(_ops));
        _pg = std::make_shared<::client_poll_group>();
        _pg->ctrlr = ::spdk_client_transport_ctrlr_construct("rdma", &_ops, nullptr);
        if (not _pg->ctrlr) {
            SPDK_ERRLOG("ERROR: construct client transport ctrlr failed, errno %d: %s\n",
              errno, ::spdk_strerror(errno));
            throw std::runtime_error{"construct client transport ctrlr failed"};
        }
        _pg->group = ::spdk_client_poll_group_create(_pg.get(), nullptr);
        SPDK_NOTICELOG(
          "Construct a new ctrlr = %p, a new poll group = %p\n",
          _pg->ctrlr, _pg->group);
        if (not _pg->group) {
            SPDK_ERRLOG(
              "ERROR: spdk_client_poll_group_create() failed, errno %d: %s\n",
              errno, ::spdk_strerror(errno));
            throw std::runtime_error{"spdk_client_poll_group_create() failed"};
        }

        _poller = SPDK_POLLER_REGISTER(poll_fn, this, 0);
        _is_start = true;
    }

    void stop() noexcept {
        if (_is_terminate) {
            return;
        }
        _is_terminate = true;
        ::spdk_poller_unregister(&_poller);
    }

    bool is_start() noexcept { return _is_start; }

    bool is_terminate() noexcept { return _is_terminate; }

    std::shared_ptr<connection>
    emplace_connection(
      const connection::id_type id,
      std::string host,
      const uint16_t port,
      const int32_t io_queue_size,
      const int32_t io_queue_request,
      std::optional<std::function<void()>> cb = std::nullopt) {
        auto conn = std::make_shared<connection>(
          id, host, port,
          io_queue_size,
          io_queue_request,
          _busy_connections,
          _busy_priority_connections);
        conn->connect(_pg);
        SPDK_NOTICELOG("Connecting to %s:%d, io_queue_size %d, io_queue_request %d\n",
        host.c_str(), port, io_queue_size, io_queue_request);
        auto conn_stack = std::make_unique<connect_task_stack>(conn, cb);
        _connect_tasks.push_back(std::move(conn_stack));

        return conn;
    }

    std::shared_ptr<connection>
    emplace_connection(
      const connection::id_type id,
      std::string host,
      const uint16_t port,
      std::optional<std::function<void()>> cb = std::nullopt) {
        return emplace_connection(
          id, host, port,
          _conn_opt.io_queue_size,
          _conn_opt.io_queue_request,
          std::move(cb));
    }

    void erase_connection(const connection::id_type id) {
        _connections.erase(id);
    }

public:

    bool process_connect_task() {
        if (_connect_tasks.empty()) { return false; }

        auto task_iter = _connect_tasks.begin();
        auto* task = task_iter->get();
        if (not task->conn->is_connected()) {
            return true;
        }

        SPDK_DEBUGLOG(
          msg,
          "Callback for connection(%s:%u)\n",
          task->conn->host().c_str(), task->conn->port());

        if (task->cb) {
            (*task->cb)();
        }
        _connections.emplace(task->conn->id(), std::move(task->conn));
        _connect_tasks.erase(task_iter);

        return true;
    }

    bool process_rpc_requests() {
        if (_busy_connections->empty() and _busy_priority_connections->empty()) {
            return false;
        }

        bool ret{false};
        if (not _busy_priority_connections->empty()) {
            auto it = _busy_priority_connections->begin();
            ret = it->get()->process_priority_rpc_request(it);
        }

        if (not _busy_connections->empty()) {
            auto it = _busy_connections->begin();
            ret |= it->get()->process_rpc_request(it);
        }

        return ret;
    }

    auto& connect_option() noexcept {
        return _conn_opt;
    }

private:

    bool _is_start{false};
    bool _is_terminate{false};
    ::spdk_poller* _poller{nullptr};
    std::list<std::unique_ptr<connect_task_stack>> _connect_tasks{};
    std::unordered_map<connection::id_type, std::shared_ptr<connection>> _connections{};
    std::shared_ptr<std::list<std::shared_ptr<connection>>> _busy_connections{
      std::make_shared<std::list<std::shared_ptr<connection>>>()};
    std::shared_ptr<std::list<std::shared_ptr<connection>>> _busy_priority_connections{
      std::make_shared<std::list<std::shared_ptr<connection>>>()};
    std::shared_ptr<::client_poll_group> _pg{nullptr};
    ::spdk_client_ctrlr_opts _ops{};
    connection::option _conn_opt{128, 1024};
};

} // namespace msg
} // namespace rpc

namespace std {
template <>
struct hash<msg::rdma::transport_client::connection> {
    auto operator()(const msg::rdma::transport_client::connection& v) {
        return hash<msg::rdma::transport_client::connection::id_type>{}(v.id());
    }
};
} // namespace std
