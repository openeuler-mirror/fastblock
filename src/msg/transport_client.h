#pragma once

#include "msg/types.h"

#include <spdk/log.h>
#include <spdk/rdma_client.h>
#include <spdk/string.h>
#include <spdk/thread.h>

#include <fmt/core.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace msg {
namespace rdma {

class transport_client {
public:

    class connection : public google::protobuf::RpcChannel, public std::enable_shared_from_this<connection> {
    public:
        using id_type = uint64_t;

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

        connection(const id_type id, const std::string host, const uint16_t port, std::shared_ptr<std::list<std::shared_ptr<connection>>> busy_list, std::shared_ptr<std::list<std::shared_ptr<connection>>> busy_priority_list)
          : _id{id}
          , _host(host)
          , _port{port}
          , _busy_list{busy_list}
          , _busy_priority_list{busy_priority_list} {}

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

        static void on_connected(void* arg, int) {
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
                  "not enough chunks for request %d, which needs %ld bytes\n",
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
                if (_priority_inflight_requests.empty()) {
                    _busy_priority_list->push_back(shared_from_this());
                }
                _priority_inflight_requests.push_back(std::move(req));
            } else {
                _busy_list->push_back(shared_from_this());
                _inflight_requests.push_back(std::move(req));
            }
        }


    public:

        auto is_connected() noexcept { return _connected; }

        auto id() const noexcept { return _id; }

        void set_connected() noexcept { _connected = true; }

        void connect(std::shared_ptr<::client_poll_group> pg) {
            SPDK_NOTICELOG("Connecting to %s:%d...\n", _host.c_str(), _port);
            auto trans_id = fmt::format("trtype:RDMA adrfam:IPV4 traddr:{} trsvcid:{}", _host, _port);
            _trid = std::make_unique<::spdk_client_transport_id>();
            auto rc = ::spdk_client_transport_id_parse(_trid.get(), trans_id.c_str());
            if (rc != 0) {
                SPDK_ERRLOG(
                  "ERROR: spdk_client_transport_id_parse() failed, errno %d: %s\n",
                  errno, spdk_strerror(errno));
                throw std::runtime_error{fmt::format(
                  "ERROR: spdk_client_transport_id_parse() failed, errno {}: {}",
                  errno, spdk_strerror(errno))};
            }

            _pg = pg;
            _conn = ::spdk_client_ctrlr_alloc_io_qpair_async(
              _pg->ctrlr, nullptr, 0, _trid.get(), _pg->group, on_connected, this);
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
                ctrlr->SetFailed(fmt::format(
                  "service name too long, should less than or equal to {}",
                  max_rpc_meta_string_size));
                c->Run();
                return;
            }

            auto& method_name = m->name();
            if (method_name.size() > max_rpc_meta_string_size) {
                SPDK_ERRLOG(
                  "ERROR: RPC method name's length(%ld) is beyond the max size(%d)\n",
                  method_name.size(), max_rpc_meta_string_size);
                ctrlr->SetFailed(fmt::format(
                  "method name is too long, should less than or equal to {}",
                  max_rpc_meta_string_size));
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
              "rpc meta service name is %s, method name is %s, data size is %ld\n",
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
            if (_priority_inflight_requests.empty()) {
                return false;
            }

            SPDK_DEBUGLOG(
              msg,
              "process %ld rpc call heartbeat()\n",
              _priority_inflight_requests.size());

            int rc{0};
            auto erase_it_end = _priority_inflight_requests.begin();
            for (auto it = _priority_inflight_requests.begin(); it != _priority_inflight_requests.end(); ++it) {
                rc = process_request_once(*it);
                if (rc == -EAGAIN) {
                    break;
                } else if (rc != 0) {
                    SPDK_ERRLOG(
                      "ERROR: send priority rpc request(key: %ld) failed\n",
                      it->get()->request_key);
                    throw std::runtime_error{"send priority rpc request failed"};
                }

                erase_it_end = it;
            }

            if (rc == -EAGAIN) {
                _priority_inflight_requests.erase(
                  _priority_inflight_requests.begin(),
                  erase_it_end);
                return true;
            }

            _busy_priority_list->erase(busy_it);

            return true;
        }

        bool process_rpc_request(std::list<std::shared_ptr<connection>>::iterator busy_it) {
            if (_inflight_requests.empty()) {
                return false;
            }

            auto it = _inflight_requests.begin();
            auto rc = process_request_once(*it);
            if (rc == -EAGAIN) {
                return true;
            } else if (rc != 0) {
                SPDK_ERRLOG(
                  "ERROR: send rpc request(key: %ld) failed\n",
                  it->get()->request_key);
                throw std::runtime_error{"send rpc request failed"};
            }

            _inflight_requests.erase(it);
            _busy_list->erase(busy_it);
            return true;
        }

        void process_response(
          unresponsed_request* request,
          int raw_status,
          ::iovec* iovs,
          int iovcnt,
          int length) {
            auto e_status = static_cast<std::underlying_type_t<status>>(raw_status);
            SPDK_NOTICELOG(
              "Received response of request %ld, status is %s, iovec count is %d, length is %d\n",
              request->request->request_key,
              string_status(e_status),
              iovcnt, length);

            auto request_it = _unresponsed_requests.find(request->request->request_key);
            if (request_it == _unresponsed_requests.end()) {
                SPDK_ERRLOG(
                  "ERROR: Cant find the request record of request key %ld on connection %ld\n",
                  request->request->request_key, _id);
                throw std::runtime_error{fmt::format(
                  "cant find the request record of request key {} on connection {}",
                  request->request->request_key, _id)};
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
            default: {
                SPDK_ERRLOG(
                  "ERROR: RPC call failed of request %ld with reply status %s\n",
                  request->request->request_key,
                  string_status(reply_status_e));
                request->request->ctrlr->SetFailed(fmt::format(""));
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

        std::list<std::unique_ptr<rpc_request>> _inflight_requests{};
        std::list<std::unique_ptr<rpc_request>> _priority_inflight_requests{};
        rpc_request::key_type _unresponsed_request_key_gen{0};
        std::unordered_map<rpc_request::key_type, std::unique_ptr<unresponsed_request>> _unresponsed_requests{};
        std::shared_ptr<std::list<std::shared_ptr<connection>>> _busy_list{};
        std::shared_ptr<std::list<std::shared_ptr<connection>>> _busy_priority_list{};
    };

public:

    transport_client() {
        SPDK_NOTICELOG("construct transport_client, this is %p\n", this);
    }

    transport_client(const transport_client&) = delete;

    transport_client(transport_client&&) = default;

    transport_client& operator=(const transport_client&) = delete;

    transport_client& operator=(transport_client&&) = delete;

    ~transport_client() noexcept {
        SPDK_NOTICELOG("destruct transport_client\n");
        ::spdk_poller_unregister(&_poller);
    }


public:

    auto poll_group() noexcept { return _pg; }

public:

    static int poll_fn(void* arg) {
        auto* this_client = reinterpret_cast<transport_client*>(arg);
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
            throw std::runtime_error{fmt::format(
              "construct client transport ctrlr failed: {}", ::spdk_strerror(errno)),};
        }
        _pg->group = ::spdk_client_poll_group_create(_pg.get(), nullptr);
        SPDK_NOTICELOG(
          "Construct a new ctrlr = %p, a new poll group = %p\n",
          _pg->ctrlr, _pg->group);
        if (not _pg->group) {
            SPDK_ERRLOG(
              "ERROR: spdk_client_poll_group_create() failed, errno %d: %s\n",
              errno, ::spdk_strerror(errno));
            throw std::runtime_error{fmt::format(
              "spdk_client_poll_group_create() failed, errno {}: {}",
              errno, ::spdk_strerror(errno))};
        }

        _poller = SPDK_POLLER_REGISTER(poll_fn, this, 0);
    }

    std::shared_ptr<connection>
    emplace_connection(const connection::id_type id, std::string host, const uint16_t port) {
        auto conn = std::make_shared<connection>(id, host, port, _busy_connections, _busy_priority_connections);
        conn->connect(_pg);
        SPDK_NOTICELOG("Connected to %s:%d\n", host.c_str(), port);
        _connect_tasks.push_back(conn);

        return conn;
    }

    void erase_connection(const connection::id_type id) {
        // TODO:
    }

public:

    bool process_connect_task() {
        if (_connect_tasks.empty()) { return false; }

        auto task_iter = _connect_tasks.begin();
        auto* task = task_iter->get();
        if (not task->is_connected()) {
            return true;
        }
        _connections.emplace(task->id(), std::move(*task_iter));
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

private:

    ::spdk_poller* _poller{nullptr};
    std::list<std::shared_ptr<connection>> _connect_tasks{};
    std::unordered_map<connection::id_type, std::shared_ptr<connection>> _connections{};
    std::shared_ptr<std::list<std::shared_ptr<connection>>> _busy_connections{
      std::make_shared<std::list<std::shared_ptr<connection>>>()};
    std::shared_ptr<std::list<std::shared_ptr<connection>>> _busy_priority_connections{
      std::make_shared<std::list<std::shared_ptr<connection>>>()};
    std::shared_ptr<::client_poll_group> _pg{nullptr};
    ::spdk_client_ctrlr_opts _ops{};
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
