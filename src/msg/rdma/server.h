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
#pragma once

#include "msg/rdma/connection_id.h"
#include "msg/rdma/cq.h"
#include "msg/rdma/socket.h"
#include "msg/rdma/transport_data.h"
#include "msg/rdma/work_request_id.h"
#include "msg/rpc_controller.h"
#include "utils/simple_poller.h"
#include "utils/utils.h"
#include "utils/log.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <boost/property_tree/ptree.hpp>

#include <chrono>
#include <memory>
#include <unordered_map>

#include <endian.h>

#define conf_or_server(json_conf, opts, opts_key) conf_or_s(json_conf, "msg_server_"#opts_key, opts, opts_key)
namespace msg {
namespace rdma {

class server {

private:

    struct cm_record {
        std::optional<connection_id> conn_id{};
        std::unique_ptr<socket> sock{nullptr};
    };

    struct rpc_task;

    struct connection_record {
        std::unique_ptr<socket> sock;
        work_request_id dispatch_id;
        std::unique_ptr<memory_pool<::ibv_recv_wr>> recv_pool{nullptr};
        std::unique_ptr<memory_pool<::ibv_recv_wr>::net_context*[]> recv_ctx{nullptr};
        std::unordered_map<work_request_id::value_type, memory_pool<::ibv_recv_wr>::net_context*> recv_ctx_map{};
        std::unordered_map<transport_data::correlation_index_type, std::shared_ptr<rpc_task>> rpc_tasks{};
        std::list<std::unique_ptr<::ibv_wc>> cqe_list{};
        bool should_signal{false};
        int32_t onflight_send_wr{0};
    };

    struct rpc_task {
        using id_type = uint64_t;

        transport_data::correlation_index_type id{0};
        std::shared_ptr<connection_record> conn{};
        std::unique_ptr<google::protobuf::Message> request_body{nullptr};
        std::unique_ptr<google::protobuf::Message> response_body{nullptr};
        std::unique_ptr<rpc_controller> rpc_ctrlr{nullptr};
        google::protobuf::Closure* done{nullptr};
        std::unique_ptr<transport_data> request_data{nullptr};
        std::unique_ptr<transport_data> response_data{nullptr};
        memory_pool<::ibv_recv_wr>::net_context* recv_ctx{nullptr};
        std::optional<status> reply_status{};
        std::chrono::system_clock::time_point start_at{std::chrono::system_clock::now()};
    };

public:

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
                SPDK_ERRLOG_EX("ERROR: null service pointer\n");
                throw std::invalid_argument{"null service pointer"};
            }

            if (not descriptor) {
                SPDK_ERRLOG_EX("ERROR: null service descriptor pointer\n");
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

    struct stop_context {
        std::optional<std::function<void()>> on_stop_cb{std::nullopt};
        server* this_server{nullptr};
    };

    struct add_service_ctx {
        service::pointer service_ptr{nullptr};
        server* this_server{nullptr};
    };

public:

    struct options {
        std::string bind_address{};
        uint16_t port{};
        int listen_backlog{1024};
        size_t poll_cq_batch_size{128};
        size_t metadata_memory_pool_capacity{16384};
        size_t metadata_memory_pool_element_size{1024};
        size_t data_memory_pool_capacity{16384};
        size_t data_memory_pool_element_size{8192};
        size_t per_post_recv_num{512};
        std::chrono::system_clock::duration rpc_timeout{std::chrono::seconds{5}};
        std::unique_ptr<endpoint> ep{nullptr};
    };

    static std::shared_ptr<options> make_options(boost::property_tree::ptree& conf) {
        auto opts = std::make_shared<options>();
        conf_or_server(conf, opts, listen_backlog);
        conf_or_server(conf, opts, metadata_memory_pool_capacity);
        conf_or_server(conf, opts, metadata_memory_pool_element_size);
        conf_or_server(conf, opts, data_memory_pool_capacity);
        conf_or_server(conf, opts, data_memory_pool_element_size);
        conf_or_server(conf, opts, per_post_recv_num);

        if (conf.count("msg_server_rpc_timeout_us") != 0) {
            auto timeout_us = conf.get_child("msg_server_rpc_timeout_us").get_value<int64_t>();
            opts->rpc_timeout = std::chrono::milliseconds{timeout_us};
        }
        opts->ep = std::make_unique<endpoint>(conf);

        return opts;
    }

public:

    server() = delete;

    server(std::string thread_name, ::spdk_cpuset cpumask, std::shared_ptr<options> opts)
      : _cpumask{cpumask}
      , _thread{::spdk_thread_create(thread_name.c_str(), &_cpumask)}
      , _opts{std::move(opts)}
      , _dev{std::make_shared<device>(process_ib_event)}
      , _pd{std::make_unique<protection_domain>(_dev, _opts->ep->device_name)}
      , _cq{std::make_shared<completion_queue>(_opts->ep->cq_num_entries, *_pd)}
      , _listener{nullptr}
      , _wcs{std::make_unique<::ibv_wc[]>(_opts->poll_cq_batch_size)}
      , _meta_pool{
        std::make_shared<memory_pool<::ibv_send_wr>>(
        _pd->value(), "srv_meta",
        _opts->metadata_memory_pool_capacity,
        _opts->metadata_memory_pool_element_size, 0)}
      , _data_pool{std::make_shared<memory_pool<::ibv_send_wr>>(
        _pd->value(), "srv_data",
        _opts->data_memory_pool_capacity,
        _opts->data_memory_pool_element_size, 0)} {
        auto ipv4_address = _dev->query_ipv4(
          _opts->ep->device_name,
          _opts->ep->device_port,
          _opts->ep->gid_index);

        if (not ipv4_address) {
            throw std::runtime_error{"cant query the ipv4"};
        }

        _opts->bind_address = *ipv4_address;
        SPDK_NOTICELOG_EX("Use ipv4 %s for listening\n", ipv4_address.value().c_str());
        endpoint ep{_opts->bind_address, _opts->port};
        ep.passive = true;
        _listener = std::make_unique<socket>(ep, *_pd, _channel.value(), false);
    }

    server(const server&) = delete;

    server(server&&) = delete;

    server& operator=(const server&) = delete;

    server& operator=(server&&) = delete;

    ~server() noexcept {
        for (auto& conn : _connections) {
            conn.second->sock->close();
        }
    }

private:

    /*
     * =======================================================================
     * private methods
     * =======================================================================
     */

    bool is_timeout(const rpc_task* task) {
        return task->start_at + _opts->rpc_timeout < std::chrono::system_clock::now();
    }

    int handle_timeout_task(rpc_task* task) {
        SPDK_ERRLOG_EX("ERROR: Timeout on task %d\n", task->id);
        task->reply_status = status::request_timeout;
        auto err = send_reply(task);

        if (err) {
            if (err->value() == ENOMEM) {
                SPDK_NOTICELOG_EX(
                  "Post the reply wr of request %u return enomem\n",
                  task->id);
                return EAGAIN;
            }
            SPDK_ERRLOG_EX(
              "ERROR: Send reply of task '%d' error '%s'\n",
              task->id, err->message().c_str());
              close_connection(task->conn.get());
            return err->value();
        }

        return 0;
    }

    void close_connection(connection_record* conn) {
        conn->recv_pool->put_bulk(std::move(conn->recv_ctx), _opts->per_post_recv_num);
        auto* sock_cm_id = conn->sock->id();
        auto dis_id = conn->dispatch_id.dispatch_id();

        conn->sock->close();
        _connections.erase(sock_cm_id);
        _cm_records.erase(sock_cm_id);
        _cqe_dispatch_map.erase(dis_id);
    }

    auto post_recv(connection_record* conn, memory_pool<::ibv_recv_wr>::net_context* recv_ctx) {
        transport_data::prepare_post_receive(recv_ctx);
        auto old_wr_id = recv_ctx->wr.wr_id;
        conn->dispatch_id.inc_request_id();
        recv_ctx->wr.wr_id = conn->dispatch_id.value();
        recv_ctx->wr.next = nullptr;
        auto rc = conn->sock->receive(&(recv_ctx->wr));
        conn->recv_ctx_map.emplace(recv_ctx->wr.wr_id, recv_ctx);
        conn->recv_ctx_map.erase(old_wr_id);
        SPDK_DEBUGLOG_EX(msg, "posted 1 receive wr\n");

        return rc;
    }

    void per_post_recv(connection_record* conn) {
        conn->recv_ctx = conn->recv_pool->get_bulk(_opts->per_post_recv_num);
        for (size_t i{0}; i < _opts->per_post_recv_num - 1; ++i) {
            transport_data::prepare_post_receive(conn->recv_ctx[i]);
            conn->dispatch_id.inc_request_id();
            conn->recv_ctx[i]->wr.wr_id = conn->dispatch_id.value();
            conn->recv_ctx[i]->wr.next = &(conn->recv_ctx[i + 1]->wr);
            conn->recv_ctx_map.emplace(conn->recv_ctx[i]->wr.wr_id, conn->recv_ctx[i]);
        }

        transport_data::prepare_post_receive(conn->recv_ctx[_opts->per_post_recv_num - 1]);
        conn->dispatch_id.inc_request_id();
        conn->recv_ctx[_opts->per_post_recv_num - 1]->wr.wr_id = conn->dispatch_id.value();
        conn->recv_ctx[_opts->per_post_recv_num - 1]->wr.next = nullptr;
        conn->recv_ctx_map.emplace(
          conn->recv_ctx[_opts->per_post_recv_num - 1]->wr.wr_id,
          conn->recv_ctx[_opts->per_post_recv_num - 1]);

        auto err = conn->sock->receive(&(conn->recv_ctx[0]->wr));
        if (err) {
            SPDK_ERRLOG_EX(
              "ERROR: post %ld receive wrs error, '%s'\n",
              _opts->per_post_recv_num,
              err->message().c_str());
            close_connection(conn);
        }
        SPDK_DEBUGLOG_EX(msg, "post %ld receive wrs\n", _opts->per_post_recv_num);
    }

    void handle_connect_request(::rdma_cm_event* evt) {
        evt->id->pd = _pd->value();
        auto sock = std::make_unique<socket>(evt->id, _cq->cq(), *_pd);
        SPDK_DEBUGLOG_EX(
          msg,
          "add cm observer with id %p, current observers' size is %ld\n",
          sock->id(), _cm_records.size());

        auto rc = sock->accept();
        if (rc) {
            SPDK_ERRLOG_EX(
              "ERROR: Accept error on fd(id: %p): %s\n",
              sock->id(), rc->message().c_str());

            return;
        }
        _cm_records.emplace(sock->id(), std::move(sock));
        SPDK_INFOLOG_EX(msg, "acceptd on rdma cm event(id: %p)\n", evt->id);
    }

    void handle_connection_established(::rdma_cm_event* evt) {
        auto it = _cm_records.find(evt->id);

        if (it == _cm_records.end()) {
            SPDK_ERRLOG_EX(
              "ERROR: Received cm event %s with id(%p) not found in cm observers\n",
              ::rdma_event_str(evt->event), evt->id);

            return;
        }

        auto& sock = it->second;
        SPDK_INFOLOG_EX(
          msg,
          "connection %s => %s:%d established\n",
          sock->peer_address().c_str(),
          _opts->bind_address.c_str(),
          _opts->port);

        auto* pd = sock->pd();
        work_request_id dis_id{_cq_dispatch_id++};
        auto conn = std::make_shared<connection_record>(
          std::move(sock), dis_id,
          std::make_unique<memory_pool<::ibv_recv_wr>>(
            pd, FMT_1("srv_recv_%1%", utils::random_string(5)),
            _opts->per_post_recv_num,
            _opts->metadata_memory_pool_element_size, 0));
        per_post_recv(conn.get());

        _connections.emplace(conn->sock->id(), conn);
        _cqe_dispatch_map.emplace(conn->dispatch_id.dispatch_id(), conn);
    }

    void handle_other_cm_event(::rdma_cm_event* evt) {
        auto it = _cm_records.find(evt->id);
        if (it == _cm_records.end()) {
            SPDK_ERRLOG_EX(
              "ERROR: Received cm event %s, but the cm id can not be found in cm observers\n",
              ::rdma_event_str(evt->event));

            ::rdma_ack_cm_event(evt);
            return;
        }

        auto conn_iter = _connections.find(it->first);
        if (conn_iter == _connections.end()) {
            SPDK_ERRLOG_EX(
              "ERROR: Received cm event %s with rdma cm id(%p) not found in connections\n",
              ::rdma_event_str(evt->event), it->first);
            _cm_records.erase(it);
            ::rdma_ack_cm_event(evt);

            return;
        }

        SPDK_INFOLOG_EX(
          msg,
          "received rdma cm event '%s' on rdma cm id %p\n",
          ::rdma_event_str(evt->event),
          conn_iter->first);

        try {
            conn_iter->second->sock->process_event_directly(evt);
            ::rdma_ack_cm_event(evt);
            evt = nullptr;
        } catch (...) {
            auto evt_id = evt->id;
            ::rdma_ack_cm_event(evt);
            evt = nullptr;

            SPDK_ERRLOG_EX(
              "ERROR: failed process cm event, will close the connection(rdma cm id: %p)\n",
              conn_iter->first);

            SPDK_NOTICELOG_EX("Reomve the connection(id: %p)\n", conn_iter->first);
            close_connection(conn_iter->second.get());
        }

        if (evt) {
            ::rdma_ack_cm_event(evt);
        }
    }

    [[gnu::always_inline]] bool
    is_reach_max_send_wr(connection_record* conn, const size_t send_count) noexcept {
        return conn->onflight_send_wr + send_count >= conn->sock->max_slient_wr() - 1;
    }

    auto send_request(std::function<::ibv_send_wr*(bool)> make_wr, connection_record* conn, const size_t n_wrs) {
        // conn->should_signal = is_reach_max_send_wr(conn, n_wrs);

        auto* head_wr = make_wr(true);
        return conn->sock->send(head_wr);
        // if (conn->should_signal) {
        //     conn->should_signal = false;
        //     conn->onflight_send_wr = n_wrs;
        // } else {
        //     conn->onflight_send_wr += n_wrs;
        // }
        // auto* head_wr = make_wr(true);

        // auto err = conn->sock->send(head_wr);
        // if (not err) {
        //     conn->onflight_send_wr += n_wrs;
        // }

        // return err;
    }

    auto send_metadata_request(connection_record* conn, transport_data* request_data) {
        return send_request([this, conn, request_data] (bool should_signal) {
            return request_data->make_send_request([this, conn] () noexcept {
                conn->dispatch_id.inc_request_id();
                return conn->dispatch_id.value();
            }, should_signal);
        }, conn, request_data->meatdata_capacity());
    }

    auto send_read_request(connection_record* conn, transport_data* request_data) {
        return send_request([this, conn, request_data] (bool should_signal) {
            return request_data->make_read_request([this, conn] () noexcept {
                conn->dispatch_id.inc_request_id();
                return conn->dispatch_id.value();
            }, should_signal);
        }, conn, request_data->data_capacity());
    }

    std::optional<std::error_code> send_reply(rpc_task* task) {
        auto s = task->reply_status.value();
        SPDK_DEBUGLOG_EX(msg, "send reply with status %s\n", string_status(s));
        auto reply_status = std::make_unique<reply_meta>(
          static_cast<std::underlying_type_t<status>>(s));
        std::optional<std::error_code> rc;
        if (s != status::success) {
            task->response_data->serialize_data(reply_status.get());
        } else {
            task->response_data->serialize_data(reply_status.get(), reply_meta_size, task->response_body.get());
        }

        return send_metadata_request(task->conn.get(), task->response_data.get());;
    }

    std::unique_ptr<transport_data> make_response_data(rpc_task* task, status s) {
        task->reply_status = s;
        if (s != status::success) {
            return std::make_unique<transport_data>(
              task->id, reply_meta_size, _meta_pool, _data_pool);
        }

        auto response_size = reply_meta_size + task->response_body->ByteSizeLong();
        return std::make_unique<transport_data>(
          task->id, response_size, _meta_pool, _data_pool);
    }

    void on_response(rpc_task* task) {
        if (task->rpc_ctrlr->Failed()) {
            SPDK_ERRLOG_EX("ERROR: Exec rpc failed: %s\n", task->rpc_ctrlr->ErrorText().c_str());
            task->response_data = make_response_data(task, status::server_error);
            return;
        }

        SPDK_INFOLOG_EX(
          msg,
          "Making rpc response body of request id %d\n",
          task->id);
        task->response_data = make_response_data(task, status::success);
    }

    void dispatch_method(request_meta* meta, rpc_task* task, bool is_inlined = false) {
        std::string_view service_name{meta->service_name, meta->service_name_size};
        std::optional<status> err_status{std::nullopt};
        auto service_it = _services.find(service_name);
        if (service_it == _services.end()) {
            SPDK_ERRLOG_EX(
              "ERROR: can not find service '%.*s'\n",
              meta->service_name_size,
              meta->service_name);
            err_status = status::service_not_found;
        }

        std::string_view method_name{meta->method_name, meta->method_name_size};
        auto method_it = service_it->second->methods.find(method_name);
        if (method_it == service_it->second->methods.end()) {
            SPDK_ERRLOG_EX(
              "ERROR: can not find method '%.*s'\n",
              meta->method_name_size,
              meta->method_name);
            err_status = status::method_not_found;
        }

        SPDK_DEBUGLOG_EX(
          msg,
          "request id %d, found service name: '%.*s', method name: '%.*s', %s => %s\n",
          task->id,
          meta->service_name_size,
          meta->service_name,
          meta->method_name_size,
          meta->method_name,
          task->conn->sock->local_address().c_str(),
          task->conn->sock->peer_address().c_str());

        if (err_status) {
            if (is_inlined) {
                auto rc = post_recv(task->conn.get(), task->recv_ctx);
                if (rc) {
                    SPDK_ERRLOG_EX("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                    close_connection(task->conn.get());
                    return;
                }
            }
            task->request_data.reset(nullptr);
            task->response_data = make_response_data(task, *err_status);
            return;
        }

        task->request_body = std::unique_ptr<google::protobuf::Message>{
          service_it->second->data->GetRequestPrototype(method_it->second).New()};
        bool is_parsed{false};
        if (is_inlined) {
            auto* data = transport_data::read_inlined_content(task->recv_ctx);
            auto data_size = transport_data::read_inlined_content_length(task->recv_ctx);
            SPDK_DEBUGLOG_EX(msg, "unserilaize inlined data, unserialized size is %d\n", data_size);
            is_parsed = task->request_body->ParseFromArray(data + request_meta_size, data_size);

            auto rc = post_recv(task->conn.get(), task->recv_ctx);
            if (rc) {
                SPDK_ERRLOG_EX("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                close_connection(task->conn.get());
            }
        } else {
            SPDK_DEBUGLOG_EX(msg, "unserilaize no inlined data\n");
            is_parsed = task->request_data->unserialize_data(task->request_body.get(), request_meta_size);
        }

        task->request_data.reset(nullptr);
        task->response_body = std::unique_ptr<google::protobuf::Message>{
          service_it->second->data->GetResponsePrototype(method_it->second).New()};
        task->rpc_ctrlr = std::make_unique<rpc_controller>();
        if (not is_parsed) {
            SPDK_ERRLOG_EX("ERROR: Unserialize request body failed\n");
            task->response_data = std::make_unique<transport_data>(
              task->id, reply_meta_size,
              _meta_pool, _data_pool);
            task->response_data = make_response_data(task, status::bad_request_body);

            return;
        }

        task->done = google::protobuf::NewCallback(this, &server::on_response, task);
        service_it->second->data->CallMethod(
          method_it->second,
          task->rpc_ctrlr.get(),
          task->request_body.get(),
          task->response_body.get(),
          task->done);
    }

    int handle_rpc_reply() {
        if (_is_terminated) {
            return SPDK_POLLER_IDLE;
        }

        if (_reply_task_list.empty()) {
            return SPDK_POLLER_IDLE;
        }

        auto& task = _reply_task_list.front();
        if (is_timeout(task.get())) {
            auto rc = handle_timeout_task(task.get());
            if (rc == EAGAIN) {
                return SPDK_POLLER_IDLE;
            }
            _task_list.pop_front();
            return SPDK_POLLER_BUSY;
        }

        if (not task->reply_status) {
            return SPDK_POLLER_IDLE;
        }

        if (not task->response_data->is_ready()) {
            return SPDK_POLLER_IDLE;
        }

        auto err = send_reply(task.get());
        if (err) {
            if (err->value() == ENOMEM) {
                SPDK_NOTICELOG_EX(
                  "Post the reply wr of request %u return enomem\n",
                  task->id);
                return SPDK_POLLER_IDLE;
            }

            SPDK_ERRLOG_EX(
             "ERROR: Send reply of task '%d' error '%s'\n",
              task->id, err->message().c_str());
            close_connection(task->conn.get());

            return SPDK_POLLER_BUSY;
        }
        SPDK_INFOLOG_EX(msg, "send reply of rpc task %d\n", task->id);
        _reply_task_list.pop_front();

        return SPDK_POLLER_BUSY;
    }

    int handle_rdma_read_task() {
        if (_is_terminated) {
            return SPDK_POLLER_IDLE;
        }

        if (_read_task_list.empty()) {
            return SPDK_POLLER_IDLE;
        }

        auto task = _read_task_list.front();
        if (is_timeout(task.get())) {
            auto rc = handle_timeout_task(task.get());
            if (rc == EAGAIN) {
                return SPDK_POLLER_IDLE;
            }
            _task_list.pop_front();
            return SPDK_POLLER_BUSY;
        }

        if (not task->request_data->is_rdma_read_complete()) {
            return SPDK_POLLER_IDLE;
        }

        SPDK_DEBUGLOG_EX(msg, "exec rpc of id %d\n", task->id);
        dispatch_method(
          task->request_data->read_request_meta(),
          task.get());
        _reply_task_list.push_back(task);
        _read_task_list.pop_front();

        return SPDK_POLLER_BUSY;
    }

    int handle_rpc_task() {
        if (_is_terminated) {
            return SPDK_POLLER_IDLE;
        }

        if (_task_list.empty()) {
            return SPDK_POLLER_IDLE;
        }

        auto& task = _task_list.front();
        if (is_timeout(task.get())) {
            auto rc = handle_timeout_task(task.get());
            if (rc == EAGAIN) {
                return SPDK_POLLER_IDLE;
            }
            _task_list.pop_front();
            return SPDK_POLLER_BUSY;
        }

        if (not task->request_data->is_metadata_complete()) {
            SPDK_DEBUGLOG_EX(msg, "rpc task %d, metadata is not complete\n", task->id);
            return SPDK_POLLER_IDLE;
        }

        if (not task->request_data->is_ready()) {
            SPDK_DEBUGLOG_EX(msg, "rpc task %d, transport data is not complete\n", task->id);
            return SPDK_POLLER_IDLE;
        }

        SPDK_DEBUGLOG_EX(msg, "post read wr of task %d\n", task->id);
        auto rc = send_read_request(task->conn.get(), task->request_data.get());
        if (rc and rc->value() == ENOMEM) {
            SPDK_NOTICELOG_EX(
              "Post the read wr of request %d return enomem\n",
              task->id);
            return SPDK_POLLER_IDLE;
        }

        if (rc) {
            SPDK_ERRLOG_EX(
              "ERROR: Post read request of rpc task '%d' error, '%s'\n",
              task->id, rc->message().c_str());
            close_connection(task->conn.get());
        }
        _read_task_list.push_back(task);
        _task_list.pop_front();
        return SPDK_POLLER_BUSY;
    }

    bool handle_cqe_poll() {
        if (_is_terminated) {
            return false;
        }

        if (_cqe_conn_list.empty()) {
            return false;
        }

        auto conn = std::move(_cqe_conn_list.front());
        _cqe_conn_list.pop_front();
        auto cqe = std::move(conn->cqe_list.front());
        conn->cqe_list.pop_front();

        switch (cqe->opcode) {
        case ::IBV_WC_RECV: {
            auto it = conn->recv_ctx_map.find(cqe->wr_id);
            if (it == conn->recv_ctx_map.end()) {
                SPDK_DEBUGLOG_EX(msg, "conn->recv_ctx_map.size(): %lu\n", conn->recv_ctx_map.size());
                SPDK_ERRLOG_EX(
                  "ERROR: Cant find the receive context of wr id '%s'\n",
                  work_request_id::fmt(cqe->wr_id).c_str());
                close_connection(conn.get());
                return true;
            }

            auto recv_ctx = it->second;
            auto task_id = transport_data::read_correlation_index(recv_ctx);
            bool is_no_content = transport_data::is_no_content(recv_ctx);
            SPDK_DEBUGLOG_EX(
              msg,
              "handle cqe of wr id %s on rdma cm id %p, is no content: %d\n",
              work_request_id::fmt(cqe->wr_id).c_str(),
              conn->sock->id(),
              transport_data::is_no_content(recv_ctx));

            if (is_no_content) {
                task_id = ::be32toh(cqe->imm_data);
                SPDK_DEBUGLOG_EX(msg, "free message cqe on task id %d\n", task_id);
                SPDK_INFOLOG_EX(msg, "RPC task of id %d done\n", task_id);
                auto rc = post_recv(conn.get(), recv_ctx);
                if (rc) {
                    SPDK_ERRLOG_EX("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                    close_connection(conn.get());
                }
                conn->rpc_tasks.erase(task_id);
                return true;
            }

            SPDK_DEBUGLOG_EX(msg, "cqe on task id %d\n", task_id);
            auto task_it = conn->rpc_tasks.find(task_id);
            if (task_it == conn->rpc_tasks.end()) {
                SPDK_DEBUGLOG_EX(msg, "new rpc task with correlation index %d\n", task_id);
                auto task = std::make_shared<rpc_task>(task_id, conn);
                if (transport_data::is_inlined(recv_ctx)) {
                    auto* inlined_data = transport_data::read_inlined_content(recv_ctx);
                    auto* meta = reinterpret_cast<request_meta*>(inlined_data);
                    SPDK_DEBUGLOG_EX(
                      msg,
                      "parsed rpc meta, service name is %.*s, method name is %.*s\n",
                      meta->service_name_size,
                      meta->service_name,
                      meta->method_name_size,
                      meta->method_name);
                    task->recv_ctx = recv_ctx;
                    conn->rpc_tasks.emplace(task_id, task);
                    _reply_task_list.push_back(task);
                    dispatch_method(meta, task.get(), true);

                    return true;
                }

                task->request_data = std::make_unique<transport_data>(_data_pool);
                auto [it, _] = conn->rpc_tasks.emplace(task_id, task);
                task_it = it;
                _task_list.push_back(task);
            }

            SPDK_DEBUGLOG_EX(msg, "handle rpc task with id %d, _task_list size is %lu\n", task_id, _task_list.size());
            auto* task = task_it->second.get();
            task->request_data->from_net_context(recv_ctx);
            auto rc = post_recv(conn.get(), recv_ctx);
            if (rc) {
                SPDK_ERRLOG_EX("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                close_connection(conn.get());
            }

            return true;
        }
        default:
            return true;
        }
    }

public:

    /*
     * =======================================================================
     * spdk callbacks
     * =======================================================================
     */

    static void on_start(void* arg) {
        auto* this_server = reinterpret_cast<server*>(arg);
        this_server->handle_start();
    }

    static void on_stop(void* arg) {
        auto* ctx = reinterpret_cast<stop_context*>(arg);
        ctx->this_server->handle_stop(std::unique_ptr<stop_context>{ctx});
    }

    static void on_add_service(void* arg) {
        auto* ctx = reinterpret_cast<add_service_ctx*>(arg);
        ctx->this_server->handle_add_service(std::unique_ptr<add_service_ctx>{ctx});
    }

    static int ib_cm_event_poller(void* arg) {
        auto* this_server = reinterpret_cast<server*>(arg);
        return this_server->handle_ib_cm_event_poll();
    }

    static int cq_poller(void* arg) {
        auto* this_server = reinterpret_cast<server*>(arg);
        return this_server->handle_cq_poll();
    }

    static int cqe_poller(void* arg) {
        auto* this_server = reinterpret_cast<server*>(arg);
        return this_server->handle_cqe_poll();
    }

    static int task_poller(void* arg) {
        auto* this_server = reinterpret_cast<server*>(arg);
        return this_server->handle_rpc_task();
    }

    static int task_read_poller(void* arg) {
        auto* this_server = reinterpret_cast<server*>(arg);
        return this_server->handle_rdma_read_task();
    }

    static int task_reply_poller(void* arg) {
        auto* this_server = reinterpret_cast<server*>(arg);
        return this_server->handle_rpc_reply();
    }

public:

    /*
     * =======================================================================
     * class callbacks
     * =======================================================================
     */

    void handle_start() {
        _ib_cm_event_poller.register_poller(ib_cm_event_poller, this, 0);
        _cq_poller.register_poller(cq_poller, this, 0);
        _cqe_poller.register_poller(cqe_poller, this, 0);
        _task_poller.register_poller(task_poller, this, 0);
        _task_read_poller.register_poller(task_read_poller, this, 0);
        _task_reply_poller.register_poller(task_reply_poller, this, 0);
        SPDK_INFOLOG_EX(msg, "Rpc server started\n");
    }

    void handle_stop(std::unique_ptr<stop_context> ctx) {
        if (_is_terminated) {
            return;
        }

        _is_terminated = true;
        _ib_cm_event_poller.unregister_poller();
        _cq_poller.unregister_poller();
        _cqe_poller.unregister_poller();
        _task_poller.unregister_poller();
        _task_read_poller.unregister_poller();
        _task_reply_poller.unregister_poller();
        SPDK_NOTICELOG_EX("Pollers of rpc server have been unregistered\n");

        _meta_pool->free();
        _data_pool->free();

        if (ctx and ctx->on_stop_cb) {
            try {
                (ctx->on_stop_cb.value())();
            } catch (const std::exception& e) {
                SPDK_ERRLOG_EX("Caught exception when executing callback on stopping rpc server: %s\n", e.what());
            }
        }
    }

    void handle_add_service(std::unique_ptr<add_service_ctx> ctx) {
        auto* p = ctx->service_ptr;
        auto descriptor = const_cast<google::protobuf::ServiceDescriptor*>(p->GetDescriptor());
        auto service_val = std::make_shared<service>(p, descriptor);
        _services.emplace(service_val->descriptor->name(), service_val);
    }

    int handle_ib_cm_event_poll() {
        if (_is_terminated) {
            return SPDK_POLLER_IDLE;
        }

        _dev->process_ib_event();
        auto evt = _channel.poll();
        if (not evt) { return SPDK_POLLER_IDLE; }

        SPDK_INFOLOG_EX(
          msg,
          "receive rdma cm event: %s, cm id: %p\n",
          ::rdma_event_str(evt->event), evt->id);

        switch (evt->event) {
        case ::RDMA_CM_EVENT_CONNECT_REQUEST: {
            handle_connect_request(evt);
            ::rdma_ack_cm_event(evt);
            break;
        }

        case ::RDMA_CM_EVENT_ESTABLISHED: {
            handle_connection_established(evt);
            ::rdma_ack_cm_event(evt);
            break;
        }

        default:
            /*
             * 这里的 rdma_ack_cm_event(evt) 调用挪到了 handle_other_cm_event() 内
             * 否则在调用 rdma_destroy_id() 前如果没有先调用 rdma_ack_cm_event()，会死锁
             */
            handle_other_cm_event(evt);
            break;
        }

        return SPDK_POLLER_BUSY;
    }

    int handle_cq_poll() {
        if (_is_terminated) { return SPDK_POLLER_IDLE; }

        auto rc = _cq->poll(_wcs.get(), _opts->poll_cq_batch_size);
        if (rc < 0) {
            SPDK_ERRLOG_EX("ERROR: Poll cq error '%s', stop the server\n", std::strerror(errno));
            handle_stop(nullptr);
            return SPDK_POLLER_IDLE;
        }

        if (rc == 0) {
            return SPDK_POLLER_IDLE;
        }

        ::ibv_wc* cqe{nullptr};
        work_request_id::dispatch_id_type dis_id{};
        decltype(_cqe_dispatch_map)::iterator dispatch_it{};
        socket* sock{nullptr};
        bool is_reset_signal_flag{false};
        for (int i{0}; i < rc; ++i) {
            cqe = &(_wcs[i]);
            SPDK_DEBUGLOG_EX(msg, "polled cqe with opcode %s\n", completion_queue::op_name(cqe->opcode).c_str());
            if (cqe->opcode != ::IBV_WC_RECV) {
                continue;
            }

            dis_id = work_request_id::dispatch_id(cqe->wr_id);
            auto dispatch_it = _cqe_dispatch_map.find(dis_id);
            if (dispatch_it == _cqe_dispatch_map.end()) {
                SPDK_NOTICELOG_EX(
                  "cant find the connection with dispatch id %ld, work request id %ld, cqe opcode is %s\n",
                  dis_id, cqe->wr_id, completion_queue::op_name(cqe->opcode).c_str());
                continue;
            }

            auto conn = dispatch_it->second;
            if (cqe->status != ::IBV_WC_SUCCESS) {
                sock = conn->sock.get();
                if (not sock->is_closed()) {
                    sock->update_qp_attr();
                    SPDK_ERRLOG_EX(
                      "ERROR: Got error wc, wc.vendor_err=%u, wc.status=%s, qp.state=%s\n",
                      cqe->vendor_err,
                      socket::wc_status_name(cqe->status).c_str(),
                      sock->qp_state_str().c_str());
                    sock->close();
                }

                _cqe_dispatch_map.erase(dispatch_it);
                _connections.erase(sock->id());
                _cm_records.erase(sock->id());
                continue;
            }

            auto cqe_cpy = std::make_unique<::ibv_wc>(_wcs[i]);
            conn->cqe_list.push_back(std::move(cqe_cpy));
            _cqe_conn_list.push_back(conn);
        }

        return SPDK_POLLER_BUSY;
    }

public:

    /*
     * =======================================================================
     * public methods
     * =======================================================================
     */

    void start() {
        auto ec = _listener->listen(_opts->listen_backlog);
        if (ec) {
            SPDK_ERRLOG_EX(
              "listen on %s:%d error: %s\n",
              _opts->bind_address.c_str(),
              _opts->port,
              ec->message().c_str());
            throw std::runtime_error{"server listen error"};
        }
        SPDK_INFOLOG_EX(msg, "Start listening...\n");
        ::spdk_thread_send_msg(_thread, on_start, this);
    }

    void stop(std::optional<std::function<void()>>&& cb = std::nullopt) {
        if (_is_terminated) {
            return;
        }

        auto* ctx = new stop_context{std::move(cb), this};
        ::spdk_thread_send_msg(_thread, on_stop, ctx);
    }

    void add_service(service::pointer p) {
        auto* ctx = new add_service_ctx{p, this};
        ::spdk_thread_send_msg(_thread, on_add_service, ctx);
    }

    std::string listen_address() noexcept {
        return _opts->bind_address;
    }

public:

    static void process_ib_event(::ibv_context* context, ::ibv_async_event* event) {
        SPDK_DEBUGLOG_EX(
          msg, "received ib event(%s) on %s\n",
          ::ibv_event_type_str(event->event_type),
          ::ibv_get_device_name(context->device));

        switch (event->event_type) {
        case ::IBV_EVENT_CQ_ERR:
            SPDK_ERRLOG_EX(
              "ERROR: Received ib event(IBV_EVENT_QP_FATAL: %s) on %s, cq is %p, "
              "may the opts 'msg_rdma_cq_num_entries' is too small",
              ::ibv_event_type_str(event->event_type),
              ::ibv_get_device_name(context->device),
              event->element.cq);
            break;
        case ::IBV_EVENT_QP_FATAL:
            SPDK_ERRLOG_EX(
              "ERROR: Received ib event(IBV_EVENT_QP_FATAL: %s) on %s, qp is %p\n",
              ::ibv_event_type_str(event->event_type),
              ::ibv_get_device_name(context->device),
              event->element.qp);
            break;
        case ::IBV_EVENT_QP_LAST_WQE_REACHED:
            // 在 SRQ 上才会有的事件，我们暂时没用到
            break;
        case ::IBV_EVENT_SQ_DRAINED: {
            // 发送这个事件时，qp 可能出错，也可能没有
            auto sock = reinterpret_cast<socket*>(
              event->element.qp->qp_context);
            auto state = sock->update_qp_attr();

            if (state == IBV_QPS_ERR) {
                SPDK_ERRLOG_EX("qp error, will close connection\n");
            }

            break;
        }

        case ::IBV_EVENT_QP_REQ_ERR:
	    case ::IBV_EVENT_QP_ACCESS_ERR:
	    case ::IBV_EVENT_COMM_EST:
	    case ::IBV_EVENT_PATH_MIG:
	    case ::IBV_EVENT_PATH_MIG_ERR:
        case ::IBV_EVENT_WQ_FATAL:
	    case ::IBV_EVENT_DEVICE_FATAL:
	    case ::IBV_EVENT_PORT_ACTIVE:
	    case ::IBV_EVENT_PORT_ERR:
	    case ::IBV_EVENT_LID_CHANGE:
	    case ::IBV_EVENT_PKEY_CHANGE:
	    case ::IBV_EVENT_SM_CHANGE:
	    case ::IBV_EVENT_SRQ_ERR:
	    case ::IBV_EVENT_SRQ_LIMIT_REACHED:
	    case ::IBV_EVENT_CLIENT_REREGISTER:
	    case ::IBV_EVENT_GID_CHANGE:
            SPDK_ERRLOG_EX(
              "received ib event(%s) on %s\n",
              ::ibv_event_type_str(event->event_type),
              ::ibv_get_device_name(context->device));
            break;
        }
    }

private:

    ::spdk_cpuset _cpumask{};
    ::spdk_thread* _thread{nullptr};
    bool _is_terminated{false};
    std::shared_ptr<options> _opts{nullptr};
    std::shared_ptr<device> _dev{nullptr};
    std::unique_ptr<protection_domain> _pd{nullptr};
    std::shared_ptr<completion_queue> _cq{nullptr};
    std::unique_ptr<socket> _listener{nullptr};
    utils::simple_poller _ib_cm_event_poller{};
    utils::simple_poller _cq_poller{};
    utils::simple_poller _cqe_poller{};
    utils::simple_poller _task_poller{};
    utils::simple_poller _task_read_poller{};
    utils::simple_poller _task_reply_poller{};
    std::unordered_map<::rdma_cm_id*, std::unique_ptr<socket>> _cm_records{};
    connection_id::serial_type _serial{0};
    work_request_id::connection_id_type _cq_dispatch_id{1};
    std::unordered_map<::rdma_cm_id*, std::shared_ptr<connection_record>> _connections{};
    std::unordered_map<work_request_id::dispatch_id_type, std::shared_ptr<connection_record>> _cqe_dispatch_map{};
    std::unique_ptr<::ibv_wc[]> _wcs{nullptr};
    std::unordered_map<std::string_view, std::shared_ptr<service>> _services{};

    std::shared_ptr<memory_pool<::ibv_send_wr>> _meta_pool{nullptr};
    std::shared_ptr<memory_pool<::ibv_send_wr>> _data_pool{nullptr};
    std::shared_ptr<memory_pool<::ibv_recv_wr>> _recv_pool{nullptr};
    std::list<std::shared_ptr<rpc_task>> _task_list{};
    std::list<std::shared_ptr<rpc_task>> _read_task_list{};
    std::list<std::shared_ptr<rpc_task>> _reply_task_list{};
    std::list<std::shared_ptr<connection_record>> _cqe_conn_list{};

    event_channel _channel{};
};

} // namespace rdma
} // namespace msg
