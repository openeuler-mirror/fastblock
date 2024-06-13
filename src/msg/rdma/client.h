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
#include "msg/rdma/memory_pool.h"
#include "msg/rdma/probe.h"
#include "msg/rdma/work_request_id.h"
#include "msg/rdma/socket.h"
#include "msg/rdma/transport_data.h"
#include "msg/rdma/types.h"
#include "utils/fmt.h"
#include "utils/utils.h"
#include "utils/log.h"
#include "utils/simple_poller.h"

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/thread.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <boost/property_tree/ptree.hpp>

#include <chrono>
#include <csignal>
#include <functional>
#include <optional>
#include <list>
#include <unordered_map>

#include <endian.h>

#define conf_or_client(json_conf, opts, opts_key) conf_or_s(json_conf, "msg_client_"#opts_key, opts, opts_key)

namespace msg {
namespace rdma {

class client : public std::enable_shared_from_this<client> {

public:

    void handle_connection_shutdown(::rdma_cm_id* cm_id, connection_id conn_id, work_request_id::dispatch_id_type dis_id) {
        SPDK_NOTICELOG_EX("erase connection with id %lu\n", conn_id.value());
        _cm_records.erase(cm_id);
        _connections.erase(conn_id);
        _cqe_dispatch_map.erase(dis_id);
    }

public:

    struct options {
        size_t poll_cq_batch_size{128};
        size_t metadata_memory_pool_capacity{4096};
        size_t metadata_memory_pool_element_size{1024};
        size_t data_memory_pool_capacity{4096};
        size_t data_memory_pool_element_size{8192};
        size_t per_post_recv_num{512};
        std::chrono::system_clock::duration rpc_timeout{std::chrono::seconds{30}};
        int64_t rpc_batch_size{1023};
        std::unique_ptr<endpoint> ep{nullptr};
        size_t connect_max_retry{0};
        std::chrono::system_clock::duration retry_interval{std::chrono::seconds{0}};
    };

    static std::shared_ptr<options> make_options(boost::property_tree::ptree& conf) {
        auto opts = std::make_shared<options>();
        conf_or_client(conf, opts, poll_cq_batch_size);
        conf_or_client(conf, opts, metadata_memory_pool_capacity);
        conf_or_client(conf, opts, metadata_memory_pool_element_size);
        conf_or_client(conf, opts, data_memory_pool_capacity);
        conf_or_client(conf, opts, data_memory_pool_element_size);
        conf_or_client(conf, opts, per_post_recv_num);
        conf_or_client(conf, opts, rpc_batch_size);
        conf_or_client(conf, opts, connect_max_retry);
        conf_or_client(conf, opts, connect_max_retry);

        if (conf.count("msg_client_rpc_timeout_us") != 0) {
            auto timeout_us = conf.get_child("msg_client_rpc_timeout_us").get_value<int64_t>();
            opts->rpc_timeout = std::chrono::milliseconds{timeout_us};
        }

        if (conf.count("msg_client_connect_retry_interval_us") != 0) {
            auto connect_retry_interval_us = conf.get_child("msg_client_connect_retry_interval_us").get_value<int64_t>();
            opts->retry_interval = std::chrono::milliseconds{connect_retry_interval_us};
        }

        opts->ep = std::make_unique<endpoint>(conf);

        return opts;
    }

    class connection : public std::enable_shared_from_this<connection>, public google::protobuf::RpcChannel {

    public:

        struct rpc_request {
            using key_type = transport_data::correlation_index_type;

            key_type request_key{};
            const google::protobuf::MethodDescriptor* method{nullptr};
            google::protobuf::RpcController* ctrlr{nullptr};
            const google::protobuf::Message* request{nullptr};
            google::protobuf::Message* response{nullptr};
            google::protobuf::Closure* closure{nullptr};
            std::unique_ptr<char[]> meta{nullptr};
            std::unique_ptr<transport_data> request_data{nullptr};
            std::unique_ptr<transport_data> reply_data{nullptr};
            std::chrono::system_clock::time_point start_at{std::chrono::system_clock::now()};
        };

    private:

        struct enqueue_request_context {
            std::unique_ptr<rpc_request> req{nullptr};
            connection* this_conn{nullptr};
        };

    public:

        connection() = delete;

        connection(
          const connection_id id,
          const work_request_id::connection_id_type dis_id,
          std::unique_ptr<socket> sock,
          std::shared_ptr<options> opts,
          std::shared_ptr<memory_pool<::ibv_send_wr>> meta_pool,
          std::shared_ptr<memory_pool<::ibv_send_wr>> data_pool,
          std::shared_ptr<std::list<std::weak_ptr<connection>>> busy_list,
          std::shared_ptr<std::list<std::weak_ptr<connection>>> busy_priority_list,
          std::shared_ptr<client> master)
          : _id{id}
          , _dispatch_id{dis_id}
          , _sock{std::move(sock)}
          , _opts{opts}
          , _meta_pool{meta_pool}
          , _data_pool{data_pool}
          , _recv_pool{nullptr}
          , _recv_ctx{nullptr}
          , _busy_list{busy_list}
          , _busy_priority_list{busy_priority_list}
          , _master{master} {}

        connection(const connection&) = delete;

        connection(connection&&) = delete;

        connection& operator=(const connection&) = delete;

        connection& operator=(connection&&) = delete;

        ~connection() noexcept = default;

    public:

        static void on_enqueue_request(void* arg) {
            auto* ctx = reinterpret_cast<enqueue_request_context*>(arg);
            ctx->this_conn->enqueue_request(std::move(ctx->req));
            ::spdk_free(ctx);
        }

    private:

        bool is_reach_max_send_wr(const size_t send_count) noexcept {
            return _onflight_send_wr + send_count >= _sock->max_slient_wr() - 1;
        }

        auto send_request(std::function<::ibv_send_wr*(bool)> make_wr, const size_t n_wrs) {
            // _should_signal = is_reach_max_send_wr(n_wrs);

            auto* head_wr = make_wr(true);
            // if (_should_signal) {
            //     _should_signal = false;
            //     _onflight_send_wr = n_wrs;
            // } else {
            //     _onflight_send_wr += n_wrs;
            // }

            // auto* head_wr = make_wr(true);
            auto err = _sock->send(head_wr);
            if (not err) {
                rdma_probe.send_wr_posted(n_wrs);
                // _onflight_send_wr += n_wrs;
            }

            return err;
        }

        auto send_finish_request(const uint32_t req_key) {
            auto wr = std::make_unique<::ibv_send_wr>();
            std::memset(wr.get(), 0, sizeof(decltype(wr)::element_type));
            wr->opcode = IBV_WR_SEND_WITH_IMM;
            wr->imm_data = ::htobe32(req_key);
            _dispatch_id.inc_request_id();
            wr->wr_id = _dispatch_id.value();

            SPDK_INFOLOG_EX(msg, "Post finish wr of request id %d\n", req_key);
            return send_request([this, wr = wr.get()] (bool should_signal) {
                if (should_signal) {
                    wr->send_flags |= IBV_SEND_SIGNALED;
                }
                return wr;
            }, 1);
        }

        auto send_metadata_request(transport_data* trans_data) {
            return send_request([this, trans_data] (bool should_signal) {
                return trans_data->make_send_request([this] () noexcept {
                    _dispatch_id.inc_request_id();
                    return _dispatch_id.value();
                }, should_signal);
            }, trans_data->meatdata_capacity());
        }

        auto send_read_request(transport_data* trans_data) {
            return send_request([this, trans_data] (bool should_signal) {
                return trans_data->make_read_request([this] () noexcept {
                    _dispatch_id.inc_request_id();
                    return _dispatch_id.value();
                }, should_signal);
            }, trans_data->data_capacity());
        }

        int process_request_once(std::unique_ptr<rpc_request>& req) {
            if (not req->request_data->is_ready()) {
                SPDK_DEBUGLOG_EX(
                  msg,
                  "not enough chunks for request %d, which needs %ld bytes\n",
                  req->request_key, req->request_data->serilaized_size());
                return -EAGAIN;
            }

            SPDK_DEBUGLOG_EX(
              msg,
              "request id is %d, request serialize size is %ld, request body size is %ld\n",
              req->request_key,
              req->request->ByteSizeLong() + request_meta_size,
              req->request->ByteSizeLong());

            auto* meta = reinterpret_cast<request_meta*>(req->meta.get());
            SPDK_DEBUGLOG_EX(
              msg,
              "service_name_size: %d, service_name: %s, method_name_size: %d, method_name: %s\n",
              meta->service_name_size, meta->service_name,
              meta->method_name_size, meta->method_name);

            req->request_data->serialize_data(
              req->meta.get(),
              request_meta_size,
              req->request);
            auto* req_ptr = req.get();

            SPDK_INFOLOG_EX(
              msg,
              "Send rpc request(id: %d, name: %s) with body size %ld, %s => %s\n",
              req_ptr->request_key,
              req_ptr->method->name().c_str(),
              req_ptr->request_data->serilaized_size(),
              _sock->local_address().c_str(),
              _sock->peer_address().c_str());

            auto err = send_metadata_request(req_ptr->request_data.get());
            if (err and err->value() == ENOMEM) {
                SPDK_NOTICELOG_EX(
                  "Post the metadata of request %d return enomem, onflight_send_wr: %d\n",
                  req_ptr->request_key, _onflight_send_wr);
                return -EAGAIN;
            }

            if (err) {
                SPDK_ERRLOG_EX(
                  "ERROR: Post send work request error '%s'\n",
                  err->message().c_str());
                shutdown();
                return err->value();
            }

            _unresponsed_requests.emplace(req_ptr->request_key, std::move(req));
            return 0;
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

        bool is_timeout(rpc_request* req) noexcept {
            return req->start_at + _opts->rpc_timeout < std::chrono::system_clock::now();
        }

    public:

#if defined(__arm__) || defined(__aarch64__)
#pragma GCC push_options
#pragma GCC optimize ("O0")
#endif
        void per_post_recv() {
            _recv_ctx = _recv_pool->get_bulk(_opts->per_post_recv_num);
            for (size_t i{0}; i < _opts->per_post_recv_num - 1; ++i) {
                _dispatch_id.inc_request_id();
                _recv_ctx[i]->wr.wr_id = _dispatch_id.value();
                _recv_ctx[i]->wr.next = &(_recv_ctx[i + 1]->wr);
                _recv_ctx_map.emplace(_recv_ctx[i]->wr.wr_id, _recv_ctx[i]);
            }
            _dispatch_id.inc_request_id();
            _recv_ctx[_opts->per_post_recv_num - 1]->wr.wr_id = _dispatch_id.value();
            _recv_ctx[_opts->per_post_recv_num - 1]->wr.next = nullptr;
            _recv_ctx_map.emplace(
              _recv_ctx[_opts->per_post_recv_num - 1]->wr.wr_id,
              _recv_ctx[_opts->per_post_recv_num - 1]);

            auto err = _sock->receive(&(_recv_ctx[0]->wr));
            if (err) {
                SPDK_ERRLOG_EX(
                  "ERROR: post %lu receive wrs error, '%s'\n",
                  _opts->per_post_recv_num,
                  err->message().c_str());
                shutdown();
            }
            rdma_probe.receive_wr_posted(_opts->per_post_recv_num);

            SPDK_DEBUGLOG_EX(msg, "post %ld receive wrs\n", _opts->per_post_recv_num);
        }
#if defined(__arm__) || defined(__aarch64__)
#pragma GCC pop_options
#endif

        void generate_id(const connection_id::serial_type serial_no) {
            _id.update(_sock->guid(), serial_no);
        }

        int handle_poll() {
            if (_is_terminated) {
                return SPDK_POLLER_IDLE;
            }

            bool is_busy{false};
            if (_onflight_rpc_task_size <= _opts->rpc_batch_size) {
                if (not _busy_priority_list->empty()) {
                    auto it = _busy_priority_list->begin();
                    if (it->expired()) {
                        SPDK_NOTICELOG_EX("Got the heartbeat rpc task on closed connection\n");
                        _busy_priority_list->pop_front();
                    } else {
                        is_busy = it->lock()->process_priority_rpc_request(it);
                    }
                }

                if (not _busy_list->empty()) {
                    auto it = _busy_list->begin();
                    if (it->expired()) {
                        SPDK_NOTICELOG_EX("Got the rpc task on closed connection\n");
                        _busy_list->pop_front();
                    } else {
                        is_busy |= it->lock()->process_rpc_request(it);
                    }
                }
            }

            is_busy |= process_read_complete();
            is_busy |= handle_cqe();
            is_busy |= process_free_server();

            return is_busy ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
        }

        static int connection_poller(void* arg) {
            auto* this_conn = reinterpret_cast<connection*>(arg);
            return this_conn->handle_poll();
        }

        void start() {
            _recv_pool = std::make_unique<memory_pool<::ibv_recv_wr>>(
              _sock->pd(), FMT_1("crv_%1%", utils::random_string(5)),
              _opts->per_post_recv_num,
              _opts->metadata_memory_pool_element_size, 0);
            _recv_ctx = std::make_unique<memory_pool<::ibv_recv_wr>::net_context*[]>(
              _opts->per_post_recv_num);
            _poller.register_poller(connection_poller, this, 0);
        }

        void enqueue_request(std::unique_ptr<rpc_request> req) {
            auto is_pri_req = is_priority_request(req->method);
            enqueue_request(std::move(req), is_pri_req);
        }

        virtual void CallMethod(
          const google::protobuf::MethodDescriptor* m,
          google::protobuf::RpcController* ctrlr,
          const google::protobuf::Message* request,
          google::protobuf::Message* response,
          google::protobuf::Closure* c) override {
            if (_is_terminated) {
                ctrlr->SetFailed("connection has been shutdown");
                c->Run();
                return;
            }

            auto& service_name = m->service()->name();
            if (service_name.size() > max_rpc_meta_string_size) {
                SPDK_ERRLOG_EX(
                  "ERROR: RPC service name's length(%ld) is beyond the max size(%d)\n",
                  service_name.size(), max_rpc_meta_string_size);
                ctrlr->SetFailed(FMT_1("service name too long, should less than or equal to %1%", max_rpc_meta_string_size));
                c->Run();
                return;
            }

            auto& method_name = m->name();
            if (method_name.size() > max_rpc_meta_string_size) {
                SPDK_ERRLOG_EX(
                  "ERROR: RPC method name's length(%ld) is beyond the max size(%d)\n",
                  method_name.size(), max_rpc_meta_string_size);
                ctrlr->SetFailed(FMT_1( "method name is too long, should less than or equal to %1%", max_rpc_meta_string_size));
                c->Run();
                return;
            }

            auto meta_holder = std::make_unique<char[]>(request_meta_size);
            auto* meta = reinterpret_cast<request_meta*>(meta_holder.get());
            meta->service_name_size =
              static_cast<request_meta::name_size_type>(service_name.size());
            service_name.copy(meta->service_name, meta->service_name_size);
            meta->method_name_size =
              static_cast<request_meta::name_size_type>(method_name.size());
            method_name.copy(meta->method_name, meta->method_name_size);
            meta->data_size = static_cast<request_meta::data_size_type>(request->ByteSizeLong());

            SPDK_DEBUGLOG_EX(
              msg,
              "service_name_size: %d, service_name: %s, method_name_size: %d, method_name: %s\n",
              meta->service_name_size, meta->service_name,
              meta->method_name_size, meta->method_name);

            auto serialized_size = request->ByteSizeLong() + request_meta_size;
            auto req_key = _unresponsed_request_key_gen++;
            auto request_data = std::make_unique<transport_data>(
              req_key, serialized_size, _meta_pool, _data_pool);
            auto req = std::make_unique<rpc_request>(
              req_key, m, ctrlr, request, response, c, std::move(meta_holder), std::move(request_data));

            SPDK_DEBUGLOG_EX(
              msg,
              "transport data_size: %d, serialized_size: %ld, req_key: %d, service: %s, method: %s\n",
              meta->data_size, serialized_size, req_key, service_name.c_str(), method_name.c_str());

            SPDK_INFOLOG_EX(
              msg,
              "Enqueued rpc task(%s::%s) %d\n",
              meta->service_name,
              meta->method_name,
              req->request_key);

            auto* ctx = (enqueue_request_context*)::spdk_zmalloc(
              sizeof(enqueue_request_context),
              0, nullptr, SPDK_ENV_LCORE_ID_ANY,
              SPDK_MALLOC_DMA);
            ctx->req = std::move(req);
            ctx->this_conn = this;
            ::spdk_thread_send_msg(_master->get_thread(), on_enqueue_request, ctx);
        }

        bool process_rpc_request(std::list<std::weak_ptr<connection>>::iterator busy_it) {
            if (_onflight_requests.empty()) {
                return false;
            }

            auto it = _onflight_requests.begin();
            auto* task_ptr = it->get();
            auto rc = process_request_once(*it);

            if (rc == -EAGAIN) {
                return false;
            }

            ++_onflight_rpc_task_size;
            SPDK_DEBUGLOG_EX(
              msg,
              "_onflight_rpc_task_size: %ld, rpc queue depth: %ld, rc is %d\n",
              _onflight_rpc_task_size,
              _priority_onflight_requests.size() + _onflight_requests.size(), rc);

            auto* stack_ptr = it->get();
            if (rc == -EINVAL) {
                SPDK_ERRLOG_EX(
                  "ERROR: Timeout occured of rpc request key %d\n",
                  stack_ptr->request_key);
                stack_ptr->ctrlr->SetFailed("timeout");
                stack_ptr->closure->Run();
            } else if (rc != 0) {
                SPDK_ERRLOG_EX("ERROR: Sending rpc request failed, rc is %d\n", rc);
                stack_ptr->ctrlr->SetFailed(FMT_1("error, rc %1%", rc));
                stack_ptr->closure->Run();
            }

            _onflight_requests.erase(it);
            _busy_list->erase(busy_it);
            return true;
        }

        bool process_priority_rpc_request(std::list<std::weak_ptr<connection>>::iterator busy_it) {
            if (_priority_onflight_requests.empty()) {
                return false;
            }

            SPDK_DEBUGLOG_EX(
              msg,
              "process %ld rpc call heartbeat()\n",
              _priority_onflight_requests.size());

            int rc{0};
            auto erase_it_end = _priority_onflight_requests.begin();
            rpc_request* stack_ptr;
            for (auto it = _priority_onflight_requests.begin(); it != _priority_onflight_requests.end(); ++it) {
                auto* task_ptr = it->get();
                rc = process_request_once(*it);
                if (rc == -EAGAIN) {
                    erase_it_end = it;
                    break;
                }

                ++_onflight_rpc_task_size;
                SPDK_DEBUGLOG_EX(
                  msg,
                  "_onflight_rpc_task_size: %ld, rpc queue depth: %ld\n",
                  _onflight_rpc_task_size,
                  _priority_onflight_requests.size() + _onflight_requests.size());

                stack_ptr = it->get();
                if (rc == -EINVAL) {
                    stack_ptr->ctrlr->SetFailed("timeout");
                    stack_ptr->closure->Run();
                } else if (rc != 0) {
                    SPDK_ERRLOG_EX("ERROR: Sending rpc request failed, rc is %d\n", rc);
                    stack_ptr->ctrlr->SetFailed(FMT_1("error, rc %1%", rc));
                    stack_ptr->closure->Run();
                }

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

        bool process_free_server() {
            if (_free_server_list.empty()) {
                return false;
            }

            auto head = _free_server_list.front();
            auto err = send_finish_request(head);
            if (err) {
                if (err->value() == ENOMEM) {
                    SPDK_NOTICELOG_EX(
                      "Post free wr of request %u return enomem, onflight_send_wr: %d\n",
                      head, _onflight_send_wr);
                    return false;
                }

                SPDK_ERRLOG_EX(
                  "ERROR: Post send work request error '%s'\n",
                  err->message().c_str());
                shutdown();
            }

            _free_server_list.pop_front();
            return true;
        }

        bool process_read_complete() {
            if (_wait_read_requests.empty()) {
                return false;
            }

            bool is_parsed{false};
            auto* stack_ptr = _wait_read_requests.begin()->get();
            if (is_timeout(stack_ptr)) {
                SPDK_ERRLOG_EX(
                  "Timeout occurred on rpc request of request key %d\n",
                  stack_ptr->request_key);
                stack_ptr->ctrlr->SetFailed("timeout");
                goto read_done;
            }

            if (not stack_ptr->reply_data->is_rdma_read_complete()) {
                return false;
            }

            SPDK_INFOLOG_EX(
              msg,
              "Read the response body of request %d\n",
              stack_ptr->request_key);

            SPDK_DEBUGLOG_EX(msg, "_onflight_rpc_task_size: %ld\n", _onflight_rpc_task_size);
            is_parsed = stack_ptr->reply_data->unserialize_data(stack_ptr->response, reply_meta_size);
            if (not is_parsed) {
                SPDK_ERRLOG_EX(
                  "ERROR: Unserialize the response body of request key %d failed\n",
                  stack_ptr->request_key);
                stack_ptr->ctrlr->SetFailed("unserialize error");
            }

read_done:
            _free_server_list.push_back(stack_ptr->request_key);
            --_onflight_rpc_task_size;
            stack_ptr->closure->Run();
            stack_ptr->reply_data.reset(nullptr);
            _wait_read_requests.pop_front();

            return true;
        }

        bool handle_cqe() {
            if (cqe_list.empty()) {
                return false;
            }

            auto cqe = std::move(cqe_list.front());
            cqe_list.pop_front();

            switch (cqe->opcode) {
            case ::IBV_WC_RECV: {
                SPDK_DEBUGLOG_EX(msg, "handle cqe of wr id '%ld'\n", cqe->wr_id);
                auto it = _recv_ctx_map.find(cqe->wr_id);
                if (it == _recv_ctx_map.end()) {
                    SPDK_ERRLOG_EX("ERROR: Cant find the receive context of wr id '%ld'\n", cqe->wr_id);
                    shutdown();
                    return true;
                }

                auto* recv_ctx = it->second;
                auto req_key = transport_data::read_correlation_index(recv_ctx);
                auto req_it = _unresponsed_requests.find(req_key);
                if (req_it == _unresponsed_requests.end()) {
                    SPDK_ERRLOG_EX("ERROR: Cant find the request stack of key '%d'\n", req_key);
                    shutdown();
                    return true;
                }

                if (is_timeout(req_it->second.get())) {
                    SPDK_ERRLOG_EX(
                      "Timeout occurred on rpc request of request key %d\n",
                      req_it->second->request_key);
                    req_it->second->ctrlr->SetFailed("timeout");
                    _free_server_list.push_back(req_it->second->request_key);
                    --_onflight_rpc_task_size;
                    SPDK_DEBUGLOG_EX(msg, "_onflight_rpc_task_size: %ld\n", _onflight_rpc_task_size);
                    _unresponsed_requests.erase(req_it);
                    break;
                }

                req_it->second->request_data.reset(nullptr);
                auto reply_m = transport_data::read_reply_meta(it->second);
                SPDK_INFOLOG_EX(
                  msg,
                  "Received reply of request %d, status is %s\n",
                  req_it->second->request_key,
                  string_status(reply_m));

                SPDK_DEBUGLOG_EX(
                  msg,
                  "received reply of request %d, status is %s, elapsed: %ldus\n",
                  req_it->second->request_key,
                  string_status(reply_m),
                  (std::chrono::system_clock::now() - req_it->second->start_at).count() / 1000);

                switch (reply_m) {
                case status::no_content: {
                    req_it->second->closure->Run();
                    _free_server_list.push_back(req_it->second->request_key);

                    --_onflight_rpc_task_size;
                    SPDK_DEBUGLOG_EX(msg, "_onflight_rpc_task_size: %ld\n", _onflight_rpc_task_size);
                    _unresponsed_requests.erase(req_it);
                    break;
                }
                case status::success: {
                    auto* response = req_it->second->response;
                    bool is_parsed{false};
                    if (transport_data::is_inlined(it->second)) {
                        auto* inlined_data = transport_data::read_inlined_content(it->second);
                        auto unserialize_size = transport_data::read_inlined_content_length(it->second);
                        SPDK_DEBUGLOG_EX(msg, "unserilaize inlined reply, size is %d\n", unserialize_size);
                        is_parsed = response->ParseFromArray(
                          inlined_data + reply_meta_size,
                          transport_data::read_inlined_content_length(it->second));
                    } else {
                        if (not req_it->second->reply_data) {
                            req_it->second->reply_data = std::make_unique<transport_data>(_data_pool);
                        }

                        req_it->second->reply_data->from_net_context(it->second);
                        if (not req_it->second->reply_data->is_metadata_complete()) {
                            SPDK_DEBUGLOG_EX(
                              msg,
                              "request key %d, metadata is not complete\n",
                              req_it->second->request_key);
                            break;
                        }

                        if (not req_it->second->reply_data->is_ready()) {
                            SPDK_DEBUGLOG_EX(
                              msg,
                              "request key %d, transport data is not ready\n",
                              req_it->second->request_key);
                            break;
                        }

                        auto err = send_read_request(req_it->second->reply_data.get());
                        if (err) {
                            if (err->value() == ENOMEM) {
                                SPDK_NOTICELOG_EX(
                                  "nomem for post read wr of request %d, on_flight_send_wr: %d\n",
                                  req_it->first, _onflight_send_wr);
                                return false;
                            }

                            SPDK_ERRLOG_EX(
                              "ERROR: Post send work request error '%s'\n",
                              err->message().c_str());
                            shutdown();
                            return true;
                        }

                        SPDK_DEBUGLOG_EX(
                          msg,
                          "request key of %d start rdma reading, _wait_read_requests addr is %p\n",
                          req_it->second->request_key,
                          &_wait_read_requests);
                        _wait_read_requests.push_back(std::move(req_it->second));
                        _unresponsed_requests.erase(req_it);

                        break;
                    }

                    --_onflight_rpc_task_size;
                    SPDK_DEBUGLOG_EX(msg, "_onflight_rpc_task_size: %ld\n", _onflight_rpc_task_size);
                    if (not is_parsed) {
                        SPDK_ERRLOG_EX(
                          "ERROR: Parse response body failed of request %d\n",
                          req_it->second->request_key);

                        req_it->second->ctrlr->SetFailed("unserialize failed");
                        req_it->second->closure->Run();
                        _free_server_list.push_back(req_it->second->request_key);
                        _unresponsed_requests.erase(req_it);
                        break;
                    }

                    SPDK_INFOLOG_EX(
                      msg,
                      "Read the inlined response body of request %d\n",
                      req_it->second->request_key);
                    req_it->second->closure->Run();
                    _free_server_list.push_back(req_it->second->request_key);
                    _unresponsed_requests.erase(req_it);
                    break;
                }
                default: {
                    SPDK_ERRLOG_EX(
                      "ERROR: RPC call failed of request %d with reply status %s\n",
                      req_it->second->request_key,
                      string_status(reply_m));
                    req_it->second->ctrlr->SetFailed(
                      FMT_1("rpc call failed with reply status %1%", string_status(reply_m)));
                    _free_server_list.push_back(req_it->second->request_key);
                    _unresponsed_requests.erase(req_it);
                    break;
                }
                }

                _dispatch_id.inc_request_id();
                it->second->wr.wr_id = _dispatch_id.value();
                it->second->wr.next = nullptr;
                _sock->receive(&(it->second->wr));
                rdma_probe.receive_wr_posted();
                _recv_ctx_map.emplace(it->second->wr.wr_id, it->second);
                _recv_ctx_map.erase(it);
            }
            default:
                break;
            }

            return true;
        }

        void shutdown_slient() {
            if (_is_terminated) {
                return;
            }

            _is_terminated = true;
            _poller.unregister_poller();
            SPDK_NOTICELOG_EX("The poller of the connection has been unregistered\n");

            for (auto& task : _onflight_requests) {
                task->ctrlr->SetFailed("connection has been shutdown");
                task->closure->Run();
            }
            _onflight_requests.clear();

            for (auto& task : _priority_onflight_requests) {
                task->ctrlr->SetFailed("connection has been shutdown");
                task->closure->Run();
            }
            _priority_onflight_requests.clear();

            for (auto& task : _wait_read_requests) {
                task->ctrlr->SetFailed("connection has been shutdown");
                task->closure->Run();
            }
            _wait_read_requests.clear();

            for (auto& task_pair : _unresponsed_requests) {
                task_pair.second->ctrlr->SetFailed("connection has been shutdown");
                task_pair.second->closure->Run();
            }
            _unresponsed_requests.clear();
            _sock->close();

            if (_recv_ctx) {
                _recv_pool->put_bulk(std::move(_recv_ctx), _opts->per_post_recv_num);
            }
            _recv_pool->free();
        }

        void shutdown() {
            shutdown_slient();
            _master->handle_connection_shutdown(_sock->id(), _id, _dispatch_id.dispatch_id());
        }

    public:

        socket& fd() noexcept {
            return *_sock;
        }

        connection_id id() noexcept {
            return _id;
        }

        auto dispatch_id() noexcept {
            return _dispatch_id.dispatch_id();
        }

        bool is_terminated() noexcept {
            return _is_terminated;
        }

    private:

        bool _is_terminated{false};
        connection_id _id{};
        work_request_id _dispatch_id;
        std::unique_ptr<socket> _sock{nullptr};
        std::shared_ptr<options> _opts{nullptr};

        std::shared_ptr<memory_pool<::ibv_send_wr>> _meta_pool{nullptr};
        std::shared_ptr<memory_pool<::ibv_send_wr>> _data_pool{nullptr};
        std::unique_ptr<memory_pool<::ibv_recv_wr>> _recv_pool{nullptr};
        std::unique_ptr<memory_pool<::ibv_recv_wr>::net_context*[]> _recv_ctx{nullptr};
        std::unordered_map<work_request_id::value_type, memory_pool<::ibv_recv_wr>::net_context*> _recv_ctx_map{};

        rpc_request::key_type _unresponsed_request_key_gen{0};
        std::list<std::unique_ptr<rpc_request>> _onflight_requests{};
        std::list<std::unique_ptr<rpc_request>> _priority_onflight_requests{};
        std::list<rpc_request::key_type> _free_server_list{};
        std::unordered_map<rpc_request::key_type, std::unique_ptr<rpc_request>> _unresponsed_requests{};
        std::list<std::unique_ptr<rpc_request>> _wait_read_requests{};

        std::shared_ptr<std::list<std::weak_ptr<connection>>> _busy_list{};
        std::shared_ptr<std::list<std::weak_ptr<connection>>> _busy_priority_list{};
        std::shared_ptr<client> _master{};
        utils::simple_poller _poller{};

        // bool _should_signal{false};
        int32_t _onflight_send_wr{0};
        int64_t _onflight_rpc_task_size{0};

    public:

        probe rdma_probe{};
        std::list<std::unique_ptr<::ibv_wc>> cqe_list{};
    };

private:

    struct start_context {
        client* this_client{nullptr};
        std::optional<std::function<void()>> on_start_cb{std::nullopt};
    };

    struct stop_context {
        client* this_client{nullptr};
        std::optional<std::function<void()>> on_stop_cb{std::nullopt};
    };

    struct emplace_connection_context {
        client* this_client{nullptr};
        std::string addr{};
        uint16_t port{};
        std::function<void(bool, std::shared_ptr<connection>)> cb{};
    };

    struct remove_connection_context {
        std::function<void(bool)> cb{};
        std::shared_ptr<connection> conn;
    };

    enum class connect_state {
        wait_address_resolved = 1,
        wait_route_resolved,
        wait_connect_established,
        connect_failed
    };

    struct reconnect_context {
        std::chrono::system_clock::time_point connect_fail_at{std::chrono::system_clock::now()};
        size_t retry_times{0};
    };

    struct connect_task {
        std::shared_ptr<connection> conn{};
        std::function<void(bool, std::shared_ptr<connection>)> cb{};
        connect_state conn_state{connect_state::wait_address_resolved};
        std::unique_ptr<reconnect_context> retry_ctx{nullptr};
    };

public:

    client() = delete;

    client(std::string name, ::spdk_cpuset* cpumask, std::shared_ptr<options> opts)
      : _opts{opts}
      , _dev{std::make_shared<device>()}
      , _pd{std::make_unique<protection_domain>(_dev, _opts->ep->device_name)}
      , _cq{std::make_shared<completion_queue>(_opts->ep->cq_num_entries, *_pd)}
      , _wcs{std::make_unique<::ibv_wc[]>(_opts->poll_cq_batch_size)}
      , _thread{::spdk_thread_create(name.c_str(), cpumask)}
      , _meta_pool{std::make_shared<memory_pool<::ibv_send_wr>>(
        _pd->value(), FMT_1("%1%_m", name),
        _opts->metadata_memory_pool_capacity,
        _opts->metadata_memory_pool_element_size, 0)}
      , _data_pool{std::make_shared<memory_pool<::ibv_send_wr>>(
        _pd->value(), FMT_1("%1%_d", name),
        _opts->data_memory_pool_capacity,
        _opts->data_memory_pool_element_size, 0)} {}

    client(const client&) = delete;

    client(client&&) = delete;

    client& operator=(const client&) = delete;

    client& operator=(client&&) = delete;

    ~client() noexcept {
        _core_poller.unregister_poller();
    }

public:

    /*
     * =======================================================================
     * spdk callbacks
     * =======================================================================
     */

    static void on_start(void* arg) {
        auto* ctx = reinterpret_cast<start_context*>(arg);
        ctx->this_client->handle_start(std::unique_ptr<start_context>{ctx});
    }

    static void on_stop(void* arg) {
        auto* ctx = reinterpret_cast<stop_context*>(arg);
        ctx->this_client->handle_stop(std::unique_ptr<stop_context>{ctx});
    }

    static void on_emplace_connection(void* arg) {
        auto* ctx = reinterpret_cast<emplace_connection_context*>(arg);
        ctx->this_client->handle_emplace_eonnection(
          std::unique_ptr<emplace_connection_context>(ctx));
    }

    static void on_remove_connection(void* arg){
        auto* ctx = reinterpret_cast<remove_connection_context*>(arg);
        ctx->conn->shutdown();
        ctx->cb(true);
        delete ctx;
    }

    static int core_poller(void* arg) {
        auto* this_client = reinterpret_cast<client*>(arg);
        return this_client->handle_core_poll();
    }

public:

    /*
     * =======================================================================
     * class callbacks
     * =======================================================================
     */

    void handle_start(std::unique_ptr<start_context> ctx) {
        if (_is_started) { return; }
        _is_started = true;
        _core_poller.register_poller(core_poller, this, 0);

        if (ctx->on_start_cb) {
            ctx->on_start_cb.value()();
        }
        SPDK_INFOLOG_EX(msg, "Rpc client started\n");
    }

    void handle_stop(std::unique_ptr<stop_context> ctx) {
        if (_is_terminated) {
            return;
        }

        _is_terminated = true;
        _core_poller.unregister_poller();
        SPDK_NOTICELOG_EX("The poller of the rpc client has been unregistered\n");

        for (auto& conn_pair : _connections) {
            conn_pair.second->shutdown_slient();
        }

        if (_thread) {
            auto current_thread = spdk_get_thread();
            ::spdk_set_thread(_thread);
            ::spdk_thread_exit(_thread);
            if (current_thread == _thread) {
                ::spdk_set_thread(nullptr);
            } else {
                ::spdk_set_thread(current_thread);
            }

            SPDK_NOTICELOG_EX("SPDK thread of the rpc client has been marked as exited\n");
        }

        _meta_pool->free();
        _data_pool->free();

        if (ctx and ctx->on_stop_cb) {
            try {
                (ctx->on_stop_cb.value())();
            } catch (const std::exception& e) {
                SPDK_ERRLOG_EX("Caught exception when executing callback on stopping rpc client: %s\n", e.what());
            }
        }
    }

    void handle_emplace_eonnection(std::unique_ptr<emplace_connection_context> ctx) {
        endpoint ep = *(_opts->ep);
        ep.addr = ctx->addr;
        ep.port = ctx->port;
        ep.passive = false;
        auto sock = std::make_unique<socket>(ep, *_pd, _channel.value());
        auto* cm_id = sock->id();
        auto conn = std::make_shared<connection>(
          connection_id{},
          _cq_dispatch_id++,
          std::move(sock),
          _opts,
          _meta_pool,
          _data_pool,
          _busy_connections,
          _busy_priority_connections,
          shared_from_this());
        auto conn_task = std::make_unique<connect_task>(conn, std::move(ctx->cb));
        _connect_tasks.emplace(cm_id, std::move(conn_task));
    }

    void process_connect_retry() {
        if (_connect_tasks.empty()) {
            return;
        }

        auto conn_task_pair = _connect_tasks.begin();
        for (; conn_task_pair != _connect_tasks.end(); ++conn_task_pair) {
            if (not conn_task_pair->second) {
                continue;
            }

            auto* task_ptr = conn_task_pair->second.get();
            if (task_ptr->conn_state != connect_state::connect_failed) {
                continue;
            }

            if (not task_ptr->retry_ctx) {
                task_ptr->retry_ctx = std::make_unique<reconnect_context>();
                continue;
            }

            if (task_ptr->retry_ctx->connect_fail_at + _opts->retry_interval > std::chrono::system_clock::now()) {
                continue;
            }

            task_ptr->retry_ctx->retry_times++;
            task_ptr->retry_ctx->connect_fail_at = std::chrono::system_clock::now();
            if (task_ptr->retry_ctx->retry_times > _opts->connect_max_retry) {
                SPDK_ERRLOG_EX(
                  "ERROR: Trying reconnting to %s exceed max times %lu\n",
                  task_ptr->conn->fd().peer_address().c_str(),
                  task_ptr->retry_ctx->retry_times);
                task_ptr->cb(false, nullptr);
                _connect_tasks.erase(task_ptr->conn->fd().id());
                break;
            }

            SPDK_NOTICELOG_EX(
              "Try to connect %s at the %lu times\n",
              task_ptr->conn->fd().peer_address().c_str(),
              task_ptr->retry_ctx->retry_times);

            auto old_fd_id = task_ptr->conn->fd().id();
            auto old_conn_id = task_ptr->conn->id();
            auto sock = std::make_unique<socket>(task_ptr->conn->fd().get_endpoint(), *_pd, _channel.value());
            auto new_fd_id = sock->id();
            task_ptr->conn = std::make_shared<connection>(
              connection_id{},
              _cq_dispatch_id++,
              std::move(sock),
              _opts,
              _meta_pool,
              _data_pool,
              _busy_connections,
              _busy_priority_connections,
              shared_from_this());
            // 不要调整这里的 erase 和 emplace 的顺序
            _cm_records.emplace(new_fd_id, task_ptr->conn);
            _cm_records.erase(old_fd_id);
            _connections.erase(old_conn_id);
            _connections.emplace(task_ptr->conn->id(), task_ptr->conn);
            task_ptr->conn_state = connect_state::wait_address_resolved;
            SPDK_DEBUGLOG_EX(
              msg,
              "erased old cm id %p, emplaced new cm id %p; "
              "erased old conn id %ld, emplaced new conn id %ld\n",
              old_fd_id, task_ptr->conn->fd().id(),
              old_conn_id, task_ptr->conn->id());
            auto conn_task = std::move(conn_task_pair->second);
            _connect_tasks.emplace(new_fd_id, std::move(conn_task));
        }
        std::erase_if(_connect_tasks, [] (const auto& pair) {
            return not pair.second;
        });
    }

    void process_connect_task(::rdma_cm_event* evt) {
        auto task_it = _connect_tasks.find(evt->id);
        if (task_it == _connect_tasks.end()) {
            SPDK_ERRLOG_EX(
              "ERROR: Cant find the connect task of cm id %p on event %s in connect tasks map\n",
              evt->id, ::rdma_event_str(evt->event));

            auto cm_rd_it = _cm_records.find(evt->id);
            if (cm_rd_it != _cm_records.end()) {
                SPDK_ERRLOG_EX(
                  "ERROR: cm id %p should not occured in cm records map on event %s\n",
                  evt->id, ::rdma_event_str(evt->event));
                std::raise(SIGINT);
            }

            return;
        }

        auto evt_val = evt->event;
        auto* task_ptr = task_it->second.get();
        auto& fd = task_it->second->conn->fd();
        switch (task_ptr->conn_state) {
        case connect_state::wait_address_resolved: {
            auto ret = fd.is_resolve_address_done(evt);
            if (ret == 0) {
                fd.resolve_route();
                task_ptr->conn_state = connect_state::wait_route_resolved;
            } else {
                SPDK_ERRLOG_EX(
                  "ERROR: resolve route failed when connecting %s\n",
                  fd.peer_address().c_str());
                task_ptr->conn_state = connect_state::connect_failed;
            }
            break;
        }
        case connect_state::wait_route_resolved: {
            auto ret = fd.is_resolve_route_done(evt);
            if (ret == 0) {
                fd.create_qp(*_pd, _cq->cq());
                auto rc = fd.start_connect();
                if (rc) {
                    SPDK_ERRLOG_EX(
                      "ERROR: Connect to %s error, '%s'\n",
                      fd.peer_address().c_str(),
                      rc->message().c_str());
                    task_ptr->conn_state = connect_state::connect_failed;
                } else {
                    task_ptr->conn_state = connect_state::wait_connect_established;
                }
            } else {
                SPDK_ERRLOG_EX(
                  "ERROR: resolve route filaed when connecting %s\n",
                  fd.peer_address().c_str());
                task_ptr->conn_state = connect_state::connect_failed;
            }
            break;
        }
        case connect_state::wait_connect_established: {
            auto ret = fd.is_established(evt);
            if (ret == 0) {
                task_ptr->conn->generate_id(_serial++);
                _cm_records.emplace(fd.id(), task_ptr->conn);
                _connections.emplace(task_ptr->conn->id(), task_ptr->conn);
                _cqe_dispatch_map.emplace(task_ptr->conn->dispatch_id(), task_ptr->conn);
                task_ptr->conn->start();
                task_ptr->conn->per_post_recv();
                SPDK_INFOLOG_EX(msg, "Connected to %s\n", task_ptr->conn->fd().peer_address().c_str());
                task_ptr->cb(true, task_ptr->conn);
                _connect_tasks.erase(fd.id());
            } else {
                SPDK_ERRLOG_EX(
                  "ERROR: Wait 'RDMA_CM_EVENT_ESTABLISHED' failed when connecting %s\n",
                  fd.peer_address().c_str());
                task_ptr->conn_state = connect_state::connect_failed;
            }
            break;
        }
        default:
            break;
        }
    }

    void handle_cm_event() {
        process_connect_retry();

        auto* evt = _channel.poll();
        if (not evt) { return; }

        SPDK_INFOLOG_EX(
          msg,
          "receive rdma cm event: %s, cm id: %p\n",
          ::rdma_event_str(evt->event), evt->id);

        if (_connect_tasks.contains(evt->id)) {
            process_connect_task(evt);
            return;
        }

        auto it = _cm_records.find(evt->id);
        if (it == _cm_records.end()) {
            SPDK_ERRLOG_EX(
              "ERROR: Received cm event %s, but the cm id can not be found in cm observers\n",
              ::rdma_event_str(evt->event));

            ::rdma_ack_cm_event(evt);
            return;
        }

        auto conn_it = _connections.find(it->second->id());
        if (conn_it == _connections.end()) {
            SPDK_ERRLOG_EX(
              "ERROR: Received cm event %s, but the connection id(%ld) can not be found in connection map\n",
              ::rdma_event_str(evt->event), it->second->id());
            _cm_records.erase(it);
            ::rdma_ack_cm_event(evt);

            return;
        }

        SPDK_INFOLOG_EX(
          msg,
          "Received rdma cm event '%s' on rdma cm id %p\n",
          ::rdma_event_str(evt->event),
          conn_it->second->fd().id());

        try {
            conn_it->second->fd().process_event_directly(evt);
            ::rdma_ack_cm_event(evt);
            evt = nullptr;
        } catch (...) {
            auto evt_id = evt->id;
            ::rdma_ack_cm_event(evt);
            evt = nullptr;

            SPDK_ERRLOG_EX(
              "ERROR: failed process cm event, will close the connection(rdma cm id: %p)\n",
              evt_id);

            SPDK_NOTICELOG_EX("Reomve the connection(id: %p)\n", evt_id);
            conn_it->second->shutdown();
        }

        if (evt) {
            ::rdma_ack_cm_event(evt);
        }
    }

    int handle_core_poll() {
        if (_is_terminated) {
            return SPDK_POLLER_IDLE;
        }

        handle_cm_event();

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
        std::shared_ptr<connection> conn{nullptr};
        work_request_id::dispatch_id_type dis_id;
        decltype(_cqe_dispatch_map)::iterator dispatch_it{};
        std::optional<std::error_code> errc{};

        for (int i{0}; i < rc; ++i) {
            cqe = &(_wcs[i]);
            SPDK_DEBUGLOG_EX(
              msg,
              "polled cqe with opcode %s, rc is %d, status is %s, wr_id is %s\n",
              completion_queue::op_name(cqe->opcode).c_str(), rc,
              socket::wc_status_name(cqe->status).c_str(),
              work_request_id::fmt(cqe->wr_id).c_str());
            dis_id = work_request_id::dispatch_id(cqe->wr_id);
            auto dispatch_it = _cqe_dispatch_map.find(dis_id);
            if (dispatch_it == _cqe_dispatch_map.end()) {
                SPDK_NOTICELOG_EX(
                  "cant find the connection with dispatch id %ld, work request id %ld, cqe opcode is %s\n",
                  dis_id, cqe->wr_id, completion_queue::op_name(cqe->opcode).c_str());
                continue;
            }

            conn = dispatch_it->second;
            switch (cqe->opcode) {
            case ::IBV_WC_RECV:
                break;
            default:
                continue;
            }

            if (cqe->status != ::IBV_WC_SUCCESS) {
                if (not conn->is_terminated()) {
                    conn->fd().update_qp_attr();
                    SPDK_ERRLOG_EX(
                      "ERROR: Got error wc, wc.vendor_err=%d, wc.status=%s, qp.state=%s\n",
                      cqe->vendor_err,
                      socket::wc_status_name(cqe->status).c_str(),
                      conn->fd().qp_state_str().c_str());
                    conn->shutdown();
                }

                continue;
            }

            auto cpy_cqe = std::make_unique<::ibv_wc>(_wcs[i]);
            conn->cqe_list.push_back(std::move(cpy_cqe));
        }

        return SPDK_POLLER_BUSY;
    }

public:

    /*
     * =======================================================================
     * public methods
     * =======================================================================
     */

    void start(std::optional<std::function<void()>> on_start_cb = std::nullopt) {
        SPDK_DEBUGLOG_EX(msg, "sending rpc client start message\n");
        auto* ctx = new start_context{this, std::move(on_start_cb)};
        ::spdk_thread_send_msg(_thread, on_start, ctx);
    }

    void stop(std::optional<std::function<void()>>&& on_stop_cb = std::nullopt) {
        auto* ctx = new stop_context{this, std::move(on_stop_cb)};
        ::spdk_thread_send_msg(_thread, on_stop, ctx);
    }

    void emplace_connection(
      std::string addr, uint16_t port,
      std::function<void(bool, std::shared_ptr<connection>)> cb) {
        auto* ctx = new emplace_connection_context{
          this, addr, port, std::move(cb)};
        ::spdk_thread_send_msg(_thread, on_emplace_connection, ctx);
    }

    void remove_connection(std::shared_ptr<msg::rdma::client::connection> conn,
      std::function<void(bool)> cb){
        auto* ctx = new remove_connection_context{std::move(cb), conn};
        ::spdk_thread_send_msg(_thread, on_remove_connection, ctx);
    }

    ::spdk_thread* get_thread() noexcept {
        return _thread;
    }

    bool is_start() noexcept {
        return _is_started;
    }

private:

    bool _is_started{false};
    bool _is_terminated{false};
    std::shared_ptr<options> _opts{nullptr};
    connection_id::serial_type _serial{0};
    work_request_id::connection_id_type _cq_dispatch_id{1};
    std::shared_ptr<device> _dev{nullptr};
    std::unique_ptr<protection_domain> _pd{nullptr};
    std::shared_ptr<completion_queue> _cq{nullptr};
    utils::simple_poller _core_poller{};
    std::unique_ptr<::ibv_wc[]> _wcs{nullptr};
    ::spdk_thread* _thread{nullptr};

    std::unordered_map<work_request_id::dispatch_id_type, std::shared_ptr<connection>> _cqe_dispatch_map{};
    std::unordered_map<connection_id, std::shared_ptr<connection>> _connections{};
    std::unordered_map<::rdma_cm_id*, std::shared_ptr<connection>> _cm_records{};
    std::unordered_map<::rdma_cm_id*, std::unique_ptr<connect_task>> _connect_tasks{};
    std::shared_ptr<memory_pool<::ibv_send_wr>> _meta_pool{nullptr};
    std::shared_ptr<memory_pool<::ibv_send_wr>> _data_pool{nullptr};

    std::shared_ptr<std::list<std::weak_ptr<connection>>> _busy_connections{
      std::make_shared<std::list<std::weak_ptr<connection>>>()};
    std::shared_ptr<std::list<std::weak_ptr<connection>>> _busy_priority_connections{
      std::make_shared<std::list<std::weak_ptr<connection>>>()};

    event_channel _channel{};
};

} // namespace rdma
} // namespace msg
