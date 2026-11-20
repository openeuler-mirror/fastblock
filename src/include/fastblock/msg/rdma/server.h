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

#include "fastblock/msg/rdma/completed_key_ring.h"
#include "fastblock/msg/rdma/connection_id.h"
#include "fastblock/msg/rdma/cq.h"
#include "fastblock/msg/rdma/probe.h"
#include "fastblock/msg/rdma/socket.h"
#include "fastblock/msg/rdma/oob_payload.h"
#include "fastblock/msg/rdma/transport_data.h"
#include "fastblock/msg/rdma/work_request_id.h"
#include "fastblock/msg/rpc_controller.h"
#include "fastblock/utils/simple_poller.h"
#include "fastblock/utils/utils.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <boost/property_tree/ptree.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include <endian.h>

#define fb_rpc_server_key(k) "msg_server_"#k
#define conf_or_server(json_conf, opts, opts_key) conf_or_s(json_conf, fb_rpc_server_key(opts_key), opts, opts_key)
#define fb_rpc_server_printv(k, v, os) os << fb_rpc_server_key(k) << ": " << v << "\n"
#define fb_rpc_server_print_suf(k, v, suf, os) os << fb_rpc_server_key(k) << ": " << v << suf << "\n"
#define fb_rpc_server_print(k, o, os) fb_rpc_server_printv(k, o.k, os)
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
        std::unordered_map<uint32_t, rpc_task*> rpc_tasks{};
        completed_key_ring done_keys{};
        std::list<std::unique_ptr<::ibv_wc>> cqe_list{};
        bool should_signal{false};
        int32_t onflight_send_wr{0};
    };

    struct rpc_task {
        using server_id_type = uint64_t;

        rpc_task(
          server_id_type sid,
          transport_data::correlation_index_type cid,
          std::shared_ptr<connection_record> c)
          : server_id{sid}, id{cid}, conn{std::move(c)} {}

        server_id_type server_id{0};
        transport_data::correlation_index_type id{0};
        std::shared_ptr<connection_record> conn{};
        std::unique_ptr<google::protobuf::Message> request_body{nullptr};
        std::unique_ptr<google::protobuf::Message> response_body{nullptr};
        std::unique_ptr<rpc_controller> rpc_ctrlr{nullptr};
        google::protobuf::Closure* done{nullptr};
        std::unique_ptr<transport_data> request_data{nullptr};
        std::unique_ptr<transport_data> response_data{nullptr};
        std::string response_payload{};
        memory_pool<::ibv_recv_wr>::net_context* recv_ctx{nullptr};
        std::optional<status> reply_status{};
        std::chrono::system_clock::time_point start_at{std::chrono::system_clock::now()};
        server* this_server{nullptr};
        // Set when a reply has been queued. CallMethod may still be using
        // request/response/done; the unique_ptr stays in _on_fligh_tasks
        // until handle_rpc_done.
        bool replied{false};
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

    struct stop_context {
        server* this_server{nullptr};
        std::optional<std::function<void()>> on_stop_cb{std::nullopt};
        std::chrono::system_clock::time_point timeout_at{std::chrono::system_clock::now()};
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
        size_t pool_low_watermark{0};
        size_t pool_high_watermark{0};
        size_t reserved_per_connection{0};
        size_t reserved_priority{0};
        std::chrono::system_clock::duration rpc_timeout{
          std::chrono::microseconds{default_rpc_timeout_us}};
        std::chrono::system_clock::duration shutdown_timeout{std::chrono::microseconds{0}};
        std::chrono::system_clock::duration slow_method{std::chrono::seconds{1}};
        std::unique_ptr<endpoint> ep{nullptr};

        friend std::ostream& operator<<(std::ostream& os, const options& o) {
            os << "[server]\n";
            fb_rpc_server_print(poll_cq_batch_size, o, os);
            fb_rpc_server_print(metadata_memory_pool_capacity, o, os);
            fb_rpc_server_print(metadata_memory_pool_element_size, o, os);
            fb_rpc_server_print(data_memory_pool_capacity, o, os);
            fb_rpc_server_print(data_memory_pool_element_size, o, os);
            fb_rpc_server_print(per_post_recv_num, o, os);
            fb_rpc_server_print(pool_low_watermark, o, os);
            fb_rpc_server_print(pool_high_watermark, o, os);
            fb_rpc_server_print(reserved_per_connection, o, os);
            fb_rpc_server_print(reserved_priority, o, os);

            auto rpc_timeout_interval = std::chrono::duration_cast<std::chrono::microseconds>(o.rpc_timeout).count();
            fb_rpc_server_print_suf(rpc_timeout_us, rpc_timeout_interval, "us", os);

            auto shutdown_us = std::chrono::duration_cast<std::chrono::microseconds>(o.shutdown_timeout).count();
            fb_rpc_server_print_suf(shutdown_timeout_us, shutdown_us, "us", os);

            auto slow_method_interval = std::chrono::duration_cast<std::chrono::microseconds>(o.slow_method).count();
            fb_rpc_server_print_suf(slow_method_us, slow_method_interval, "us", os);

            os << *(o.ep);

            return os;
        }
    };

    static std::shared_ptr<options> make_options(boost::property_tree::ptree& conf) {
        auto opts = std::make_shared<options>();
        conf_or_server(conf, opts, listen_backlog);
        conf_or_server(conf, opts, poll_cq_batch_size);
        conf_or_server(conf, opts, metadata_memory_pool_capacity);
        conf_or_server(conf, opts, metadata_memory_pool_element_size);
        conf_or_server(conf, opts, data_memory_pool_capacity);
        conf_or_server(conf, opts, data_memory_pool_element_size);
        conf_or_server(conf, opts, per_post_recv_num);
        conf_or_server(conf, opts, pool_low_watermark);
        conf_or_server(conf, opts, pool_high_watermark);
        conf_or_server(conf, opts, reserved_per_connection);
        conf_or_server(conf, opts, reserved_priority);

        if (conf.count("msg_server_rpc_timeout_us") != 0) {
            auto timeout_us = conf.get_child("msg_server_rpc_timeout_us").get_value<int64_t>();
            opts->rpc_timeout = std::chrono::microseconds{timeout_us};
        }

        if (conf.count("msg_client_shutdown_timeout_us") != 0) {
            auto shutdown_timeout_us = conf.get_child("msg_client_shutdown_timeout_us").get_value<int64_t>();
            opts->shutdown_timeout = std::chrono::microseconds{shutdown_timeout_us};
        }

        if (conf.count("msg_server_slow_method_us") != 0) {
            auto slow_method_us = conf.get_child("msg_server_slow_method_us").get_value<int64_t>();
            opts->slow_method = std::chrono::microseconds{slow_method_us};
        }

        if (conf.count("msg_server_bind_address") != 0) {
            opts->bind_address = conf.get_child("msg_server_bind_address").get_value<std::string>();
        }

        opts->ep = std::make_unique<endpoint>(conf);

        return opts;
    }

public:

    server() = delete;

    server(::spdk_thread* thread, std::shared_ptr<options> opts, int sock_id = SPDK_ENV_SOCKET_ID_ANY)
      : _thread{thread}
      , _ib_cm_event_poller{_thread}
      , _cq_poller{_thread}
      , _cqe_poller{_thread}
      , _task_poller{_thread}
      , _task_read_poller{_thread}
      , _task_reply_poller{_thread}
      , _stop_poller{_thread}
      , _opts{std::move(opts)}
      , _dev{std::make_shared<device>(process_ib_event)}
      , _pd{std::make_unique<protection_domain>(_dev, _opts->ep->device_name)}
      , _cq{std::make_shared<completion_queue>(_opts->ep->cq_num_entries, *_pd)}
      , _listener{nullptr}
      , _wcs{std::make_unique<::ibv_wc[]>(_opts->poll_cq_batch_size)}
      , _sock_id{sock_id}
      , _meta_pool{
        std::make_shared<memory_pool<::ibv_send_wr>>(
        _pd->value(), "srv_meta",
        _opts->metadata_memory_pool_capacity,
        _opts->metadata_memory_pool_element_size, 0, _sock_id)}
      , _data_pool{std::make_shared<memory_pool<::ibv_send_wr>>(
        _pd->value(), "srv_data",
        _opts->data_memory_pool_capacity,
        _opts->data_memory_pool_element_size, 0, _sock_id)} {
        pool_quota q{};
        q.low_watermark = _opts->pool_low_watermark;
        q.high_watermark = _opts->pool_high_watermark;
        q.reserved_per_connection = _opts->reserved_per_connection;
        q.reserved_priority = _opts->reserved_priority;
        _meta_pool->configure_quota(q);
        _data_pool->configure_quota(q);
        if (_opts->bind_address.empty()) {
            auto ipv4_address = _dev->get_ipv4(_opts->ep->device_name);

            if (not ipv4_address) {
                throw std::runtime_error{"cant query the ipv4"};
            }

            _opts->bind_address = *ipv4_address;
            SPDK_INFOLOG(msg, "Use ipv4 %s for listening\n", ipv4_address.value().c_str());
        } else {
            SPDK_INFOLOG(msg, "Use configured ipv4 %s for listening\n", _opts->bind_address.c_str());
        }
    }

    server(std::string thread_name, ::spdk_cpuset* cpumask, std::shared_ptr<options> opts, int sock_id = SPDK_ENV_SOCKET_ID_ANY)
      : server{::spdk_thread_create(thread_name.c_str(), cpumask), opts, sock_id} {
        // Only this ctor creates the thread; OSD passes a borrowed shard thread.
        _owns_thread = (_thread != nullptr);
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

    bool is_timeout(const rpc_task* task) noexcept {
        return task->start_at + _opts->rpc_timeout < std::chrono::system_clock::now();
    }

    bool is_all_tasks_done() noexcept {
        return _task_list.empty() and
               _read_task_list.empty() and
               _on_fligh_tasks.empty() and
               _reply_task_list.empty();
    }

    void handle_timeout_task(std::unique_ptr<rpc_task> task) {
        SPDK_ERRLOG("ERROR: Timeout on task %lu\n", task->id);
        ++rdma_probe.rpc_timeout;
        ++rdma_probe.rpc_complete_fail;
        enqueue_reply(std::move(task), status::request_timeout);
    }

    // CallMethod already owns request/response/done. Reply timeout on a
    // clone; leave the original in _on_fligh_tasks until handle_rpc_done.
    void timeout_inflight_task(rpc_task* task) {
        if (task == nullptr || task->replied) {
            return;
        }
        SPDK_ERRLOG(
          "ERROR: Timeout on inflight task %lu, keep until service done\n",
          task->id);
        ++rdma_probe.rpc_timeout;
        auto reply = std::make_unique<rpc_task>(task->server_id, task->id, task->conn);
        reply->this_server = this;
        reply->start_at = task->start_at;
        if (enqueue_reply(std::move(reply), status::request_timeout)) {
            task->replied = true;
            ++rdma_probe.rpc_complete_fail;
        }
    }

    void forget_rpc_task(
      connection_record* conn, transport_data::correlation_index_type full) {
        if (not conn) {
            return;
        }
        conn->done_keys.remember(full);
        conn->rpc_tasks.erase(transport_data::correlation_index_lo(full));
    }

    void drop_rpc_task(std::unique_ptr<rpc_task> task) {
        if (task and task->conn) {
            forget_rpc_task(task->conn.get(), task->id);
        }
    }

    bool enqueue_reply(std::unique_ptr<rpc_task> task, status s) {
        if (not make_response_data(task.get(), s) or
            not task->response_data or
            not task->response_data->prepared()) {
            ++rdma_probe.rpc_pool_exhausted;
            ++rdma_probe.rpc_complete_fail;
            SPDK_WARNLOG(
              "drop rpc task %lu, cannot prepare reply status %s, data_free=%zu pool_exhausted=%lu\n",
              task->id, string_status(s),
              _data_pool ? _data_pool->size() : 0,
              rdma_probe.rpc_pool_exhausted);
            drop_rpc_task(std::move(task));
            return false;
        }
        if (task->conn) {
            forget_rpc_task(task->conn.get(), task->id);
        }
        _reply_task_list.push_back(std::move(task));
        return true;
    }

    void process_timeout_scan() {
        auto expire_list = [this](std::list<std::unique_ptr<rpc_task>>& lst) {
            for (auto it = lst.begin(); it != lst.end(); ) {
                if (is_timeout(it->get())) {
                    auto task = std::move(*it);
                    it = lst.erase(it);
                    handle_timeout_task(std::move(task));
                } else {
                    ++it;
                }
            }
        };

        expire_list(_task_list);
        expire_list(_read_task_list);

        for (auto& kv : _on_fligh_tasks) {
            if (is_timeout(kv.second.get()) && not kv.second->replied) {
                timeout_inflight_task(kv.second.get());
            }
        }
    }

    void close_connection(connection_record* conn) {
        ++rdma_probe.rpc_qp_teardown;
        SPDK_ERRLOG(
          "qp teardown server close_connection qp_teardown=%lu (not unknown_key)\n",
          rdma_probe.rpc_qp_teardown);
        _meta_pool->remove_connection();
        _data_pool->remove_connection();
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
        SPDK_DEBUGLOG(msg, "posted 1 receive wr\n");

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
            SPDK_ERRLOG(
              "ERROR: post %ld receive wrs error, '%s'\n",
              _opts->per_post_recv_num,
              err->message().c_str());
            close_connection(conn);
        }
        SPDK_DEBUGLOG(msg, "post %ld receive wrs\n", _opts->per_post_recv_num);
    }

    void handle_connect_request(::rdma_cm_event* evt) {
        evt->id->pd = _pd->value();
        auto sock = std::make_unique<socket>(evt->id, _cq->cq(), *_pd);
        SPDK_DEBUGLOG(
          msg,
          "add cm observer with id %p, current observers' size is %ld\n",
          sock->id(), _cm_records.size());

        auto rc = sock->accept();
        if (rc) {
            SPDK_ERRLOG(
              "ERROR: Accept error on fd(id: %p): %s\n",
              sock->id(), rc->message().c_str());

            return;
        }
        _cm_records.emplace(sock->id(), std::move(sock));
        SPDK_INFOLOG(msg, "acceptd on rdma cm event(id: %p)\n", evt->id);
    }

    void handle_connection_established(::rdma_cm_event* evt) {
        auto it = _cm_records.find(evt->id);

        if (it == _cm_records.end()) {
            SPDK_ERRLOG(
              "ERROR: Received cm event %s with id(%p) not found in cm observers\n",
              ::rdma_event_str(evt->event), evt->id);

            return;
        }

        auto& sock = it->second;
        SPDK_INFOLOG(
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
            pd, FB_FMT_1("srv_recv_%1%", utils::random_string(5)),
            _opts->per_post_recv_num,
            _opts->metadata_memory_pool_element_size, 0, _sock_id));
        per_post_recv(conn.get());
        _meta_pool->add_connection();
        _data_pool->add_connection();

        _connections.emplace(conn->sock->id(), conn);
        _cqe_dispatch_map.emplace(conn->dispatch_id.dispatch_id(), conn);
    }

    void handle_other_cm_event(::rdma_cm_event* evt) {
        auto it = _cm_records.find(evt->id);
        if (it == _cm_records.end()) {
            SPDK_ERRLOG(
              "ERROR: Received cm event %s, but the cm id can not be found in cm observers\n",
              ::rdma_event_str(evt->event));

            ::rdma_ack_cm_event(evt);
            return;
        }

        auto conn_iter = _connections.find(it->first);
        if (conn_iter == _connections.end()) {
            SPDK_ERRLOG(
              "ERROR: Received cm event %s with rdma cm id(%p) not found in connections\n",
              ::rdma_event_str(evt->event), it->first);
            _cm_records.erase(it);
            ::rdma_ack_cm_event(evt);

            return;
        }

        SPDK_INFOLOG(
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

            SPDK_ERRLOG(
              "ERROR: failed process cm event, will close the connection(rdma cm id: %p)\n",
              conn_iter->first);

            SPDK_NOTICELOG("Reomve the connection(id: %p)\n", conn_iter->first);
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
        ::ibv_send_wr* head_wr{nullptr};
        if (_opts->ep->qp_sig_all) {
            head_wr = make_wr(true);
        } else {
            conn->should_signal = is_reach_max_send_wr(conn, n_wrs);
            head_wr = make_wr(conn->should_signal);
            conn->onflight_send_wr += n_wrs;
        }

        return conn->sock->send(head_wr);
    }

    auto send_metadata_request(connection_record* conn, transport_data* request_data) {
        auto ret = send_request([this, conn, request_data] (bool should_signal) {
            return request_data->make_send_request([this, conn] () noexcept {
                conn->dispatch_id.inc_request_id();
                return conn->dispatch_id.value();
            }, should_signal);
        }, conn, request_data->meatdata_capacity());

        if (ret) { request_data->mark_bad_send(ret->bad_wr); }

        return ret;
    }

    auto send_read_request(connection_record* conn, transport_data* request_data) {
        auto ret = send_request([this, conn, request_data] (bool should_signal) {
            return request_data->make_read_request([this, conn] () noexcept {
                conn->dispatch_id.inc_request_id();
                return conn->dispatch_id.value();
            }, should_signal);
        }, conn, request_data->data_capacity());

        if (ret) { request_data->mark_bad_read(ret->bad_wr); }

        return ret;
    }

    auto send_reply(rpc_task* task) {
        auto s = task->reply_status.value();
        SPDK_DEBUGLOG(msg, "send reply with status %s\n", string_status(s));
        auto reply_status = std::make_unique<reply_meta>();
        reply_status->reply_status = static_cast<std::underlying_type_t<status>>(s);
        if (s == status::success && task->response_body) {
            reply_status->data_size = static_cast<uint32_t>(
              task->response_body->ByteSizeLong());
            reply_status->payload_size =
              static_cast<uint32_t>(task->response_payload.size());
        }
        if (s != status::success) {
            task->response_data->serialize_data(reply_status.get());
        } else {
            task->response_data->serialize_data(
              reply_status.get(), reply_meta_size, task->response_body.get(),
              task->response_payload);
        }

        return send_metadata_request(task->conn.get(), task->response_data.get());;
    }

    bool make_response_data(rpc_task* task, status s) {
        task->reply_status = s;
        if (s != status::success) {
            SPDK_DEBUGLOG(msg, "task id is %lu, status is not success\n", task->id);
            task->response_data = std::make_unique<transport_data>(
              task->id, reply_meta_size, _meta_pool, _data_pool);
        } else {
            peel_rpc_payload(*task->response_body, &task->response_payload);
            auto env_size = task->response_body->ByteSizeLong();
            auto response_size =
              reply_meta_size + env_size + task->response_payload.size();
            SPDK_DEBUGLOG(msg, "task id is %lu, status is success\n", task->id);
            task->response_data = std::make_unique<transport_data>(
              task->id, response_size, _meta_pool, _data_pool);
        }
        if (not task->response_data or not task->response_data->prepared()) {
            SPDK_NOTICELOG(
              "failed to prepare response for task %lu status %s\n",
              task->id, string_status(s));
            task->response_data.reset();
            return false;
        }
        return true;
    }

    void on_response(rpc_task* task) {
        auto dur = std::chrono::system_clock::now() - task->start_at;
        if (dur >= _opts->slow_method) {
            SPDK_WARNLOG(
              "slow rpc method detected, task id is %lu, dur is %ldms\n",
              task->id, dur.count() / 1000 / 1000);
        }

        auto cur_thread = spdk_get_thread();
        if(_thread != cur_thread){
            SPDK_DEBUGLOG(msg, "current thread %lu, thread %lu\n", spdk_thread_get_id(cur_thread), spdk_thread_get_id(_thread));
            auto rc = ::spdk_thread_send_msg(_thread, on_rpc_done, task);
            if (rc != 0) {
                SPDK_ERRLOG("send msg failed, rc is %d, task id is %lu\n", rc, task->id);
                throw std::runtime_error{"send msg failed"};
            }
        } else {
            handle_rpc_done(task);
        }
    }

    void dispatch_method(const request_meta& meta, std::unique_ptr<rpc_task> task, bool is_inlined = false) {
        std::string_view service_name{meta.service_name, meta.service_name_size};
        std::optional<status> err_status{std::nullopt};
        auto service_it = _services.find(service_name);
        if (service_it == _services.end()) {
            SPDK_ERRLOG(
              "ERROR: can not find service '%.*s'\n",
              meta.service_name_size,
              meta.service_name);
            err_status = status::service_not_found;
        }

        std::string_view method_name{meta.method_name, meta.method_name_size};
        service::method_descriptor_pointer method{nullptr};
        if (!err_status) {
            auto method_it = service_it->second->methods.find(method_name);
            if (method_it != service_it->second->methods.end()) {
                method = method_it->second;
            }
        }
        if (!method) {
            SPDK_ERRLOG(
              "ERROR: can not find method '%.*s'\n",
              meta.method_name_size,
              meta.method_name);
            if (!err_status) {
                err_status = status::method_not_found;
            }
        }

        SPDK_DEBUGLOG(
          msg,
          "request id %lu, found service name: '%.*s', method name: '%.*s', %s => %s\n",
          task->id,
          meta.service_name_size,
          meta.service_name,
          meta.method_name_size,
          meta.method_name,
          task->conn->sock->local_address().c_str(),
          task->conn->sock->peer_address().c_str());

        if (err_status) {
            if (is_inlined) {
                auto rc = post_recv(task->conn.get(), task->recv_ctx);
                if (rc) {
                    SPDK_ERRLOG("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                    close_connection(task->conn.get());
                    return;
                }
            }
            task->request_data.reset(nullptr);
            enqueue_reply(std::move(task), *err_status);
            return;
        }

        task->request_body = std::unique_ptr<google::protobuf::Message>{
          service_it->second->data->GetRequestPrototype(method).New()};
        bool is_parsed{false};
        if (is_inlined) {
            auto* data = transport_data::read_inlined_content(task->recv_ctx);
            auto req_h = load_request_meta(data);
            auto data_size = req_h.data_size != 0
              ? req_h.data_size
              : transport_data::read_inlined_content_length(task->recv_ctx);
            SPDK_DEBUGLOG(msg, "unserilaize inlined data, unserialized size is %d\n", data_size);
            is_parsed = task->request_body->ParseFromArray(data + request_meta_size, data_size);
            if (is_parsed && req_h.payload_size > 0) {
                is_parsed = attach_rpc_payload(
                  *task->request_body,
                  std::string_view(data + request_meta_size + data_size,
                                   req_h.payload_size));
            }

            auto rc = post_recv(task->conn.get(), task->recv_ctx);
            if (rc) {
                SPDK_ERRLOG("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                close_connection(task->conn.get());
            }
        } else {
            SPDK_DEBUGLOG(msg, "unserilaize no inlined data\n");
            std::string_view req_payload;
            size_t env_sz = 0;
            if (auto* head = task->request_data->data_head(); head != nullptr) {
                env_sz = load_request_meta(head).data_size;
            }
            is_parsed = task->request_data->unserialize_data(
              task->request_body.get(), request_meta_size, env_sz, &req_payload);
            if (is_parsed && !req_payload.empty()) {
                is_parsed = attach_rpc_payload(*task->request_body, req_payload);
            }
        }

        task->request_data.reset(nullptr);
        task->response_body = std::unique_ptr<google::protobuf::Message>{
          service_it->second->data->GetResponsePrototype(method).New()};
        task->rpc_ctrlr = std::make_unique<rpc_controller>();
        task->rpc_ctrlr->attach_pd(task->conn->sock->pd());
        task->rpc_ctrlr->attach_peer_address(task->conn->sock->peer_address());
        if (not is_parsed) {
            SPDK_ERRLOG("ERROR: Unserialize request body failed\n");
            enqueue_reply(std::move(task), status::bad_request_body);
            return;
        }

        auto* task_ptr = task.get();
        task->done = google::protobuf::NewCallback(this, &server::on_response, task_ptr);
        SPDK_DEBUGLOG(
          msg,
          "emplace task of server id %ld to _on_fligh_tasks, core id %d\n",
          task_ptr->server_id, ::spdk_env_get_current_core());
        _on_fligh_tasks.emplace(task_ptr->server_id, std::move(task));
        service_it->second->data->CallMethod(
          method,
          task_ptr->rpc_ctrlr.get(),
          task_ptr->request_body.get(),
          task_ptr->response_body.get(),
          task_ptr->done);
    }

    int handle_rpc_reply() {
        if (_reply_task_list.empty()) {
            return SPDK_POLLER_IDLE;
        }

        auto& task = _reply_task_list.front();
        if (not task->reply_status) {
            return SPDK_POLLER_IDLE;
        }

        SPDK_DEBUGLOG(msg, "task id is %lu, task->response_data addr is %p\n", task->id, task->response_data.get());
        if (not task->response_data or not task->response_data->prepared()) {
            ++rdma_probe.rpc_pool_exhausted;
            ++rdma_probe.rpc_complete_fail;
            SPDK_WARNLOG(
              "drop unprepared reply task %lu, data_free=%zu pool_exhausted=%lu\n",
              task->id,
              _data_pool ? _data_pool->size() : 0,
              rdma_probe.rpc_pool_exhausted);
            drop_rpc_task(std::move(task));
            _reply_task_list.pop_front();
            return SPDK_POLLER_BUSY;
        }

        auto err = send_reply(task.get());
        if (err) {
            if (err->err.value() == ENOMEM) {
                SPDK_NOTICELOG(
                  "Post the reply wr of request %lu return enomem\n",
                  task->id);
                return SPDK_POLLER_IDLE;
            }

            SPDK_ERRLOG(
             "ERROR: Send reply of task '%lu' error '%s'\n",
              task->id, err->err.message().c_str());
            close_connection(task->conn.get());

            return SPDK_POLLER_BUSY;
        }
        ++rdma_probe.rpc_complete_ok;
        SPDK_INFOLOG(msg, "send reply of rpc task %lu\n", task->id);
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

        auto& task = _read_task_list.front();
        if (is_timeout(task.get())) {
            handle_timeout_task(std::move(task));
            _read_task_list.pop_front();
            return SPDK_POLLER_BUSY;
        }

        if (not task->request_data->is_rdma_read_complete()) {
            return SPDK_POLLER_IDLE;
        }

        SPDK_DEBUGLOG(msg, "exec rpc of id %lu\n", task->id);
        auto req_meta = task->request_data->read_request_meta();
        dispatch_method(req_meta, std::move(task));
        _read_task_list.pop_front();

        return SPDK_POLLER_BUSY;
    }

    void maybe_dump_rpc_stats() {
        auto now = std::chrono::steady_clock::now();
        if (now < _next_stats_dump) {
            return;
        }
        _next_stats_dump = now + std::chrono::seconds{default_rpc_stats_dump_interval_s};
        const auto inflight =
          _task_list.size() + _read_task_list.size() +
          _on_fligh_tasks.size() + _reply_task_list.size();
        SPDK_NOTICELOG(
          "rpc stats server inflight=%zu data_inuse=%zu data_free=%zu data_wm=%lu "
          "meta_inuse=%zu meta_free=%zu meta_wm=%lu %s\n",
          inflight,
          _data_pool ? _data_pool->inuse() : 0,
          _data_pool ? _data_pool->size() : 0,
          _data_pool ? _data_pool->watermark_hits() : 0,
          _meta_pool ? _meta_pool->inuse() : 0,
          _meta_pool ? _meta_pool->size() : 0,
          _meta_pool ? _meta_pool->watermark_hits() : 0,
          rdma_probe.fmt_rpc().c_str());
    }

    int handle_rpc_task() {
        if (_is_terminated) {
            return SPDK_POLLER_IDLE;
        }

        maybe_dump_rpc_stats();
        process_timeout_scan();

        if (_task_list.empty()) {
            return SPDK_POLLER_IDLE;
        }

        auto& task = _task_list.front();
        if (not task->request_data->is_metadata_complete()) {
            SPDK_DEBUGLOG(msg, "rpc task %lu, metadata is not complete\n", task->id);
            return SPDK_POLLER_IDLE;
        }

        if (not task->request_data->prepared()) {
            SPDK_NOTICELOG(
              "rpc task %lu request transport not prepared, reply resource_exhausted\n",
              task->id);
            auto fail = std::move(task);
            _task_list.pop_front();
            enqueue_reply(std::move(fail), status::resource_exhausted);
            return SPDK_POLLER_BUSY;
        }

        SPDK_DEBUGLOG(msg, "post read wr of task %lu\n", task->id);
        auto rc = send_read_request(task->conn.get(), task->request_data.get());
        if (rc and rc->err.value() == ENOMEM) {
            SPDK_NOTICELOG(
              "Post the read wr of request %lu return enomem\n",
              task->id);
            return SPDK_POLLER_IDLE;
        }

        if (rc) {
            SPDK_ERRLOG(
              "ERROR: Post read request of rpc task '%lu' error, '%s'\n",
              task->id, rc->err.message().c_str());
            close_connection(task->conn.get());
        }
        _read_task_list.push_back(std::move(task));
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
        case ::IBV_WC_SEND: {
            auto max_wr = conn->sock->max_slient_wr();
            conn->onflight_send_wr -= max_wr;
            if (static_cast<uint32_t>(conn->onflight_send_wr) < max_wr) {
                conn->should_signal = false;
            }
            return false;
        }
        case ::IBV_WC_RECV: {
            auto it = conn->recv_ctx_map.find(cqe->wr_id);
            if (it == conn->recv_ctx_map.end()) {
                SPDK_DEBUGLOG(msg, "conn->recv_ctx_map.size(): %lu\n", conn->recv_ctx_map.size());
                SPDK_ERRLOG(
                  "ERROR: Cant find the receive context of wr id '%s'\n",
                  work_request_id::fmt(cqe->wr_id).c_str());
                close_connection(conn.get());
                return true;
            }

            auto recv_ctx = it->second;
            auto task_full = transport_data::read_correlation_index(recv_ctx);
            auto task_id = transport_data::correlation_index_lo(task_full);
            bool is_new_task = transport_data::is_new(recv_ctx);
            bool is_inlined = transport_data::is_inlined(recv_ctx);
            bool is_no_content = transport_data::is_no_content(recv_ctx);
            SPDK_DEBUGLOG(
              msg,
              "handle cqe of wr id %s on rdma cm id %p, is no content: %d, "
              "is new task: %d, is inlined: %d, serial_no: %d, metadata_count: %d\n",
              work_request_id::fmt(cqe->wr_id).c_str(), conn->sock->id(), is_no_content,
              is_new_task, is_inlined, transport_data::read_serial_no(recv_ctx),
              transport_data::read_metacount(recv_ctx));

            if (is_no_content) {
                task_id = ::be32toh(cqe->imm_data);
                SPDK_DEBUGLOG(msg, "free message cqe on task id %u\n", task_id);
                SPDK_INFOLOG(msg, "RPC task of id %u done\n", task_id);
                auto rc = post_recv(conn.get(), recv_ctx);
                if (rc) {
                    SPDK_ERRLOG("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                    close_connection(conn.get());
                }
                conn->rpc_tasks.erase(task_id);
                return true;
            }

            SPDK_DEBUGLOG(msg, "cqe on task id %lu\n", task_full);
            auto task_it = conn->rpc_tasks.find(task_id);

            if (is_new_task or is_inlined) {
                SPDK_DEBUGLOG(msg, "new rpc task with correlation index %lu\n", task_full);
                auto task = std::make_unique<rpc_task>(_task_id_gen++, task_full, conn);
                task->this_server = this; // FIXME: 这里 this 指向的 server 可能已经析构了，在访问时。

                if (_is_terminated) {
                    enqueue_reply(std::move(task), status::terminating);
                    return true;
                }

                if (is_inlined) {
                    auto* inlined_data = transport_data::read_inlined_content(recv_ctx);
                    auto meta = load_request_meta(inlined_data);
                    SPDK_DEBUGLOG(
                      msg,
                      "[%d] parsed rpc meta, task id is %lu, service name is %.*s, method name is %.*s\n",
                      ::spdk_env_get_current_core(),
                      task->id,
                      meta.service_name_size,
                      meta.service_name,
                      meta.method_name_size,
                      meta.method_name);
                    task->recv_ctx = recv_ctx;
                    conn->rpc_tasks.emplace(task_id, task.get());
                    dispatch_method(meta, std::move(task), true);

                    return true;
                }

                SPDK_DEBUGLOG(msg, "created request_data for task id %lu\n", task->id);
                task->request_data = std::make_unique<transport_data>(_data_pool);
                auto [it, _] = conn->rpc_tasks.emplace(task_id, task.get());
                task_it = it;
                _task_list.push_back(std::move(task));
            } else if (task_it == conn->rpc_tasks.end() or
                       task_it->second->id != task_full) {
                if (conn->done_keys.contains(task_full)) {
                    auto rc = post_recv(conn.get(), recv_ctx);
                    if (rc) {
                        SPDK_ERRLOG("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                        close_connection(conn.get());
                    }
                    return true;
                }
                ++rdma_probe.rpc_unknown_key;
                SPDK_INFOLOG(
                  msg,
                  "unknown rpc task id %lu, drop and keep QP, unknown_key=%lu\n",
                  task_full, rdma_probe.rpc_unknown_key);
                auto rc = post_recv(conn.get(), recv_ctx);
                if (rc) {
                    SPDK_ERRLOG("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                    close_connection(conn.get());
                }

                return true;
            }

            SPDK_DEBUGLOG(msg, "handle rpc task with id %u, _task_list size is %lu\n", task_id, _task_list.size());
            auto* task = task_it->second;
            task->request_data->from_net_context(recv_ctx);

            auto rc = post_recv(conn.get(), recv_ctx);
            if (rc) {
                SPDK_ERRLOG("ERROR: Post receive wr error, '%s'\n", rc->message().c_str());
                close_connection(conn.get());
            }

            return true;
        }
        default:
            return true;
        }
    }

    int handle_stop_timeout() {
        if (not is_all_tasks_done()) {
            if (_stop_ctx->timeout_at <= std::chrono::system_clock::now()) {
                return SPDK_POLLER_IDLE;
            }
        }

        _ib_cm_event_poller.unregister_poller();
        _cq_poller.unregister_poller();
        _cqe_poller.unregister_poller();
        _task_poller.unregister_poller();
        _task_read_poller.unregister_poller();
        _task_reply_poller.unregister_poller();
        _stop_poller.unregister_poller();
        SPDK_NOTICELOG("Pollers of rpc server have been unregistered\n");

        _meta_pool->free();
        _data_pool->free();

        if (_stop_ctx->on_stop_cb) {
            try {
                (_stop_ctx->on_stop_cb.value())();
            } catch (const std::exception& e) {
                SPDK_ERRLOG("Caught exception when executing callback on stopping rpc server: %s\n", e.what());
            }
        }

        // OSD hands in core_sharded::get_thread(shard_id). Exiting that
        // kills the shard reactor. Only exit a thread this server created.
        if (_owns_thread && _thread != nullptr &&
            _thread != ::spdk_thread_get_app_thread()) {
            ::spdk_thread_exit(_thread);
        } else {
            SPDK_NOTICELOG(
              "rpc server stop complete on borrowed thread %s; not calling spdk_thread_exit\n",
              _thread ? ::spdk_thread_get_name(_thread) : "<null>");
        }

        return SPDK_POLLER_IDLE;
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

    static void on_rpc_done(void* arg) {
        auto* task = reinterpret_cast<rpc_task*>(arg);
        task->this_server->handle_rpc_done(task);
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

    static int stop_timeout_poller(void* arg) {
        auto* this_server = reinterpret_cast<server*>(arg);
        return this_server->handle_stop_timeout();
    }

public:

    /*
     * =======================================================================
     * class callbacks
     * =======================================================================
     */

    void handle_start() {
        _ib_cm_event_poller.register_poller(ib_cm_event_poller, this, 0, "rpc_srv_ib_evt");
        _cq_poller.register_poller(cq_poller, this, 0, "rpc_srv_cq");
        _cqe_poller.register_poller(cqe_poller, this, 0, "rpc_srv_cqe");
        _task_poller.register_poller(task_poller, this, 0, "rpc_srv_task");
        _task_read_poller.register_poller(task_read_poller, this, 0, "rpc_srv_rdma_read");
        _task_reply_poller.register_poller(task_reply_poller, this, 0, "rpc_srv_reply");
        SPDK_INFOLOG(msg, "Rpc server started\n");
    }

    void handle_stop(std::unique_ptr<stop_context> ctx) {
        _stop_ctx = std::move(ctx);
        _stop_ctx->timeout_at = std::chrono::system_clock::now() + _opts->shutdown_timeout;
        _is_terminated = true;
        SPDK_NOTICELOG("Start stop the rpc server\n");
        _stop_poller.register_poller(stop_timeout_poller, this, 0, "rpc_srv_stop");
    }

    void handle_add_service(std::unique_ptr<add_service_ctx> ctx) {
        auto* p = ctx->service_ptr;
        auto descriptor = const_cast<google::protobuf::ServiceDescriptor*>(p->GetDescriptor());
        auto service_val = std::make_shared<service>(p, descriptor);
        _services.emplace(service_val->descriptor->name(), service_val);
    }

    void handle_rpc_done(rpc_task* task) {
        auto it = _on_fligh_tasks.find(task->server_id);
        if (it == _on_fligh_tasks.end()) {
            SPDK_ERRLOG(
              "ERROR: Cant find the task record of server id %ld, core id %d\n",
              task->server_id, ::spdk_env_get_current_core());
            return;
        }

        auto owned = std::move(it->second);
        const auto task_id = owned->id;
        _on_fligh_tasks.erase(it);
        if (owned->replied) {
            SPDK_NOTICELOG(
              "late complete of already-replied task %lu, drop\n", task_id);
            drop_rpc_task(std::move(owned));
            return;
        }
        owned->replied = true;
        if (owned->rpc_ctrlr->Failed()) {
            SPDK_ERRLOG("ERROR: Exec rpc failed: %s\n", owned->rpc_ctrlr->ErrorText().c_str());
            enqueue_reply(std::move(owned), status::server_error);
        } else {
            SPDK_INFOLOG(
              msg,
              "Making rpc response body of request id %lu\n",
              task_id);
            enqueue_reply(std::move(owned), status::success);
        }
        SPDK_DEBUGLOG(msg, "task id %lu rpc exec done\n", task_id);
    }

    int handle_ib_cm_event_poll() {
        _dev->process_ib_event();
        auto evt = _channel.poll();
        if (not evt) { return SPDK_POLLER_IDLE; }

        SPDK_INFOLOG(
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
             * The rdma_ack_cm_event(evt) call was moved into handle_other_cm_event()
             * Otherwise, calling rdma_destroy_id() without rdma_ack_cm_event() first causes deadlock
             */
            handle_other_cm_event(evt);
            break;
        }

        return SPDK_POLLER_BUSY;
    }

    int handle_cq_poll() {
        auto rc = _cq->poll(_wcs.get(), _opts->poll_cq_batch_size);
        if (rc < 0) {
            SPDK_ERRLOG("ERROR: Poll cq error '%s', stop the server\n", std::strerror(errno));
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
            SPDK_DEBUGLOG(msg, "polled cqe with opcode %s\n", completion_queue::op_name(cqe->opcode).c_str());
            if (cqe->opcode != ::IBV_WC_RECV) {
                continue;
            }

            dis_id = work_request_id::dispatch_id(cqe->wr_id);
            auto dispatch_it = _cqe_dispatch_map.find(dis_id);
            if (dispatch_it == _cqe_dispatch_map.end()) {
                SPDK_NOTICELOG(
                  "cant find the connection with dispatch id %ld, work request id %ld, cqe opcode is %s\n",
                  dis_id, cqe->wr_id, completion_queue::op_name(cqe->opcode).c_str());
                continue;
            }

            auto conn = dispatch_it->second;
            if (cqe->status != ::IBV_WC_SUCCESS) {
                sock = conn->sock.get();
                if (not sock->is_closed()) {
                    sock->update_qp_attr();
                    SPDK_ERRLOG(
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

    void create_listener(uint16_t port) {
        _opts->port = port;
        endpoint ep{_opts->bind_address, _opts->port};
        SPDK_INFOLOG(msg, "start listening on %s:%d\n", _opts->bind_address.c_str(), _opts->port);
        ep.passive = true;
        _listener = std::make_unique<socket>(ep, *_pd, _channel.value(), false);
    }
    void start() {
        auto ec = _listener->listen(_opts->listen_backlog);
        if (ec) {
            SPDK_ERRLOG(
              "listen on %s:%d error: %s\n",
              _opts->bind_address.c_str(),
              _opts->port,
              ec->message().c_str());
            throw std::runtime_error{"server listen error"};
        }
        SPDK_INFOLOG(msg, "Start listening...\n");
        auto rc = ::spdk_thread_send_msg(_thread, on_start, this);
        if (rc != 0) {
            SPDK_ERRLOG("ERROR: Send msg to thread error, %s\n", ::spdk_strerror(rc));
            throw std::runtime_error{"start rpc server failed"};
        }
    }

    void stop(std::optional<std::function<void()>>&& cb = std::nullopt) {
        if (_is_terminated) {
            return;
        }

        stop_context* ctx = new stop_context{this, std::move(cb)};
        auto rc = ::spdk_thread_send_msg(_thread, on_stop, ctx);
        if (rc != 0) {
            SPDK_ERRLOG("ERROR: Send msg to thread error, %s\n", ::spdk_strerror(rc));
            throw std::runtime_error{"stop rpc server failed"};
        }
    }

    void add_service(service::pointer p) {
        auto* ctx = new add_service_ctx{p, this};
        auto rc = ::spdk_thread_send_msg(_thread, on_add_service, ctx);
        if (rc != 0) {
            SPDK_ERRLOG("ERROR: Send msg to thread error, %s\n", ::spdk_strerror(rc));
            throw std::runtime_error{"add service failed"};
        }
    }

    std::string listen_address() noexcept {
        return _opts->bind_address;
    }

    uint16_t listen_port() noexcept {
        return _opts->port;
    }

public:

    static void process_ib_event(::ibv_context* context, ::ibv_async_event* event) {
        SPDK_DEBUGLOG(
          msg, "received ib event(%s) on %s\n",
          ::ibv_event_type_str(event->event_type),
          ::ibv_get_device_name(context->device));

        switch (event->event_type) {
        case ::IBV_EVENT_CQ_ERR:
            SPDK_ERRLOG(
              "ERROR: Received ib event(IBV_EVENT_QP_FATAL: %s) on %s, cq is %p, "
              "may the opts 'msg_rdma_cq_num_entries' is too small",
              ::ibv_event_type_str(event->event_type),
              ::ibv_get_device_name(context->device),
              event->element.cq);
            break;
        case ::IBV_EVENT_QP_FATAL:
            SPDK_ERRLOG(
              "ERROR: Received ib event(IBV_EVENT_QP_FATAL: %s) on %s, qp is %p\n",
              ::ibv_event_type_str(event->event_type),
              ::ibv_get_device_name(context->device),
              event->element.qp);
            break;
        case ::IBV_EVENT_QP_LAST_WQE_REACHED:
            // SRQ-only event, not used currently
            break;
        case ::IBV_EVENT_SQ_DRAINED: {
            // QP may or may not be in error state when this event is sent
            auto sock = reinterpret_cast<socket*>(
              event->element.qp->qp_context);
            auto state = sock->update_qp_attr();

            if (state == IBV_QPS_ERR) {
                SPDK_ERRLOG("qp error, will close connection\n");
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
            SPDK_ERRLOG(
              "received ib event(%s) on %s\n",
              ::ibv_event_type_str(event->event_type),
              ::ibv_get_device_name(context->device));
            break;
        }
    }

private:

    ::spdk_cpuset _cpumask{};
    ::spdk_thread* _thread{nullptr};
    bool _owns_thread{false};
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
    utils::simple_poller _stop_poller{};
    std::unordered_map<::rdma_cm_id*, std::unique_ptr<socket>> _cm_records{};
    connection_id::serial_type _serial{0};
    work_request_id::connection_id_type _cq_dispatch_id{1};
    std::unordered_map<::rdma_cm_id*, std::shared_ptr<connection_record>> _connections{};
    std::unordered_map<work_request_id::dispatch_id_type, std::shared_ptr<connection_record>> _cqe_dispatch_map{};
    std::unique_ptr<::ibv_wc[]> _wcs{nullptr};
    std::unordered_map<std::string_view, std::shared_ptr<service>> _services{};

    int _sock_id{SPDK_ENV_SOCKET_ID_ANY};
    std::shared_ptr<memory_pool<::ibv_send_wr>> _meta_pool{nullptr};
    std::shared_ptr<memory_pool<::ibv_send_wr>> _data_pool{nullptr};
    std::shared_ptr<memory_pool<::ibv_recv_wr>> _recv_pool{nullptr};
    std::list<std::unique_ptr<rpc_task>> _task_list{};
    std::list<std::unique_ptr<rpc_task>> _read_task_list{};
    std::unordered_map<rpc_task::server_id_type, std::unique_ptr<rpc_task>> _on_fligh_tasks{};
    std::list<std::unique_ptr<rpc_task>> _reply_task_list{};
    std::list<std::shared_ptr<connection_record>> _cqe_conn_list{};

    event_channel _channel{};
    rpc_task::server_id_type _task_id_gen{0};
    std::unique_ptr<stop_context> _stop_ctx{nullptr};
    std::chrono::steady_clock::time_point _next_stats_dump{
      std::chrono::steady_clock::now()};

public:
    probe rdma_probe{};
};

} // namespace rdma
} // namespace msg
