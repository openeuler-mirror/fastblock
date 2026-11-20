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

#include "fastblock/msg/rdma/connection_id.h"
#include "fastblock/msg/rdma/cq.h"
#include "fastblock/msg/rdma/memory_pool.h"
#include "fastblock/msg/rdma/probe.h"
#include "fastblock/msg/rdma/work_request_id.h"
#include "fastblock/msg/rdma/socket.h"
#include "fastblock/msg/rdma/completed_key_ring.h"
#include "fastblock/msg/rdma/oob_payload.h"
#include "fastblock/msg/rdma/transport_data.h"
#include "fastblock/msg/rdma/types.h"
#include "fastblock/msg/rpc_controller.h"
#include "fastblock/utils/duration_map.h"
#include "fastblock/utils/fmt.h"
#include "fastblock/utils/utils.h"

#include "fastblock/utils/simple_poller.h"

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/thread.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include <boost/property_tree/ptree.hpp>

#include <chrono>
#include <cstdint>
#include <csignal>
#include <exception>
#include <functional>
#include <optional>
#include <list>
#include <unordered_map>

#include <endian.h>

#define fb_rpc_client_key(k) "msg_client_"#k
#define conf_or_client(json_conf, opts, opts_key) conf_or_s(json_conf, fb_rpc_client_key(opts_key), opts, opts_key)
#define fb_rpc_client_printv(k, v, os) os << fb_rpc_client_key(k) << ": " << v << "\n"
#define fb_rpc_client_print_suf(k, v, suf, os) os << fb_rpc_client_key(k) << ": " << v << suf << "\n"
#define fb_rpc_client_print(k, o, os) fb_rpc_client_printv(k, o.k, os)

namespace msg {
namespace rdma {

class client : public std::enable_shared_from_this<client> {

public:

    struct options {
        size_t poll_cq_batch_size{128};
        size_t metadata_memory_pool_capacity{1024};
        size_t metadata_memory_pool_element_size{1024};
        size_t data_memory_pool_capacity{1024};
        size_t data_memory_pool_element_size{8192};
        size_t per_post_recv_num{512};
        size_t pool_low_watermark{0};
        size_t pool_high_watermark{0};
        size_t reserved_per_connection{0};
        size_t reserved_priority{0};
        std::chrono::system_clock::duration rpc_timeout{
          std::chrono::microseconds{default_rpc_timeout_us}};
        int64_t rpc_batch_size{1024};
        std::unique_ptr<endpoint> ep{nullptr};
        size_t connect_max_retry{10};
        std::chrono::system_clock::duration retry_interval{std::chrono::seconds{0}};
        std::chrono::system_clock::duration shutdown_timeout{std::chrono::microseconds{0}};
        std::chrono::system_clock::duration slow_rpc_warn{std::chrono::seconds{1}};

        friend std::ostream& operator<<(std::ostream& os, const options& o) {
            os << "[client]\n";
            fb_rpc_client_print(poll_cq_batch_size, o, os);
            fb_rpc_client_print(metadata_memory_pool_capacity, o, os);
            fb_rpc_client_print(metadata_memory_pool_element_size, o, os);
            fb_rpc_client_print(data_memory_pool_capacity, o, os);
            fb_rpc_client_print(data_memory_pool_element_size, o, os);
            fb_rpc_client_print(per_post_recv_num, o, os);
            fb_rpc_client_print(pool_low_watermark, o, os);
            fb_rpc_client_print(pool_high_watermark, o, os);
            fb_rpc_client_print(reserved_per_connection, o, os);
            fb_rpc_client_print(reserved_priority, o, os);
            fb_rpc_client_print(rpc_batch_size, o, os);
            fb_rpc_client_print(connect_max_retry, o, os);

            auto rpc_timeout_interval = std::chrono::duration_cast<std::chrono::microseconds>(o.rpc_timeout).count();
            fb_rpc_client_print_suf(rpc_timeout_us, rpc_timeout_interval, "us", os);

            auto retry_us_count = std::chrono::duration_cast<std::chrono::microseconds>(o.retry_interval).count();
            fb_rpc_client_print_suf(connect_retry_interval_us, retry_us_count, "us", os);

            auto shutdown_us = std::chrono::duration_cast<std::chrono::microseconds>(o.shutdown_timeout).count();
            fb_rpc_client_print_suf(shutdown_timeout_us, shutdown_us, "us", os);

            auto slow_rpc_warn_us = std::chrono::duration_cast<std::chrono::microseconds>(o.slow_rpc_warn).count();
            fb_rpc_client_print_suf(slow_rpc_warn_us, slow_rpc_warn_us, "us", os);

            os << *(o.ep);

            return os;
        }
    };

    static std::shared_ptr<options> make_options(boost::property_tree::ptree& conf) {
        auto opts = std::make_shared<options>();
        conf_or_client(conf, opts, poll_cq_batch_size);
        conf_or_client(conf, opts, metadata_memory_pool_capacity);
        conf_or_client(conf, opts, metadata_memory_pool_element_size);
        conf_or_client(conf, opts, data_memory_pool_capacity);
        conf_or_client(conf, opts, data_memory_pool_element_size);
        conf_or_client(conf, opts, per_post_recv_num);
        conf_or_client(conf, opts, pool_low_watermark);
        conf_or_client(conf, opts, pool_high_watermark);
        conf_or_client(conf, opts, reserved_per_connection);
        conf_or_client(conf, opts, reserved_priority);
        conf_or_client(conf, opts, rpc_batch_size);
        conf_or_client(conf, opts, connect_max_retry);

        if (conf.count("msg_client_rpc_timeout_us") != 0) {
            auto timeout_us = conf.get_child("msg_client_rpc_timeout_us").get_value<int64_t>();
            opts->rpc_timeout = std::chrono::microseconds{timeout_us};
        }

        if (conf.count("msg_client_connect_retry_interval_us") != 0) {
            auto connect_retry_interval_us = conf.get_child("msg_client_connect_retry_interval_us").get_value<int64_t>();
            opts->retry_interval = std::chrono::microseconds{connect_retry_interval_us};
        }

        if (conf.count("msg_client_shutdown_timeout_us") != 0) {
            auto shutdown_timeout_us = conf.get_child("msg_client_shutdown_timeout_us").get_value<int64_t>();
            opts->shutdown_timeout = std::chrono::microseconds{shutdown_timeout_us};
        }

        if (conf.count("msg_client_slow_rpc_warn_us") != 0) {
            auto slow_rpc_warn_us = conf.get_child("msg_client_slow_rpc_warn_us").get_value<int64_t>();
            opts->slow_rpc_warn = std::chrono::microseconds{slow_rpc_warn_us};
        }

        opts->ep = std::make_unique<endpoint>(conf);

        return opts;
    }

public:

    void handle_connection_shutdown(::rdma_cm_id* cm_id, connection_id conn_id, work_request_id::dispatch_id_type dis_id) {
        SPDK_INFOLOG(msg, "erase connection with id %lu\n", conn_id.value());
        _cm_records.erase(cm_id);
        _connections.erase(conn_id);
        _cqe_dispatch_map.erase(dis_id);
    }

    options* get_options() noexcept { return _opts.get(); }

public:

    class connection : public std::enable_shared_from_this<connection>, public google::protobuf::RpcChannel {

    public:

        enum class rpc_fail : uint8_t {
            ok = 0,
            resource_exhausted,
            timeout,
            request_timeout,
            too_large,
            connection_dead,
            unserialize,
            other
        };

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
            std::string payload{};
            std::chrono::system_clock::time_point start_at{std::chrono::system_clock::now()};
            bool completed{false};
        };

        struct quarantined_mr {
            memory_pool<::ibv_send_wr>::net_context* ctx{nullptr};
            std::chrono::system_clock::time_point expire_at{};
        };

        enum class state {
            running,
            peer_terminate,
            bad_event,
            shutdown,
            termianted
        };

        struct control_operation {
            state change_state{state::running};
        };

        struct shutdown_context {
            connection* this_conn{nullptr};
            state shutdown_state{};
        };

    private:

        struct enqueue_request_context {
            std::unique_ptr<rpc_request> req{nullptr};
            connection* this_conn{nullptr};
        };

        struct external_send_context {
            std::function<void()> on_complete{};
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
          std::shared_ptr<client> master,
          int sock_id)
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
          , _master{master}
          , _poller{_master->get_thread()}
          , _sock_id{sock_id} {}

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

        void set_failed_msg(google::protobuf::RpcController* ctrlr) {
            switch (_state) {
            case state::peer_terminate:
                fail_rpc(ctrlr, "peer terminating", rpc_error_class::peer_terminating);
                break;
            case state::shutdown:
                fail_rpc(ctrlr, "connection terminated", rpc_error_class::connection);
                break;
            case state::bad_event:
                fail_rpc(ctrlr, "bad ib/cm event", rpc_error_class::connection);
                break;
            default:
                break;
            }
        }

        void process_terminating_request(rpc_request* stack_ptr) {
            set_failed_msg(stack_ptr->ctrlr);
            _free_server_list.push_back(stack_ptr->request_key);
        }

        auto send_request(std::function<::ibv_send_wr*(bool)> make_wr, const size_t n_wrs) {
            ::ibv_send_wr* head_wr{nullptr};
            if (_opts->ep->qp_sig_all) {
                head_wr = make_wr(true);
            } else {
                _should_signal = is_reach_max_send_wr(n_wrs);

                head_wr = make_wr(_should_signal);
                _onflight_send_wr += n_wrs;

            }

            return _sock->send(head_wr);
        }

        auto send_finish_request(const rpc_request::key_type req_key) {
            auto wr = std::make_unique<::ibv_send_wr>();
            std::memset(wr.get(), 0, sizeof(decltype(wr)::element_type));
            wr->opcode = IBV_WR_SEND_WITH_IMM;
            wr->imm_data = ::htobe32(transport_data::correlation_index_lo(req_key));
            _dispatch_id.inc_request_id();
            wr->wr_id = _dispatch_id.value();

            SPDK_INFOLOG(msg, "Post finish wr of request id %lu\n", req_key);
            return send_request([this, wr = wr.get()] (bool should_signal) {
                if (should_signal) {
                    wr->send_flags |= IBV_SEND_SIGNALED;
                }
                return wr;
            }, 1);
        }

        auto send_metadata_request(transport_data* trans_data) {
            auto ret = send_request([this, trans_data] (bool should_signal) {
                return trans_data->make_send_request([this] () noexcept {
                    _dispatch_id.inc_request_id();
                    return _dispatch_id.value();
                }, should_signal);
            }, trans_data->meatdata_capacity());

            if (ret) { trans_data->mark_bad_send(ret->bad_wr); }

            return ret;
        }

        auto send_read_request(transport_data* trans_data) {
            auto ret = send_request([this, trans_data] (bool should_signal) {
                return trans_data->make_read_request([this] () noexcept {
                    _dispatch_id.inc_request_id();
                    return _dispatch_id.value();
                }, should_signal);
            }, trans_data->data_capacity());

            if (ret) { trans_data->mark_bad_read(ret->bad_wr); }

            return ret;
        }

        int process_request_once(std::unique_ptr<rpc_request>& req) {
            if (_state != state::running) { return -ENOLINK; }

            if (not req->request_data or not req->request_data->prepared()) {
                SPDK_NOTICELOG(
                  "request %lu is not prepared, fail-soft without enqueue retry\n",
                  req->request_key);
                return -ENOMEM;
            }

            SPDK_DEBUGLOG(
              msg,
              "request id is %lu, request serialize size is %ld, request body size is %ld\n",
              req->request_key,
              req->request->ByteSizeLong() + request_meta_size,
              req->request->ByteSizeLong());

            auto meta = load_request_meta(req->meta.get());
            SPDK_DEBUGLOG(
              msg,
              "service_name_size: %d, service_name: %s, method_name_size: %d, method_name: %s\n",
              meta.service_name_size, meta.service_name,
              meta.method_name_size, meta.method_name);

            req->request_data->serialize_data(
              req->meta.get(),
              request_meta_size,
              req->request,
              req->payload);
            // Restore bytes data so a later write-ring retry still has payload.
            if (!req->payload.empty()) {
                attach_rpc_payload(
                  *const_cast<google::protobuf::Message*>(req->request),
                  req->payload);
            }
            auto* req_ptr = req.get();

            SPDK_INFOLOG(
              msg,
              "Send rpc request(id: %lu, name: %s) with body size %ld, %s => %s\n",
              req_ptr->request_key,
              req_ptr->method->name().c_str(),
              req_ptr->request_data->serilaized_size(),
              _sock->local_address().c_str(),
              _sock->peer_address().c_str());

            auto err = send_metadata_request(req_ptr->request_data.get());
            if (err and err->err.value() == ENOMEM) {
                SPDK_NOTICELOG(
                  "Post the metadata of request %lu return enomem, onflight_send_wr: %d\n",
                  req_ptr->request_key, _onflight_send_wr);
                return -EAGAIN;
            }

            if (err) {
                SPDK_ERRLOG(
                  "ERROR: Post send work request error '%s'\n",
                  err->err.message().c_str());
                _ctrl_ops.push_back(std::make_unique<control_operation>(state::shutdown));
                return err->err.value();
            }

            _unresponsed_requests.emplace(req_ptr->request_key, std::move(req));
            return 0;
        }

        bool is_priority_request(const google::protobuf::MethodDescriptor* m) {
            return m->name() == "heartbeat";
        }

        rpc_request::key_type next_request_key() {
            for (int n{0}; n < 8; ++n) {
                const auto key = transport_data::make_correlation_index(_key_gen, _key_idx);
                if (++_key_idx == 0) {
                    ++_key_gen;
                    _key_idx = 1;
                }
                if (not _done_keys.contains(key)) {
                    return key;
                }
            }
            return transport_data::make_correlation_index(_key_gen, _key_idx);
        }

        void enqueue_request(std::unique_ptr<rpc_request> req, bool priority_req) {
            FASTBLOCK_DUR_MAP_EMPLACE_END(_enqueue_dur_map, req->request_key);
            req->request_data = std::make_unique<transport_data>(
              req->request_key,
              req->request->ByteSizeLong() + request_meta_size +
                req->payload.size(),
              _meta_pool, _data_pool,
              priority_req ? alloc_class::priority : alloc_class::reserved);
            if (not req->request_data->prepared()) {
                if (req->request_data->too_large()) {
                    complete_rpc(req.get(), rpc_fail::too_large);
                } else {
                    complete_rpc(req.get(), rpc_fail::resource_exhausted);
                }
                return;
            }
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

        void quarantine_transport(std::unique_ptr<transport_data>& td) {
            if (not td) {
                return;
            }
            auto expire_at = std::chrono::system_clock::now() + _opts->rpc_timeout;
            auto ctxs = td->release_data_contexts();
            for (auto* ctx : ctxs) {
                if (ctx) {
                    _mr_quarantine.push_back(quarantined_mr{ctx, expire_at});
                }
            }
            td.reset();
        }

        void quarantine_request(rpc_request* req) {
            if (not req) {
                return;
            }
            quarantine_transport(req->request_data);
            quarantine_transport(req->reply_data);
        }

        void reclaim_quarantine(bool force) {
            auto now = std::chrono::system_clock::now();
            for (auto it = _mr_quarantine.begin(); it != _mr_quarantine.end(); ) {
                if (force or now >= it->expire_at) {
                    if (_data_pool and it->ctx) {
                        _data_pool->put(it->ctx);
                    }
                    it = _mr_quarantine.erase(it);
                } else {
                    ++it;
                }
            }
        }

        void complete_rpc(rpc_request* req, rpc_fail why, const char* detail = nullptr) {
            if (not req or req->completed) {
                if (req and req->completed) {
                    ++rdma_probe.rpc_closure_double;
                    SPDK_INFOLOG(
                      msg, "rpc %lu complete_rpc double, why=%d double=%lu\n",
                      req->request_key, static_cast<int>(why),
                      rdma_probe.rpc_closure_double);
                }
                return;
            }
            req->completed = true;
            _done_keys.remember(req->request_key);

            switch (why) {
            case rpc_fail::ok:
                break;
            case rpc_fail::resource_exhausted:
                ++rdma_probe.rpc_pool_exhausted;
                fail_rpc(req->ctrlr, "resource_exhausted",
                         rpc_error_class::resource_exhausted);
                SPDK_WARNLOG(
                  "rpc %lu resource_exhausted method=%s data_free=%zu data_inuse=%zu pool_exhausted=%lu\n",
                  req->request_key,
                  req->method ? req->method->name().c_str() : "?",
                  _data_pool ? _data_pool->size() : 0,
                  _data_pool ? _data_pool->inuse() : 0,
                  rdma_probe.rpc_pool_exhausted);
                break;
            case rpc_fail::timeout:
                ++rdma_probe.rpc_timeout;
                fail_rpc(req->ctrlr, "timeout", rpc_error_class::timeout);
                SPDK_NOTICELOG(
                  "rpc %lu timeout, rpc_timeout=%lu\n",
                  req->request_key, rdma_probe.rpc_timeout);
                break;
            case rpc_fail::request_timeout:
                ++rdma_probe.rpc_timeout;
                fail_rpc(req->ctrlr, "request_timeout",
                         rpc_error_class::request_timeout);
                SPDK_NOTICELOG(
                  "rpc %lu request_timeout, rpc_timeout=%lu\n",
                  req->request_key, rdma_probe.rpc_timeout);
                break;
            case rpc_fail::too_large:
                ++rdma_probe.rpc_pool_too_large;
                fail_rpc(req->ctrlr, "too_large", rpc_error_class::too_large);
                SPDK_ERRLOG("rpc %lu too_large for memory pool\n", req->request_key);
                break;
            case rpc_fail::connection_dead:
                set_failed_msg(req->ctrlr);
                if (not req->ctrlr->Failed()) {
                    fail_rpc(req->ctrlr, "connection terminated",
                             rpc_error_class::connection);
                }
                break;
            case rpc_fail::unserialize:
                fail_rpc(req->ctrlr, detail ? detail : "unserialize error",
                         rpc_error_class::application);
                break;
            case rpc_fail::other:
                fail_rpc(
                  req->ctrlr,
                  detail ? detail : "rpc error",
                  detail ? classify_rpc_error(detail) : rpc_error_class::application);
                break;
            }

            if (why == rpc_fail::ok) {
                ++rdma_probe.rpc_complete_ok;
            } else {
                ++rdma_probe.rpc_complete_fail;
            }

            if (req->closure) {
                req->closure->Run();
                req->closure = nullptr;
            }
        }

        void drop_from_busy_list(
          std::shared_ptr<std::list<std::weak_ptr<connection>>> lst) {
            if (not lst) {
                return;
            }
            lst->remove_if([this](const std::weak_ptr<connection>& w) {
                auto p = w.lock();
                return not p or p.get() == this;
            });
        }

        bool process_timeout_scan() {
            bool busy{false};

            auto expire_queued = [this, &busy](std::list<std::unique_ptr<rpc_request>>& lst) {
                for (auto it = lst.begin(); it != lst.end(); ) {
                    if (is_timeout(it->get())) {
                        complete_rpc(it->get(), rpc_fail::timeout);
                        it = lst.erase(it);
                        busy = true;
                    } else {
                        ++it;
                    }
                }
            };

            auto expire_posted = [this, &busy](std::list<std::unique_ptr<rpc_request>>& lst) {
                for (auto it = lst.begin(); it != lst.end(); ) {
                    if (is_timeout(it->get())) {
                        quarantine_request(it->get());
                        --_onflight_rpc_task_size;
                        _free_server_list.push_back((*it)->request_key);
                        complete_rpc(it->get(), rpc_fail::timeout);
                        it = lst.erase(it);
                        busy = true;
                    } else {
                        ++it;
                    }
                }
            };

            expire_queued(_onflight_requests);
            expire_queued(_priority_onflight_requests);
            expire_posted(_send_read_requests);
            expire_posted(_wait_read_requests);

            for (auto it = _unresponsed_requests.begin(); it != _unresponsed_requests.end(); ) {
                if (is_timeout(it->second.get())) {
                    quarantine_request(it->second.get());
                    --_onflight_rpc_task_size;
                    _free_server_list.push_back(it->second->request_key);
                    complete_rpc(it->second.get(), rpc_fail::timeout);
                    it = _unresponsed_requests.erase(it);
                    busy = true;
                } else {
                    ++it;
                }
            }

            if (_onflight_requests.empty()) {
                drop_from_busy_list(_busy_list);
            }
            if (_priority_onflight_requests.empty()) {
                drop_from_busy_list(_busy_priority_list);
            }

            reclaim_quarantine(false);
            return busy;
        }

        void post_recv_ctx(memory_pool<::ibv_recv_wr>::net_context* recv_ctx,
                           work_request_id::value_type old_wr_id) {
            _dispatch_id.inc_request_id();
            recv_ctx->wr.wr_id = _dispatch_id.value();
            recv_ctx->wr.next = nullptr;
            _sock->receive(&(recv_ctx->wr));
            rdma_probe.receive_wr_posted();
            _recv_ctx_map.emplace(recv_ctx->wr.wr_id, recv_ctx);
            _recv_ctx_map.erase(old_wr_id);
        }

    public:

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
                SPDK_ERRLOG(
                  "ERROR: post %lu receive wrs error, '%s'\n",
                  _opts->per_post_recv_num,
                  err->message().c_str());
                _ctrl_ops.push_back(std::make_unique<control_operation>(state::shutdown));
            }
            rdma_probe.receive_wr_posted(_opts->per_post_recv_num);

            SPDK_DEBUGLOG(msg, "post %ld receive wrs\n", _opts->per_post_recv_num);
        }

        void generate_id(const connection_id::serial_type serial_no) {
            _id.update(_sock->guid(), serial_no);
        }

        int handle_poll() {
            if (_state == state::termianted) {
                return SPDK_POLLER_IDLE;
            }

            maybe_dump_rpc_stats();

            auto is_busy = process_control_op();
            auto is_shutdown = process_shutdown();
            if (is_shutdown) {
                return SPDK_POLLER_IDLE;
            }

            if (_onflight_rpc_task_size <= _opts->rpc_batch_size) {
                if (not _busy_priority_list->empty()) {
                    auto it = _busy_priority_list->begin();
                    if (it->expired()) {
                        SPDK_NOTICELOG("Got the heartbeat rpc task on closed connection\n");
                        _busy_priority_list->pop_front();
                    } else {
                        is_busy = it->lock()->process_priority_rpc_request(it);
                    }
                }

                if (not _busy_list->empty()) {
                    auto it = _busy_list->begin();
                    if (it->expired()) {
                        SPDK_NOTICELOG("Got the rpc task on closed connection\n");
                        _busy_list->pop_front();
                    } else {
                        is_busy |= it->lock()->process_rpc_request(it);
                    }
                }
            }

            is_busy |= process_timeout_scan();
            is_busy |= process_send_read_wr();
            is_busy |= process_read_complete();
            is_busy |= handle_cqe();
            is_busy |= process_free_server();

            return is_busy ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
        }

        void maybe_dump_rpc_stats() {
            auto now = std::chrono::steady_clock::now();
            if (now < _next_stats_dump) {
                return;
            }
            _next_stats_dump = now + std::chrono::seconds{default_rpc_stats_dump_interval_s};
            SPDK_NOTICELOG(
              "rpc stats peer=%s inflight=%ld data_inuse=%zu data_free=%zu data_wm=%lu "
              "meta_inuse=%zu meta_free=%zu meta_wm=%lu %s %s\n",
              fd().peer_address().c_str(),
              _onflight_rpc_task_size,
              _data_pool ? _data_pool->inuse() : 0,
              _data_pool ? _data_pool->size() : 0,
              _data_pool ? _data_pool->watermark_hits() : 0,
              _meta_pool ? _meta_pool->inuse() : 0,
              _meta_pool ? _meta_pool->size() : 0,
              _meta_pool ? _meta_pool->watermark_hits() : 0,
              rdma_probe.fmt_rpc().c_str(),
              rdma_probe.fmt().c_str());
        }

        static int connection_poller(void* arg) {
            auto* this_conn = reinterpret_cast<connection*>(arg);
            return this_conn->handle_poll();
        }

        void start() {
            _recv_pool = std::make_unique<memory_pool<::ibv_recv_wr>>(
              _sock->pd(), FB_FMT_1("crv_%1%", utils::random_string(5)),
              _opts->per_post_recv_num,
              _opts->metadata_memory_pool_element_size, 0, _sock_id);
            _recv_ctx = std::make_unique<memory_pool<::ibv_recv_wr>::net_context*[]>(
              _opts->per_post_recv_num);
            if (_meta_pool) {
                _meta_pool->add_connection();
            }
            if (_data_pool) {
                _data_pool->add_connection();
            }
            _quota_held = true;
            _poller.register_poller(connection_poller, this, 0, "rpc_cli_conn");
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
            if (_state != state::running) {
                set_failed_msg(ctrlr);
                c->Run();
                return;
            }

            auto& service_name = m->service()->name();
            if (service_name.size() > max_rpc_meta_string_size) {
                SPDK_ERRLOG(
                  "ERROR: RPC service name's length(%ld) is beyond the max size(%d)\n",
                  service_name.size(), max_rpc_meta_string_size);
                fail_rpc(
                  ctrlr,
                  FB_FMT_1("service name too long, should less than or equal to %1%", max_rpc_meta_string_size),
                  rpc_error_class::application);
                c->Run();
                return;
            }

            auto& method_name = m->name();
            if (method_name.size() > max_rpc_meta_string_size) {
                SPDK_ERRLOG(
                  "ERROR: RPC method name's length(%ld) is beyond the max size(%d)\n",
                  method_name.size(), max_rpc_meta_string_size);
                fail_rpc(
                  ctrlr,
                  FB_FMT_1( "method name is too long, should less than or equal to %1%", max_rpc_meta_string_size),
                  rpc_error_class::application);
                c->Run();
                return;
            }

            auto req_key = next_request_key();
            FASTBLOCK_DUR_MAP_EMPLACE_START(_enqueue_dur_map, req_key);
            std::string payload;
            peel_rpc_payload(*const_cast<google::protobuf::Message*>(request),
                             &payload);
            auto meta_holder = std::make_unique<char[]>(request_meta_size);
            auto meta = make_request_meta(
              service_name,
              method_name,
              static_cast<request_meta::data_size_type>(request->ByteSizeLong()),
              static_cast<request_meta::data_size_type>(payload.size()));
            store_request_meta(meta_holder.get(), meta);

            SPDK_DEBUGLOG(
              msg,
              "service_name_size: %d, service_name: %s, method_name_size: %d, method_name: %s\n",
              meta.service_name_size, meta.service_name,
              meta.method_name_size, meta.method_name);

            auto req = std::make_unique<rpc_request>(
              req_key, m, ctrlr, request, response, c, std::move(meta_holder));
            req->payload = std::move(payload);

            SPDK_DEBUGLOG(
              msg,
              "transport data_size: %d, serialized_size: %ld, req_key: %lu, service: %s, method: %s\n",
              meta.data_size, request->ByteSizeLong() + request_meta_size,
              req_key, service_name.c_str(), method_name.c_str());

            SPDK_INFOLOG(
              msg,
              "Enqueued rpc task(%s::%s) %lu\n",
              meta.service_name,
              meta.method_name,
              req->request_key);

            auto cur_thread = spdk_get_thread();
            if(_master->get_thread() != cur_thread){
                SPDK_DEBUGLOG(msg, "current thread %lu, thread %lu\n", spdk_thread_get_id(cur_thread), spdk_thread_get_id(_master->get_thread()));
                auto* ctx = (enqueue_request_context*)::spdk_zmalloc(
                  sizeof(enqueue_request_context),
                  0, nullptr, SPDK_ENV_LCORE_ID_ANY,
                  SPDK_MALLOC_DMA);
                ctx->req = std::move(req);
                ctx->this_conn = this;
                ::spdk_thread_send_msg(_master->get_thread(), on_enqueue_request, ctx);
            } else {
                enqueue_request(std::move(req));
            }
        }

        bool process_rpc_request(std::list<std::weak_ptr<connection>>::iterator busy_it) {
            if (_onflight_requests.empty()) {
                return false;
            }

            auto it = _onflight_requests.begin();
            auto* task_ptr = it->get();
            FASTBLOCK_DUR_MAP_EMPLACE_START(_proc_rpc_map, task_ptr->request_key);
            auto rc = process_request_once(*it);

            if (rc == -EAGAIN) {
                return false;
            }

            ++_onflight_rpc_task_size;
            SPDK_DEBUGLOG(
              msg,
              "_onflight_rpc_task_size: %ld, rpc queue depth: %ld, rc is %d\n",
              _onflight_rpc_task_size,
              _priority_onflight_requests.size() + _onflight_requests.size(), rc);

            auto* stack_ptr = it->get();
            if (rc == -EINVAL) {
                SPDK_ERRLOG(
                  "ERROR: Timeout occured of rpc request key %lu\n",
                  stack_ptr->request_key);
                complete_rpc(stack_ptr, rpc_fail::timeout);
            } else if (rc == -ENOMEM) {
                complete_rpc(stack_ptr, rpc_fail::resource_exhausted);
            } else if (rc == -ENOLINK) {
                process_terminating_request(stack_ptr);
                complete_rpc(stack_ptr, rpc_fail::connection_dead);
            } else if (rc != 0) {
                SPDK_ERRLOG("ERROR: Sending rpc request failed, rc is %d\n", rc);
                auto err_text = FB_FMT_1("error, rc %1%", rc);
                complete_rpc(stack_ptr, rpc_fail::other, err_text.c_str());
            }

            FASTBLOCK_DUR_MAP_EMPLACE_END_LAST(_proc_rpc_map);
            _onflight_requests.erase(it);
            _busy_list->erase(busy_it);
            return true;
        }

        bool process_priority_rpc_request(std::list<std::weak_ptr<connection>>::iterator busy_it) {
            if (_priority_onflight_requests.empty()) {
                return false;
            }

            SPDK_DEBUGLOG(
              msg,
              "process %ld rpc call heartbeat()\n",
              _priority_onflight_requests.size());

            int rc{0};
            auto erase_it_end = _priority_onflight_requests.begin();
            rpc_request* stack_ptr;
            for (auto it = _priority_onflight_requests.begin(); it != _priority_onflight_requests.end(); ++it) {
                rc = process_request_once(*it);
                if (rc == -EAGAIN) {
                    erase_it_end = it;
                    break;
                }

                ++_onflight_rpc_task_size;
                SPDK_DEBUGLOG(
                  msg,
                  "_onflight_rpc_task_size: %ld, rpc queue depth: %ld\n",
                  _onflight_rpc_task_size,
                  _priority_onflight_requests.size() + _onflight_requests.size());

                stack_ptr = it->get();
                if (rc == -EINVAL) {
                    complete_rpc(stack_ptr, rpc_fail::timeout);
                } else if (rc == -ENOMEM) {
                    complete_rpc(stack_ptr, rpc_fail::resource_exhausted);
                } else if (rc == -ENOLINK) {
                    process_terminating_request(stack_ptr);
                    complete_rpc(stack_ptr, rpc_fail::connection_dead);
                } else if (rc != 0) {
                    SPDK_ERRLOG("ERROR: Sending rpc request failed, rc is %d\n", rc);
                    auto err_text = FB_FMT_1("error, rc %1%", rc);
                    complete_rpc(stack_ptr, rpc_fail::other, err_text.c_str());
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

            if (_state != state::running) {
                return false;
            }

            auto head = _free_server_list.front();
            auto err = send_finish_request(head);
            if (err) {
                if (err->err.value() == ENOMEM) {
                    SPDK_NOTICELOG(
                      "Post free wr of request %lu return enomem, onflight_send_wr: %d\n",
                      head, _onflight_send_wr);
                    return false;
                }

                SPDK_ERRLOG(
                  "ERROR: Post send work request error '%s'\n",
                  err->err.message().c_str());
                _ctrl_ops.push_back(std::make_unique<control_operation>(state::shutdown));
            }

            _free_server_list.pop_front();
            return true;
        }

        bool process_send_read_wr() {
            if (_send_read_requests.empty()) {
                return false;
            }

            auto& req = _send_read_requests.front();
            auto err = send_read_request(req->reply_data.get());
            if (err) {
                if (err->err.value() == ENOMEM) {
                    SPDK_NOTICELOG(
                      "enomem for post read wr of request %lu, on_flight_send_wr: %d\n",
                      req->request_key, _onflight_send_wr);
                    return true;
                }

                SPDK_ERRLOG(
                  "ERROR: Post send work request error '%s'\n",
                  err->err.message().c_str());
                _ctrl_ops.push_back(std::make_unique<control_operation>(state::shutdown));
                return true;
            }

            SPDK_DEBUGLOG(
              msg,
              "request key of %lu start rdma reading, _wait_read_requests addr is %p\n",
              req->request_key,
              &_wait_read_requests);
            _wait_read_requests.push_back(std::move(req));
            _send_read_requests.pop_front();

            return true;
        }

        bool process_read_complete() {
            if (_wait_read_requests.empty()) {
                return false;
            }

            bool is_parsed{false};
            auto* stack_ptr = _wait_read_requests.begin()->get();
            if (is_timeout(stack_ptr)) {
                SPDK_ERRLOG(
                  "Timeout occurred on rpc request of request key %lu\n",
                  stack_ptr->request_key);
                complete_rpc(stack_ptr, rpc_fail::timeout);
                quarantine_request(stack_ptr);
                goto read_done;
            }

            if (not stack_ptr->reply_data->is_rdma_read_complete()) {
                return false;
            }

            SPDK_INFOLOG(
              msg,
              "Read the response body of request %lu\n",
              stack_ptr->request_key);

            SPDK_DEBUGLOG(msg, "_onflight_rpc_task_size: %ld\n", _onflight_rpc_task_size);
            {
                std::string_view reply_payload;
                size_t env_sz = 0;
                if (auto* head = stack_ptr->reply_data->data_head();
                    head != nullptr) {
                    auto rh = *reinterpret_cast<reply_meta*>(head);
                    env_sz = rh.data_size;
                }
                is_parsed = stack_ptr->reply_data->unserialize_data(
                  stack_ptr->response, reply_meta_size, env_sz, &reply_payload);
                if (is_parsed && !reply_payload.empty()) {
                    is_parsed =
                      attach_rpc_payload(*stack_ptr->response, reply_payload);
                }
            }
            if (not is_parsed) {
                SPDK_ERRLOG(
                  "ERROR: Unserialize the response body of request key %lu failed\n",
                  stack_ptr->request_key);
                complete_rpc(stack_ptr, rpc_fail::unserialize);
            } else {
                complete_rpc(stack_ptr, rpc_fail::ok);
            }

read_done:
            _free_server_list.push_back(stack_ptr->request_key);
            --_onflight_rpc_task_size;
            if (not stack_ptr->completed) {
                complete_rpc(stack_ptr, rpc_fail::ok);
            }
            auto rpc_dur = std::chrono::system_clock::now() - stack_ptr->start_at;
            _wait_read_requests.pop_front();

            if (rpc_dur >= _opts->slow_rpc_warn) {
                SPDK_WARNLOG(
                  "slow rpc detected, request id %lu, rpc duration %ldms\n",
                  stack_ptr->request_key, rpc_dur.count() / 1000 / 1000);
            }

            return true;
        }

        bool handle_cqe() {
            if (cqe_list.empty()) {
                return false;
            }

            auto cqe = std::move(cqe_list.front());
            cqe_list.pop_front();

            auto send_it = _external_send_ctx.find(cqe->wr_id);
            if (send_it != _external_send_ctx.end()) {
                auto cb = std::move(send_it->second.on_complete);
                _external_send_ctx.erase(send_it);
                if (cb) {
                    cb();
                }
                return true;
            }

            switch (cqe->opcode) {
            case ::IBV_WC_SEND: {
                auto max_wr = _sock->max_slient_wr();
                _onflight_send_wr -= max_wr;
                if (static_cast<uint32_t>(_onflight_send_wr) < max_wr) {
                    _should_signal = false;
                }
                break;
            }
            case ::IBV_WC_RECV: {
                SPDK_DEBUGLOG(msg, "handle cqe of wr id '%ld'\n", cqe->wr_id);
                auto it = _recv_ctx_map.find(cqe->wr_id);
                if (it == _recv_ctx_map.end()) {
                    SPDK_ERRLOG("ERROR: Cant find the receive context of wr id '%ld'\n", cqe->wr_id);
                    return true;
                }

                auto* recv_ctx = it->second;
                auto req_key = transport_data::read_correlation_index(recv_ctx);
                auto req_it = _unresponsed_requests.find(req_key);
                if (req_it == _unresponsed_requests.end()) {
                    if (_done_keys.contains(req_key)) {
                        post_recv_ctx(recv_ctx, it->first);
                        return true;
                    }
                    ++rdma_probe.rpc_unknown_key;
                    SPDK_INFOLOG(
                      msg,
                      "unknown rpc key %lu wr_id=%lu, drop and keep QP, unknown_key=%lu\n",
                      req_key, cqe->wr_id, rdma_probe.rpc_unknown_key);
                    post_recv_ctx(recv_ctx, it->first);
                    return true;
                }

                if (is_timeout(req_it->second.get())) {
                    SPDK_ERRLOG(
                      "Timeout occurred on rpc request of request key %lu\n",
                      req_it->second->request_key);
                    quarantine_request(req_it->second.get());
                    complete_rpc(req_it->second.get(), rpc_fail::timeout);
                    _free_server_list.push_back(req_it->second->request_key);
                    --_onflight_rpc_task_size;
                    SPDK_DEBUGLOG(msg, "_onflight_rpc_task_size: %ld\n", _onflight_rpc_task_size);
                    _unresponsed_requests.erase(req_it);
                    break;
                }

                req_it->second->request_data.reset(nullptr);
                auto reply_m = transport_data::read_reply_meta(it->second);
                SPDK_INFOLOG(
                  msg,
                  "Received reply of request %lu, status is %s\n",
                  req_it->second->request_key,
                  string_status(reply_m));

                SPDK_DEBUGLOG(
                  msg,
                  "received reply of request %lu, status is %s, elapsed: %ldus\n",
                  req_it->second->request_key,
                  string_status(reply_m),
                  (std::chrono::system_clock::now() - req_it->second->start_at).count() / 1000);

                switch (reply_m) {
                case status::no_content: {
                    complete_rpc(req_it->second.get(), rpc_fail::ok);
                    _free_server_list.push_back(req_it->second->request_key);

                    --_onflight_rpc_task_size;
                    SPDK_DEBUGLOG(msg, "_onflight_rpc_task_size: %ld\n", _onflight_rpc_task_size);
                    _unresponsed_requests.erase(req_it);
                    break;
                }
                case status::terminating: {
                    _ctrl_ops.push_back(std::make_unique<control_operation>(state::peer_terminate));
                    complete_rpc(req_it->second.get(), rpc_fail::other, "server terminating");
                    --_onflight_rpc_task_size;
                    _unresponsed_requests.erase(req_it);
                    break;
                }
                case status::success: {
                    auto* response = req_it->second->response;
                    bool is_parsed{false};
                    if (transport_data::is_inlined(it->second)) {
                        auto* inlined_data = transport_data::read_inlined_content(it->second);
                        auto reply_h = *reinterpret_cast<reply_meta*>(inlined_data);
                        auto unserialize_size = transport_data::read_inlined_content_length(it->second);
                        SPDK_DEBUGLOG(msg, "unserilaize inlined reply, size is %d\n", unserialize_size);
                        const auto env = reply_h.data_size != 0
                          ? reply_h.data_size
                          : static_cast<uint32_t>(unserialize_size);
                        is_parsed = response->ParseFromArray(
                          inlined_data + reply_meta_size, env);
                        if (is_parsed && reply_h.payload_size > 0) {
                            is_parsed = attach_rpc_payload(
                              *response,
                              std::string_view(
                                inlined_data + reply_meta_size + env,
                                reply_h.payload_size));
                        }
                    } else {
                        if (not req_it->second->reply_data) {
                            req_it->second->reply_data = std::make_unique<transport_data>(_data_pool);
                        }

                        req_it->second->reply_data->from_net_context(it->second);
                        if (not req_it->second->reply_data->is_metadata_complete()) {
                            SPDK_DEBUGLOG(
                              msg,
                              "request key %lu, metadata is not complete\n",
                              req_it->second->request_key);
                            break;
                        }

                        if (not req_it->second->reply_data->is_ready()) {
                            SPDK_NOTICELOG(
                              "request key %lu reply transport not prepared, fail-soft\n",
                              req_it->second->request_key);
                            complete_rpc(
                              req_it->second.get(),
                              req_it->second->reply_data->too_large()
                                ? rpc_fail::too_large
                                : rpc_fail::resource_exhausted);
                            _free_server_list.push_back(req_it->second->request_key);
                            --_onflight_rpc_task_size;
                            _unresponsed_requests.erase(req_it);
                            break;
                        }

                        _send_read_requests.push_back(std::move(req_it->second));
                        _unresponsed_requests.erase(req_it);

                        break;
                    }

                    --_onflight_rpc_task_size;
                    SPDK_DEBUGLOG(msg, "_onflight_rpc_task_size: %ld\n", _onflight_rpc_task_size);
                    if (not is_parsed) {
                        SPDK_ERRLOG(
                          "ERROR: Parse response body failed of request %lu\n",
                          req_it->second->request_key);

                        complete_rpc(req_it->second.get(), rpc_fail::unserialize, "unserialize failed");
                        _free_server_list.push_back(req_it->second->request_key);
                        _unresponsed_requests.erase(req_it);
                        break;
                    }

                    SPDK_INFOLOG(
                      msg,
                      "Read the inlined response body of request %lu\n",
                      req_it->second->request_key);
                    complete_rpc(req_it->second.get(), rpc_fail::ok);
                    _free_server_list.push_back(req_it->second->request_key);
                    _unresponsed_requests.erase(req_it);
                    break;
                }
                default: {
                    SPDK_ERRLOG(
                      "ERROR: RPC call failed of request %lu with reply status %s\n",
                      req_it->second->request_key,
                      string_status(reply_m));
                    if (reply_m == status::resource_exhausted) {
                        complete_rpc(req_it->second.get(), rpc_fail::resource_exhausted);
                    } else if (reply_m == status::request_timeout) {
                        complete_rpc(req_it->second.get(), rpc_fail::request_timeout);
                    } else {
                        complete_rpc(req_it->second.get(), rpc_fail::other, string_status(reply_m));
                    }
                    _free_server_list.push_back(req_it->second->request_key);
                    --_onflight_rpc_task_size;
                    _unresponsed_requests.erase(req_it);
                    break;
                }
                }

                post_recv_ctx(it->second, it->first);
            }
            case ::IBV_WC_RDMA_WRITE:
            default:
                break;
            }

            return true;
        }

        bool process_control_op() {
            if (_ctrl_ops.empty()) {
                return false;
            }

            auto op = std::move(_ctrl_ops.front());
            _ctrl_ops.pop_front();

            switch (_state) {
            case state::termianted:
                return false;
            case state::running:
                _state = op->change_state;
                break;
            default:
                _state = op->change_state;
                return true;
            }

            switch (_state) {
            case state::bad_event:
            case state::peer_terminate:
            case state::shutdown: {
                auto* opts = _master->get_options();
                _shutdown_timeout = std::chrono::system_clock::now() + _opts->shutdown_timeout;
                break;
            }
            default:
                break;
            }

            return true;
        }

        bool process_shutdown() {
            if (_state == state::running or _state == state::termianted) {
                return false;
            }

            if (_shutdown_timeout < std::chrono::system_clock::now() or shutdown_drained()) {
                auto master = _master;
                auto* cm_id = _sock ? _sock->id() : nullptr;
                auto conn_id = _id;
                auto dispatch_id = _dispatch_id.dispatch_id();
                if (_shutdown_timeout < std::chrono::system_clock::now() &&
                    !shutdown_drained()) {
                    SPDK_WARNLOG(
                      "shutdown timeout with pending state: conn id %lu, dispatch id %lu, onflight=%lu priority_onflight=%lu wait_read=%lu send_read=%lu unresponsed=%lu external_send=%lu cqes=%lu onflight_send_wr=%d\n",
                      conn_id.value(),
                      dispatch_id,
                      _onflight_requests.size(),
                      _priority_onflight_requests.size(),
                      _wait_read_requests.size(),
                      _send_read_requests.size(),
                      _unresponsed_requests.size(),
                      _external_send_ctx.size(),
                      cqe_list.size(),
                      _onflight_send_wr);
                }
                free_resources();
                if (master && cm_id) {
                    master->handle_connection_shutdown(cm_id, conn_id, dispatch_id);
                }
                _state = state::termianted;
                return true;
            }

            return false;
        }

        void free_resources() {
            if (_resources_freed) {
                return;
            }
            _resources_freed = true;
            if (_quota_held) {
                if (_meta_pool) {
                    _meta_pool->remove_connection();
                }
                if (_data_pool) {
                    _data_pool->remove_connection();
                }
                _quota_held = false;
            }
            _poller.unregister_poller();
            SPDK_INFOLOG(msg, "The poller of the connection has been unregistered\n");

            reclaim_quarantine(true);

            for (auto& task : _onflight_requests) {
                complete_rpc(task.get(), rpc_fail::connection_dead);
            }
            _onflight_requests.clear();

            for (auto& task : _priority_onflight_requests) {
                complete_rpc(task.get(), rpc_fail::connection_dead);
            }
            _priority_onflight_requests.clear();

            for (auto& task : _wait_read_requests) {
                complete_rpc(task.get(), rpc_fail::connection_dead);
            }
            _wait_read_requests.clear();

            for (auto& task : _send_read_requests) {
                complete_rpc(task.get(), rpc_fail::connection_dead);
            }
            _send_read_requests.clear();

            for (auto& task_pair : _unresponsed_requests) {
                complete_rpc(task_pair.second.get(), rpc_fail::connection_dead);
            }
            _unresponsed_requests.clear();

            if (_sock) {
                _sock->close();
            }

            if (_recv_ctx and _recv_pool) {
                _recv_pool->put_bulk(std::move(_recv_ctx), _opts->per_post_recv_num);
            }
            if (_recv_pool) {
                _recv_pool->free();
                _recv_pool.reset();
            }

            _recv_ctx_map.clear();
            _free_server_list.clear();
            _ctrl_ops.clear();
            _external_send_ctx.clear();
            cqe_list.clear();

            _sock.reset();
            _busy_list.reset();
            _busy_priority_list.reset();
            _master.reset();
            _meta_pool.reset();
            _data_pool.reset();
            _opts.reset();
        }

        auto async_shutdown(const state shutdown_state = state::shutdown) {
            auto* thread = _master->get_thread();
            auto* ctx = new shutdown_context{this, shutdown_state};
            return ::spdk_thread_send_msg(thread, on_async_shutdown, ctx);
        }

    public:

        void handle_async_shutdown(const state shutdown_state) {
            ++rdma_probe.rpc_qp_teardown;
            SPDK_ERRLOG(
              "qp teardown %s state=%d qp_teardown=%lu (not unknown_key)\n",
              fd().peer_address().c_str(),
              static_cast<int>(shutdown_state),
              rdma_probe.rpc_qp_teardown);
            FASTBLOCK_DUR_MAP_PRINT(_enqueue_dur_map);
            FASTBLOCK_DUR_MAP_PRINT(_proc_rpc_map);
            _ctrl_ops.push_back(std::make_unique<control_operation>(shutdown_state));
        }

        static void on_async_shutdown(void* arg) {
            auto* ctx = reinterpret_cast<shutdown_context*>(arg);
            ctx->this_conn->handle_async_shutdown(ctx->shutdown_state);
            delete ctx;
        }

    public:

        socket& fd() noexcept {
            return *_sock;
        }

        auto post_send_wr(::ibv_send_wr* wr, const size_t n_wrs = 1) {
            return send_request([wr] (bool should_signal) {
                if (should_signal) {
                    wr->send_flags |= IBV_SEND_SIGNALED;
                }
                return wr;
            }, n_wrs);
        }

        auto post_external_send_wr(::ibv_send_wr* wr, std::function<void()> on_complete) {
            _dispatch_id.inc_request_id();
            wr->wr_id = _dispatch_id.value();
            wr->send_flags |= IBV_SEND_SIGNALED;
            _external_send_ctx.emplace(
              wr->wr_id,
              external_send_context{.on_complete = std::move(on_complete)});
            auto ret = _sock->send(wr);
            if (ret) {
                _external_send_ctx.erase(wr->wr_id);
            }

            return ret;
        }

        connection_id id() noexcept {
            return _id;
        }

        auto dispatch_id() noexcept {
            return _dispatch_id.dispatch_id();
        }

        bool is_terminated() noexcept {
            return _state == state::termianted;
        }

        void shutdown_now() {
            free_resources();
            _state = state::termianted;
        }

        bool task_queue_empty() {
            return _unresponsed_requests.empty() and _wait_read_requests.empty();
        }

        bool shutdown_drained() {
            return _onflight_requests.empty()
                && _priority_onflight_requests.empty()
                && _wait_read_requests.empty()
                && _send_read_requests.empty()
                && _unresponsed_requests.empty()
                && _external_send_ctx.empty()
                && cqe_list.empty()
                && _onflight_send_wr == 0;
        }

    private:

        state _state{state::running};
        connection_id _id{};
        work_request_id _dispatch_id;
        std::unique_ptr<socket> _sock{nullptr};
        std::shared_ptr<options> _opts{nullptr};

        std::shared_ptr<memory_pool<::ibv_send_wr>> _meta_pool{nullptr};
        std::shared_ptr<memory_pool<::ibv_send_wr>> _data_pool{nullptr};
        std::unique_ptr<memory_pool<::ibv_recv_wr>> _recv_pool{nullptr};
        std::unique_ptr<memory_pool<::ibv_recv_wr>::net_context*[]> _recv_ctx{nullptr};
        std::unordered_map<work_request_id::value_type, memory_pool<::ibv_recv_wr>::net_context*> _recv_ctx_map{};

        uint32_t _key_idx{1};
        uint32_t _key_gen{1};
        completed_key_ring _done_keys{};
        std::list<std::unique_ptr<rpc_request>> _onflight_requests{};
        std::list<std::unique_ptr<rpc_request>> _priority_onflight_requests{};
        std::list<rpc_request::key_type> _free_server_list{};
        std::unordered_map<rpc_request::key_type, std::unique_ptr<rpc_request>> _unresponsed_requests{};
        std::list<std::unique_ptr<rpc_request>> _send_read_requests{};
        std::list<std::unique_ptr<rpc_request>> _wait_read_requests{};
        std::list<std::unique_ptr<control_operation>> _ctrl_ops{};

        std::shared_ptr<std::list<std::weak_ptr<connection>>> _busy_list{};
        std::shared_ptr<std::list<std::weak_ptr<connection>>> _busy_priority_list{};
        std::shared_ptr<client> _master{};
        utils::simple_poller _poller{};

        bool _should_signal{false};
        int32_t _onflight_send_wr{0};
        int64_t _onflight_rpc_task_size{0};
        int _sock_id{SPDK_ENV_SOCKET_ID_ANY};
        std::chrono::system_clock::time_point _shutdown_timeout{};
        std::unordered_map<uint64_t, external_send_context> _external_send_ctx{};
        bool _resources_freed{false};
        bool _quota_held{false};
        std::list<quarantined_mr> _mr_quarantine{};
        std::chrono::steady_clock::time_point _next_stats_dump{
          std::chrono::steady_clock::now()};

        FASTBLOCK_DUR_MAP_CREATE(rpc_request::key_type, _enqueue_dur_map, "enqueue_request");
        FASTBLOCK_DUR_MAP_CREATE(rpc_request::key_type, _proc_rpc_map, "process_rpc_request");

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

    // Thread is passed by caller and must be released by caller
    client(std::string name, ::spdk_thread* thread, std::shared_ptr<options> opts, int sock_id = SPDK_ENV_SOCKET_ID_ANY)
      : _opts{opts}
      , _dev{std::make_shared<device>()}
      , _pd{std::make_unique<protection_domain>(_dev, _opts->ep->device_name)}
      , _cq{std::make_shared<completion_queue>(_opts->ep->cq_num_entries, *_pd)}
      , _wcs{std::make_unique<::ibv_wc[]>(_opts->poll_cq_batch_size)}
      , _thread{thread}
      , _core_poller{_thread}
      , _stop_poller{_thread}
      , _sock_id{sock_id}
      , _meta_pool{std::make_shared<memory_pool<::ibv_send_wr>>(
        _pd->value(), FB_FMT_1("%1%_m", name),
        _opts->metadata_memory_pool_capacity,
        _opts->metadata_memory_pool_element_size, 0, _sock_id)}
      , _data_pool{std::make_shared<memory_pool<::ibv_send_wr>>(
        _pd->value(), FB_FMT_1("%1%_d", name),
        _opts->data_memory_pool_capacity,
        _opts->data_memory_pool_element_size, 0, _sock_id)} {
        pool_quota q{};
        q.low_watermark = _opts->pool_low_watermark;
        q.high_watermark = _opts->pool_high_watermark;
        q.reserved_per_connection = _opts->reserved_per_connection;
        q.reserved_priority = _opts->reserved_priority;
        _meta_pool->configure_quota(q);
        _data_pool->configure_quota(q);
    }

    client(const client&) = delete;

    client(client&&) = delete;

    client& operator=(const client&) = delete;

    client& operator=(client&&) = delete;

    ~client() noexcept = default;

private:

    /*
     * =======================================================================
     * private methods
     * =======================================================================
     */

    bool all_connection_termianted() {
        return std::all_of(_connections.begin(), _connections.end(), [] (auto& conn) {
            return conn.second->is_terminated();
        });
    }

    void free_resources() {
        if (_resources_freed) {
            return;
        }
        _resources_freed = true;
        _core_poller.unregister_poller();
        _stop_poller.unregister_poller();
        SPDK_INFOLOG(msg, "The poller of the rpc client has been unregistered\n");

        for (auto& [_, conn] : _connections) {
            if (conn) {
                conn->shutdown_now();
            }
        }
        for (auto& [_, task] : _connect_tasks) {
            if (task and task->conn) {
                task->conn->shutdown_now();
            }
        }

        _connections.clear();
        _cm_records.clear();
        _cqe_dispatch_map.clear();
        _connect_tasks.clear();
        _busy_connections->clear();
        _busy_priority_connections->clear();

        if (_meta_pool) {
            _meta_pool->free();
            _meta_pool.reset();
        }
        if (_data_pool) {
            _data_pool->free();
            _data_pool.reset();
        }
        _wcs.reset();
        _cq.reset();
        _pd.reset();
        _dev.reset();
        _opts.reset();
        _channel.close();

        if (_stop_ctx and _stop_ctx->on_stop_cb) {
            try {
                (_stop_ctx->on_stop_cb.value())();
            } catch (const std::exception& e) {
                SPDK_ERRLOG("Caught exception when executing callback on stopping rpc client: %s\n", e.what());
            }
        }
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
        if (ctx->conn && not ctx->conn->is_terminated()) {
            ctx->conn->async_shutdown();
        }
        ctx->cb(true);
        delete ctx;
    }

    static int core_poller(void* arg) {
        auto* this_client = reinterpret_cast<client*>(arg);
        return this_client->handle_core_poll();
    }

    static int stop_poller(void* arg) {
        auto* this_client = reinterpret_cast<client*>(arg);
        return this_client->handle_stop_poll();
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
        _core_poller.register_poller(core_poller, this, 0, "rpc_cli_core");

        if (ctx->on_start_cb) {
            ctx->on_start_cb.value()();
        }
        SPDK_INFOLOG(msg, "Rpc client started\n");
    }

    void handle_stop(std::unique_ptr<stop_context> ctx) {
        if (_is_terminated) {
            return;
        }

        SPDK_NOTICELOG(
          "Start stop the rpc client, connections=%lu connect_tasks=%lu cm_records=%lu dispatch=%lu\n",
          _connections.size(),
          _connect_tasks.size(),
          _cm_records.size(),
          _cqe_dispatch_map.size());
        for (auto& conn_pair : _connections) {
            conn_pair.second->async_shutdown();
        }

        _is_terminated = true;
        _stop_ctx = std::move(ctx);
        _stop_timeout_at = std::chrono::system_clock::now() + _opts->shutdown_timeout;
        _stop_poller.register_poller(stop_poller, this, 0, "rpc_cli_stop");
    }

    void handle_emplace_eonnection(std::unique_ptr<emplace_connection_context> ctx) {
        endpoint ep = *(_opts->ep);
        ep.addr = ctx->addr;
        ep.port = ctx->port;
        ep.passive = false;
        std::unique_ptr<socket> sock;
        try {
            sock = std::make_unique<socket>(ep, *_pd, _channel.value());
        } catch (const std::exception& e) {
            SPDK_ERRLOG(
              "ERROR: create rdma socket to %s:%d failed: %s\n",
              ep.addr.c_str(), ep.port, e.what());
            if (ctx->cb) {
                ctx->cb(false, nullptr);
            }
            return;
        }
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
          shared_from_this(),
          _sock_id);
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
                SPDK_ERRLOG(
                  "ERROR: Trying reconnting to %s exceed max times %lu\n",
                  task_ptr->conn->fd().peer_address().c_str(),
                  task_ptr->retry_ctx->retry_times);
                task_ptr->cb(false, nullptr);
                _connect_tasks.erase(task_ptr->conn->fd().id());
                break;
            }

            SPDK_INFOLOG(msg, 
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
              shared_from_this(),
              _sock_id);
            // Do not reorder the erase and emplace operations here
            _cm_records.emplace(new_fd_id, task_ptr->conn);
            _cm_records.erase(old_fd_id);
            _connections.erase(old_conn_id);
            _connections.emplace(task_ptr->conn->id(), task_ptr->conn);
            task_ptr->conn_state = connect_state::wait_address_resolved;
            SPDK_DEBUGLOG(
              msg,
              "erased old cm id %p, emplaced new cm id %p; "
              "erased old conn id %ld, emplaced new conn id %ld\n",
              old_fd_id, task_ptr->conn->fd().id(),
              old_conn_id.value(), task_ptr->conn->id().value());
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
            SPDK_ERRLOG(
              "ERROR: Cant find the connect task of cm id %p on event %s in connect tasks map\n",
              evt->id, ::rdma_event_str(evt->event));

            auto cm_rd_it = _cm_records.find(evt->id);
            if (cm_rd_it != _cm_records.end()) {
                SPDK_ERRLOG(
                  "ERROR: cm id %p should not occur in cm records map on event %s\n",
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
                SPDK_ERRLOG(
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
                    SPDK_ERRLOG(
                      "ERROR: Connect to %s error, '%s'\n",
                      fd.peer_address().c_str(),
                      rc->message().c_str());
                    task_ptr->conn_state = connect_state::connect_failed;
                } else {
                    task_ptr->conn_state = connect_state::wait_connect_established;
                }
            } else {
                SPDK_ERRLOG(
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
                SPDK_INFOLOG(msg, "Connected to %s\n", task_ptr->conn->fd().peer_address().c_str());
                task_ptr->cb(true, task_ptr->conn);
                _connect_tasks.erase(fd.id());
            } else {
                SPDK_ERRLOG(
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

        SPDK_INFOLOG(
          msg,
          "receive rdma cm event: %s, cm id: %p\n",
          ::rdma_event_str(evt->event), evt->id);

        if (_connect_tasks.contains(evt->id)) {
            process_connect_task(evt);
            return;
        }

        auto it = _cm_records.find(evt->id);
        if (it == _cm_records.end()) {
            SPDK_ERRLOG(
              "ERROR: Received cm event %s, but the cm id can not be found in cm observers\n",
              ::rdma_event_str(evt->event));

            ::rdma_ack_cm_event(evt);
            return;
        }

        auto conn_it = _connections.find(it->second->id());
        if (conn_it == _connections.end()) {
            SPDK_ERRLOG(
              "ERROR: Received cm event %s, but the connection id(%lu) can not be found in connection map\n",
              ::rdma_event_str(evt->event), it->second->id().value());
            _cm_records.erase(it);
            ::rdma_ack_cm_event(evt);

            return;
        }

        SPDK_INFOLOG(
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

            SPDK_ERRLOG(
              "ERROR: failed process cm event, will close the connection(rdma cm id: %p)\n",
              evt_id);

            SPDK_INFOLOG(msg, "Reomve the connection(id: %p)\n", evt_id);
            conn_it->second->async_shutdown(connection::state::bad_event);
        }

        if (evt) {
            ::rdma_ack_cm_event(evt);
        }
    }

    int handle_core_poll() {
        handle_cm_event();

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
        std::shared_ptr<connection> conn{nullptr};
        work_request_id::dispatch_id_type dis_id;
        decltype(_cqe_dispatch_map)::iterator dispatch_it{};
        std::optional<std::error_code> errc{};

        for (int i{0}; i < rc; ++i) {
            cqe = &(_wcs[i]);
            SPDK_DEBUGLOG(msg, "polled cqe with opcode %s\n", completion_queue::op_name(cqe->opcode).c_str());
            dis_id = work_request_id::dispatch_id(cqe->wr_id);
            auto dispatch_it = _cqe_dispatch_map.find(dis_id);
            if (dispatch_it == _cqe_dispatch_map.end()) {
                SPDK_NOTICELOG(
                  "cant find the connection with dispatch id %ld, work request id %ld, cqe opcode is %s\n",
                  dis_id, cqe->wr_id, completion_queue::op_name(cqe->opcode).c_str());
                continue;
            }

            conn = dispatch_it->second;

            if (cqe->status != ::IBV_WC_SUCCESS) {
                if (not conn->is_terminated()) {
                    conn->fd().update_qp_attr();
                    SPDK_ERRLOG(
                      "ERROR: Got error wc, wc.vendor_err=%d, wc.status=%s, qp.state=%s\n",
                      cqe->vendor_err,
                      socket::wc_status_name(cqe->status).c_str(),
                      conn->fd().qp_state_str().c_str());
                    conn->async_shutdown(connection::state::bad_event);
                }

                continue;
            }

            auto cpy_cqe = std::make_unique<::ibv_wc>(_wcs[i]);
            conn->cqe_list.push_back(std::move(cpy_cqe));
        }

        return SPDK_POLLER_BUSY;
    }

    int handle_stop_poll() {
        auto timeout = _stop_timeout_at < std::chrono::system_clock::now();
        if (timeout && !all_connection_termianted()) {
            SPDK_WARNLOG(
              "rpc client stop timeout: connections=%lu connect_tasks=%lu cm_records=%lu dispatch=%lu\n",
              _connections.size(),
              _connect_tasks.size(),
              _cm_records.size(),
              _cqe_dispatch_map.size());
        }
        if (timeout or all_connection_termianted()) {
            free_resources();
        }

        return SPDK_POLLER_IDLE;
    }

public:

    /*
     * =======================================================================
     * public methods
     * =======================================================================
     */

    void start(std::optional<std::function<void()>> on_start_cb = std::nullopt) {
        SPDK_DEBUGLOG(msg, "sending rpc client start message\n");
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
    ::spdk_thread* _thread{nullptr};
    utils::simple_poller _core_poller{};
    utils::simple_poller _stop_poller{};
    std::unique_ptr<::ibv_wc[]> _wcs{nullptr};

    std::unordered_map<work_request_id::dispatch_id_type, std::shared_ptr<connection>> _cqe_dispatch_map{};
    std::unordered_map<connection_id, std::shared_ptr<connection>> _connections{};
    std::unordered_map<::rdma_cm_id*, std::shared_ptr<connection>> _cm_records{};
    std::unordered_map<::rdma_cm_id*, std::unique_ptr<connect_task>> _connect_tasks{};
    int _sock_id{SPDK_ENV_SOCKET_ID_ANY};
    std::shared_ptr<memory_pool<::ibv_send_wr>> _meta_pool{nullptr};
    std::shared_ptr<memory_pool<::ibv_send_wr>> _data_pool{nullptr};

    std::shared_ptr<std::list<std::weak_ptr<connection>>> _busy_connections{
      std::make_shared<std::list<std::weak_ptr<connection>>>()};
    std::shared_ptr<std::list<std::weak_ptr<connection>>> _busy_priority_connections{
      std::make_shared<std::list<std::weak_ptr<connection>>>()};

    event_channel _channel{};
    std::chrono::system_clock::time_point _stop_timeout_at{};
    std::unique_ptr<stop_context> _stop_ctx{nullptr};
    bool _resources_freed{false};
};

} // namespace rdma
} // namespace msg
