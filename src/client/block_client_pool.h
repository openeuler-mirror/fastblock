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

#include "client/hash.h"
#include "client/pg_client.h"
#include "client/connection_pool.h"
#include "utils/worker_pool.h"

#include <google/protobuf/stubs/callback.h>

#include <spdk/bdev_module.h>
#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/thread.h>

#include <functional>

namespace fastblock {
namespace client {

class block_client_pool {
public:

    enum class worker_ctrl_message {
        new_write_request_args = 1,
        new_read_request_args,
        new_delete_request_args,
        free_resouce,
        request_done
    };

    struct iovec_data {
        ::iovec* iovs{nullptr};
        int iovec_index{0};
        int64_t size{0};
        std::ptrdiff_t iovec_offset{0};
    };

    using data_args_type = std::variant<
      std::monostate,
      std::unique_ptr<iovec_data>,
      std::string*
    >;

    using request_type = std::variant<
      std::monostate,
      std::unique_ptr<osd::write_request>,
      std::unique_ptr<osd::read_request>,
      std::unique_ptr<osd::delete_request>>;

    using response_type = std::variant<
      std::monostate,
      std::unique_ptr<osd::write_reply>,
      std::unique_ptr<osd::read_reply>,
      std::unique_ptr<osd::delete_reply>
    >;

    using write_callback_type = std::function<void(int32_t, std::function<void()>)>;
    using read_callback_type = std::function<void(const std::string&, int32_t, std::function<void()>)>;
    using delete_callback_type = std::function<void(int32_t)>;
    using callback_type = std::variant<
      std::monostate,
      write_callback_type,
      read_callback_type
    >;

    struct write_args_type {
        std::string object_name{};
        uint64_t offset{0};
        int32_t pool_id{0};
        write_callback_type cb{};
        data_args_type data{};
        size_t worker_id{};
        block_client_pool* this_pool{nullptr};
    };

    struct read_args_type {
        std::string object_name{};
        uint64_t offset{0};
        uint64_t length{0};
        int32_t pool_id{0};
        read_callback_type cb{};
        size_t worker_id{};
        block_client_pool* this_pool{nullptr};
    };

    struct delete_args_type {
        std::string object_name{};
        int32_t pool_id{0};
        delete_callback_type cb{};
        size_t worker_id{};
        block_client_pool* this_pool{nullptr};
    };

    struct request_stack_type {
        uint64_t id{0};
        int32_t pool_id{};
        int32_t pg_id{};
        osd::rpc_service_osd_Stub* stub{nullptr};
        request_type req{std::monostate{}};
        response_type resp{std::monostate{}};
        callback_type cb{};
        size_t worker_id{};
        block_client_pool* this_pool{nullptr};
    };

    using message_type = std::variant<
      std::monostate,
      std::unique_ptr<request_stack_type>,
      std::unique_ptr<write_args_type>,
      std::unique_ptr<read_args_type>,
      std::unique_ptr<delete_args_type>,
      request_stack_type*
    >;

    struct worker_message {
        worker_ctrl_message ctrlr_msg{};
        message_type msg{std::monostate{}};
    };

    struct worker_state {
        block_client_pool* this_pool{nullptr};
        std::unordered_map<uint64_t, std::unique_ptr<request_stack_type>> requests{};
    };

public:

    block_client_pool() = delete;

    block_client_pool(
      ::spdk_thread* thread,
      monitor::client* mon_cli,
      pg_client* pg_cli,
      connection_pool* conn_pool,
      size_t n_workers,
      core_sharded::core_container_type& cores)
      : _mon_cli{mon_cli}
      , _pg_client{pg_cli}
      , _workers{"blkcli", n_workers, cores}
      , _conn_pool{conn_pool}
      , _mgr{thread} {
        worker_state s{this};
        _workers.set_state(std::move(s));
        _workers.register_handler(handle_worker_message);
    }

    block_client_pool(const block_client_pool&) = delete;

    block_client_pool(block_client_pool&&) = default;

    block_client_pool& operator=(const block_client_pool&) = delete;

    block_client_pool& operator=(block_client_pool&&) = delete;

    ~block_client_pool() noexcept = default;

public:

    /************************************************************
     * spdk_thread_send_msg callbacks' callbacks
     ************************************************************/


    /************************************************************
     * worker message handler
     ************************************************************/

    static void handle_worker_message(worker_message* msg, worker_state* state) {
        switch (msg->ctrlr_msg) {
        case worker_ctrl_message::new_write_request_args: {
            state->this_pool->handle_write_request_args(std::unique_ptr<worker_message>{msg}, state);
            break;
        }
        case worker_ctrl_message::new_read_request_args: {
            state->this_pool->handle_read_request_args(std::unique_ptr<worker_message>{msg}, state);
            break;
        }
        case worker_ctrl_message::request_done: {
            state->this_pool->handle_request_done(std::unique_ptr<worker_message>{msg}, state);
            break;
        }
        case worker_ctrl_message::free_resouce: {
            state->this_pool->handle_free_resource(std::unique_ptr<worker_message>{msg}, state);
            break;
        }
        }
    }

    /************************************************************
     * spdk_thread_send_msg callbacks
     ************************************************************/


private:

    /************************************************************
     * private methods
     ************************************************************/

    void on_reqeust_done(request_stack_type* stack_ptr) {
        auto* msg = new worker_message{worker_ctrl_message::request_done, stack_ptr};
        _workers.send_message(stack_ptr->worker_id, msg);
    }

    void copy_iovs(::iovec* iovs, size_t start_index, std::ptrdiff_t cpy_offset, int64_t len, char* dst) {
        auto* src = reinterpret_cast<char*>(iovs[start_index].iov_base) + cpy_offset;
        auto cpy_len = std::min(static_cast<size_t>(len), iovs[start_index].iov_len - cpy_offset);
        while (len > 0) {
            SPDK_DEBUGLOG(
              blkcli,
              "start_index %ld, cpy_offset %ld, len %ld, cpy_len %ld\n",
              start_index, cpy_offset, len, cpy_len);
            std::memcpy(dst, src, cpy_len);

            ++start_index;
            src = reinterpret_cast<char*>(iovs[start_index].iov_base);
            len -= cpy_len;
            cpy_len = std::min(len, static_cast<int64_t>(iovs[start_index].iov_len));
        }
    }

    void handle_free_resource(std::unique_ptr<worker_message> msg, worker_state* state) {
        auto* stack_ptr = std::get<request_stack_type*>(msg->msg);
        SPDK_DEBUGLOG(blkcli, "free request %ld\n", stack_ptr->id);
        state->requests.erase(stack_ptr->id);
    }

    void handle_request_done(std::unique_ptr<worker_message> msg, worker_state* state) {
        auto stack_ptr = std::get<request_stack_type*>(msg->msg);
        if (std::holds_alternative<std::unique_ptr<osd::write_reply>>(stack_ptr->resp)) {
            auto& req = std::get<std::unique_ptr<osd::write_request>>(stack_ptr->req);
            auto& resp = std::get<std::unique_ptr<osd::write_reply>>(stack_ptr->resp);
            SPDK_INFOLOG(
              blkcli,
              "write_object pool: %lu pg: %lu object: %s offset: %lu done, state: %d\n",
              req->pool_id(), req->pg_id(), req->object_name().c_str(),
              req->offset(), resp->state());
            auto& cb = std::get<write_callback_type>(stack_ptr->cb);
            cb(resp->state(), [this, stack_ptr, worker_id = stack_ptr->worker_id] () {
                auto* msg = new worker_message{worker_ctrl_message::free_resouce, stack_ptr};
                _workers.send_message(worker_id, msg);
            });
        } else if (std::holds_alternative<std::unique_ptr<osd::read_reply>>(stack_ptr->resp)) {
            auto& req = std::get<std::unique_ptr<osd::read_request>>(stack_ptr->req);
            auto& resp = std::get<std::unique_ptr<osd::read_reply>>(stack_ptr->resp);
            SPDK_INFOLOG(
              blkcli,
              "read_object pool: %lu pg:%lu object:%s offset:%lu done. state:%d data size: %lu\n",
              req->pool_id(), req->pg_id(), req->object_name().c_str(),
              req->offset(), resp->state(), resp->data().size());

            auto& cb = std::get<read_callback_type>(stack_ptr->cb);
            cb(resp->data(), resp->state(), [this, stack_ptr, worker_id = stack_ptr->worker_id] () {
                auto* msg = new worker_message{worker_ctrl_message::free_resouce, stack_ptr};
                _workers.send_message(worker_id, msg);
            });
        } else {
            SPDK_ERRLOG("Unknown reply type\n");
            throw std::runtime_error{"Unknown reply type"};
        }
    }

    template <typename proto_request_type, typename proto_response_type>
    void handle_request(request_stack_type* stack_ptr, osd::rpc_service_osd_Stub* stub, auto rpc_method) {
        auto& proto_req = std::get<std::unique_ptr<proto_request_type>>(stack_ptr->req);
        auto done = google::protobuf::NewCallback(
          this, &block_client_pool::on_reqeust_done, stack_ptr);
        auto proto_resp = std::make_unique<proto_response_type>();
        rpc_method(stub, &_ctrlr, proto_req.get(), proto_resp.get(), done);
        stack_ptr->resp = std::move(proto_resp);
    }

    void handle_write_request_args(std::unique_ptr<worker_message> msg, worker_state* state) {
        auto& args = std::get<std::unique_ptr<write_args_type>>(msg->msg);
        auto pg_num = _mon_cli->get_pg_num(args->pool_id);
        auto target_pg = calc_target(args->object_name, args->pool_id, pg_num);
        auto proto_req = std::make_unique<osd::write_request>();
        proto_req->set_pool_id(args->pool_id);
        proto_req->set_pg_id(target_pg);
        proto_req->set_object_name(args->object_name);
        proto_req->set_offset(args->offset);

        if (std::holds_alternative<std::string*>(args->data)) {
            auto* buf = std::get<std::string*>(args->data);
            proto_req->set_data(std::move(*buf));
        } else if (std::holds_alternative<std::unique_ptr<iovec_data>>(args->data)) {
            auto& iovec_args = std::get<std::unique_ptr<iovec_data>>(args->data);
            if (not iovec_args->iovs) {
                SPDK_ERRLOG("iovec_data::iovs is nullptr\n");
                throw std::invalid_argument{"null iove"};
            }

            std::string buf(iovec_args->size, '\0');
            SPDK_DEBUGLOG(
              blkcli,
              "iovec_index: %d, iovec_offset: %ld, buf.size(): %ld\n",
              iovec_args->iovec_index,
              iovec_args->iovec_offset,
              buf.size());
            auto iovs_str = std::string(7, 'x');
            std::memcpy(iovs_str.data(), iovec_args->iovs[iovec_args->iovec_index].iov_base, 7);
            SPDK_DEBUGLOG(blkcli, "head 7 bytes of iovs is %s\n", iovs_str.c_str());
            copy_iovs(
              iovec_args->iovs,
              iovec_args->iovec_index,
              iovec_args->iovec_offset,
              iovec_args->size,
              buf.data());
            SPDK_DEBUGLOG(blkcli, "head 7 bytes of buf is ***%s***\n", buf.substr(0, 7).c_str());
            proto_req->set_data(std::move(buf));
        } else {
            SPDK_ERRLOG("Unknown write args data type\n");
            throw std::runtime_error{"Unknown write args data type"};
        }

        SPDK_INFOLOG(
          blkcli,
          "write_object pool: %u pg: %u object_name: %s offset: %lu length: %lu\n",
          args->pool_id,
          target_pg,
          args->object_name.c_str(),
          args->offset,
          proto_req->data().size());

        auto* proto_req_ptr = proto_req.get();
        auto req_stk = std::make_unique<request_stack_type>(
          _req_id_gen++,
          args->pool_id,
          target_pg,
          nullptr,
          std::move(proto_req),
          std::monostate{},
          args->cb,
          args->worker_id,
          this);
        auto* stack_ptr = req_stk.get();
        state->requests.emplace(req_stk->id, std::move(req_stk));

        _conn_pool->get_leader_stub(
          args->pool_id, target_pg, args->object_name,
          [this, proto_req_ptr, stack_ptr] (osd::rpc_service_osd_Stub* stub) {
              if (not stub) {
                  SPDK_ERRLOG(
                    "get stub for object %s failed\n",
                    proto_req_ptr->object_name().c_str());
                  throw std::runtime_error{"get leader stub failed\n"};
              }
              handle_request<osd::write_request, osd::write_reply>(
                stack_ptr, stub, std::mem_fn(&osd::rpc_service_osd::process_write));
          }
        );
    }

    void handle_read_request_args(std::unique_ptr<worker_message> msg, worker_state* state) {
        auto& args = std::get<std::unique_ptr<read_args_type>>(msg->msg);
        auto pg_num = _mon_cli->get_pg_num(args->pool_id);
        auto target_pg = calc_target(args->object_name, args->pool_id, pg_num);

        SPDK_INFOLOG(
          blkcli,
          "read_object pool: %u pg: %u object: %s offset: %lu length: %lu\n",
          args->pool_id,
          target_pg,
          args->object_name.c_str(),
          args->offset, args->length);

        auto proto_req = std::make_unique<osd::read_request>();
        proto_req->set_pool_id(args->pool_id);
        proto_req->set_pg_id(target_pg);
        proto_req->set_object_name(args->object_name);
        proto_req->set_offset(args->offset);
        proto_req->set_length(args->length);
        auto* proto_req_ptr = proto_req.get();
        auto req_stk = std::make_unique<request_stack_type>(
          _req_id_gen++,
          args->pool_id,
          target_pg,
          nullptr,
          std::move(proto_req),
          std::monostate{},
          args->cb,
          args->worker_id,
          this);
        auto* stack_ptr = req_stk.get();
        state->requests.emplace(req_stk->id, std::move(req_stk));

        SPDK_DEBUGLOG(blkcli, "start get leader stub\n");
        _conn_pool->get_leader_stub(
          args->pool_id, target_pg, args->object_name,
          [this, proto_req_ptr, stack_ptr] (osd::rpc_service_osd_Stub* stub) {
              if (not stub) {
                  SPDK_ERRLOG(
                    "get stub for object %s failed\n",
                    proto_req_ptr->object_name().c_str());
                  throw std::runtime_error{"get leader stub failed\n"};
              }
              SPDK_DEBUGLOG(
                blkcli, "leader acquired for reading object %s, stub addr is %p\n",
                proto_req_ptr->object_name().c_str(), stub);
              handle_request<osd::read_request, osd::read_reply>(
                stack_ptr, stub, std::mem_fn(&osd::rpc_service_osd::process_read));
          }
        );
    }

    void handle_delete_request_args(std::unique_ptr<worker_message> msg, worker_state* state) {
        auto& args = std::get<std::unique_ptr<read_args_type>>(msg->msg);
        auto pg_num = _mon_cli->get_pg_num(args->pool_id);
        auto target_pg = calc_target(args->object_name, args->pool_id, pg_num);

        SPDK_INFOLOG(
          blkcli,
          "delete_object pool: %d pg: %u object: %s\n",
          args->pool_id, target_pg, args->object_name.c_str());

        auto proto_req = std::make_unique<osd::delete_request>();
        proto_req->set_pool_id(args->pool_id);
        proto_req->set_pg_id(target_pg);
        proto_req->set_object_name(args->object_name);
        auto* proto_req_ptr = proto_req.get();
        auto req_stk = std::make_unique<request_stack_type>(
          _req_id_gen++,
          args->pool_id,
          target_pg,
          nullptr,
          std::move(proto_req),
          std::monostate{},
          std::move(args->cb),
          args->worker_id,
          this);
        auto* stack_ptr = req_stk.get();
        state->requests.emplace(req_stk->id, std::move(req_stk));

        _conn_pool->get_leader_stub(
          args->pool_id, target_pg, args->object_name,
          [this, proto_req_ptr, stack_ptr] (osd::rpc_service_osd_Stub* stub) {
              if (not stub) {
                  SPDK_ERRLOG(
                    "get stub for object %s failed\n",
                    proto_req_ptr->object_name().c_str());
                  throw std::runtime_error{"get leader stub failed\n"};
              }
              handle_request<osd::delete_request, osd::delete_reply>(
                stack_ptr, stub, std::mem_fn(&osd::rpc_service_osd::process_delete));
          }
        );
    }

public:

    /************************************************************
     * public methods
     ************************************************************/

    void write_object(
      std::string* buf,
      std::string&& object_name,
      uint64_t offset,
      int32_t target_pool_id,
      write_callback_type&& cb) {
        auto name_hash = std::hash<std::string>{}(object_name);
        auto worker_id = name_hash % _workers.size();
        auto args = std::make_unique<write_args_type>(
          std::move(object_name),
          offset,
          target_pool_id,
          std::move(cb),
          buf,
          worker_id,
          this);
        auto* msg = new worker_message{worker_ctrl_message::new_write_request_args, std::move(args)};
        _workers.send_message(worker_id, msg);
    }

    auto write_object(
      ::iovec* iovs,
      size_t iov_index,
      int64_t size,
      std::ptrdiff_t iov_offset,
      std::string object_name,
      uint64_t offset,
      int32_t target_pool_id,
      write_callback_type&& cb) {
        if (not iovs) {
            SPDK_ERRLOG("iovec_data::iovs is nullptr\n");
            throw std::invalid_argument{"null iove"};
        }
        auto name_hash = std::hash<std::string>{}(object_name);
        auto worker_id = name_hash % _workers.size();
        auto iov_args = std::make_unique<iovec_data>(iovs, iov_index, size, iov_offset);
        auto args = std::make_unique<write_args_type>(
          std::move(object_name),
          offset,
          target_pool_id,
          std::move(cb),
          std::move(iov_args),
          worker_id,
          this);
        auto* msg = new worker_message{worker_ctrl_message::new_write_request_args, std::move(args)};
        _workers.send_message(worker_id, msg);
    }

    void read_object(
      std::string object_name,
      uint64_t offset,
      uint64_t length,
      int32_t target_pool_id,
      read_callback_type&& cb) {
        auto name_hash = std::hash<std::string>{}(object_name);
        auto worker_id = name_hash % _workers.size();
        auto args = std::make_unique<read_args_type>(
          std::move(object_name),
          offset,
          length,
          target_pool_id,
          std::move(cb),
          worker_id, this);
        auto* msg = new worker_message{worker_ctrl_message::new_read_request_args, std::move(args)};
        _workers.send_message(worker_id, msg);
    }

    void delete_object(
      std::string object_name,
      int32_t target_pool_id,
      delete_callback_type&& cb) {
        auto name_hash = std::hash<std::string>{}(object_name);
        auto worker_id = name_hash % _workers.size();
        auto args = std::make_unique<delete_args_type>(
          std::move(object_name),
          target_pool_id,
          std::move(cb),
          worker_id,
          this);
        auto* msg = new worker_message{worker_ctrl_message::new_delete_request_args, std::move(args)};
        _workers.send_message(worker_id, msg);
    }

private:

    uint64_t _req_id_gen{0};
    monitor::client* _mon_cli{nullptr};
    pg_client* _pg_client{nullptr};
    fastblock::utils::worker_pool<worker_message*, worker_state> _workers;
    connection_pool* _conn_pool{nullptr};
    ::spdk_thread* _mgr{nullptr};
    std::unique_ptr<::utils::simple_poller> _poller{nullptr};
    msg::rdma::rpc_controller _ctrlr{};
};

} // namespace client
} // namespace fastblock
