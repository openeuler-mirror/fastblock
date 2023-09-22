#pragma once

#include "mon/client.h"
#include "msg/rpc_controller.h"
#include "msg/transport_client.h"
#include <utils/overload.h>
#include "utils/simple_poller.h"
#include "rpc/osd_msg.pb.h"
#include "rpc/connect_cache.h"

#include <google/protobuf/stubs/callback.h>

#include <mutex>
#include <variant>

typedef void (*read_callback)(struct spdk_bdev_io *, char *, uint64_t, int32_t);

typedef void (*write_callback)(struct spdk_bdev_io *, int32_t);

typedef void (*delete_callback)(struct spdk_bdev_io *, int32_t);

typedef void (*read_object_callback)(void *src, uint64_t object_idx, const std::string &data, int32_t state);
typedef void (*write_object_callback)(void *src, int32_t state);

class fblock_client
{
private:

using request_type = std::variant<
  std::monostate,
  std::unique_ptr<osd::write_request>,
  std::unique_ptr<osd::read_request>,
  std::unique_ptr<osd::delete_request>>;

using response_type = std::variant<
  std::monostate,
  std::unique_ptr<osd::write_reply>,
  std::unique_ptr<osd::read_reply>,
  std::unique_ptr<osd::delete_reply>>;

using response_callback_type = std::variant<
  std::monostate,
  write_object_callback,
  read_object_callback,
  delete_callback
>;

enum request_state {
    waiting_leader_osd = 1,
    leader_osd_acquired,
    waiting_response,
    response_acquired
};

struct request_stack_type {
    std::shared_ptr<osd::rpc_service_osd_Stub> stub{nullptr};
    std::unique_ptr<osd::pg_leader_request> leader_req{nullptr};
    request_type req{std::monostate{}};
    std::unique_ptr<osd::pg_leader_response> leader_resp{std::make_unique<osd::pg_leader_response>()};
    response_type resp{std::monostate{}};
    response_callback_type resp_cb{std::monostate{}};
    request_state state{request_state::waiting_leader_osd};
    uint64_t obj_index{0};
    void* ctx{nullptr};
};

public:

    fblock_client(monitor::client* mon_cli);

    ~fblock_client() noexcept {
        SPDK_DEBUGLOG(libblk, "call ~fblck_client()");
    }

    void connect(); // 连接函数。
    bool connect_state();

    static void do_start(void* arg) {
        auto* arg_this = reinterpret_cast<fblock_client*>(arg);
        arg_this->handle_start();
    }

    static int do_poll(void* arg) {
        auto* arg_this = reinterpret_cast<fblock_client*>(arg);
        auto is_busy = arg_this->poller_handler();

        return is_busy ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
    }

    //////////////////////////////////////////////////////////////

    void start() {
        if (not _current_thread) {
            SPDK_ERRLOG("ERROR: Cant get current spdk thread\n");
            throw std::runtime_error{"cant get current spdk thread"};
        }

        ::spdk_thread_send_msg(_current_thread, do_start, this);
    }

    bool poller_handler() {
        return process_request() | process_response();
    }

    void handle_start() {
        SPDK_INFOLOG(libblk, "Starting block client...\n");
        _poller.poller = SPDK_POLLER_REGISTER(do_poll, this, 0);
    }

    bool process_request() {
        if (_requests.empty()) {
            return false;
        }

        auto& head = _requests.front();
        if (head->state != request_state::leader_osd_acquired) {
            return false;
        }

        auto request_handler = utils::overload {
            [this, stack_ptr = head.get()] (std::unique_ptr<osd::write_request>& req) {
                auto& resp = stack_ptr->leader_resp;
                stack_ptr->stub = get_stub(
                  resp->leader_id(),
                  resp->leader_addr(),
                  resp->leader_port());

                auto reply = std::make_unique<osd::write_reply>();
                stack_ptr->state = request_state::waiting_response;
                stack_ptr->stub->process_write(
                  &_ctrlr, req.get(), reply.get(),
                  google::protobuf::NewCallback(this, &fblock_client::on_response, stack_ptr));
                stack_ptr->resp = std::move(reply);
            },

            [this, stack_ptr = head.get()] (std::unique_ptr<osd::read_request>& req) {
                auto& resp = stack_ptr->leader_resp;
                stack_ptr->stub = get_stub(
                  resp->leader_id(),
                  resp->leader_addr(),
                  resp->leader_port());
                auto reply = std::make_unique<osd::read_reply>();
                stack_ptr->state = request_state::waiting_response;
                stack_ptr->stub->process_read(
                  &_ctrlr, req.get(), reply.get(),
                  google::protobuf::NewCallback(this, &fblock_client::on_response, stack_ptr));
                stack_ptr->resp = std::move(reply);
            },

            [this, stack_ptr = head.get()] (std::unique_ptr<osd::delete_request>& req) {
                auto& resp = stack_ptr->leader_resp;
                stack_ptr->stub = get_stub(
                  resp->leader_id(),
                  resp->leader_addr(),
                  resp->leader_port());
                auto reply = std::make_unique<osd::delete_reply>();
                stack_ptr->state = request_state::waiting_response;
                stack_ptr->stub->process_delete(
                  &_ctrlr, req.get(), reply.get(),
                  google::protobuf::NewCallback(this, &fblock_client::on_response, stack_ptr));
                stack_ptr->resp = std::move(reply);
            },

            [] ([[maybe_unused]] auto& _) {
                SPDK_ERRLOG("ERROR: Un-allocated request\n");
                throw std::runtime_error{"un-allocated request"};
            }
        };

        std::visit(request_handler, head->req);

        return true;
    }

    bool process_response() {
        if (_requests.empty()) {
            return false;
        }

        auto& head = _requests.front();
        if (head->state != request_state::response_acquired) {
            return false;
        }

        auto response_handler = utils::overload {
            [this, stack_ptr = head.get()] (std::unique_ptr<osd::write_reply>& resp) {
                auto& req = std::get<std::unique_ptr<osd::write_request>>(stack_ptr->req);
                SPDK_INFOLOG(
                  libblk,
                  "write_object pool: %lu pg: %lu object: %s offset: %lu done, state: %d\n",
                  req->pool_id(), req->pg_id(), req->object_name().c_str(),
                  req->offset(), resp->state());

                auto cb = std::get<write_object_callback>(stack_ptr->resp_cb);
                cb(stack_ptr->ctx, resp->state());
            },

            [this, stack_ptr = head.get()] (std::unique_ptr<osd::read_reply>& resp) {
                auto& req = std::get<std::unique_ptr<osd::read_request>>(stack_ptr->req);
                SPDK_INFOLOG(
                 libblk,
                   "read_object pool: %lu pg:%lu object:%s offset:%lu done. state:%d data size: %lu\n",
                   req->pool_id(), req->pg_id(), req->object_name().c_str(),
                   req->offset(), resp->state(), resp->data().size());

                auto cb = std::get<read_object_callback>(stack_ptr->resp_cb);
                cb(stack_ptr->ctx, stack_ptr->obj_index, resp->data(), resp->state());
            },

            [this, stack_ptr = head.get()] (std::unique_ptr<osd::delete_reply>& resp) {
                auto& req = std::get<std::unique_ptr<osd::write_request>>(stack_ptr->req);
                SPDK_INFOLOG(
                  libblk,
                  "delete_request pool: %lu pg:%lu object:%s done. state:%d\n",
                  req->pool_id(), req->pg_id(), req->object_name().c_str(), resp->state());
                auto* bdev_io = reinterpret_cast<::spdk_bdev_io*>(stack_ptr->ctx);
                auto cb = std::get<delete_callback>(stack_ptr->resp_cb);
                cb(bdev_io, resp->state());
            },

            [] ([[maybe_unused]] auto& _) {
                SPDK_ERRLOG("ERROR: Un-allocated response\n");
                throw std::runtime_error{"un-allocated response"};
            }
        };

        std::visit(response_handler, head->resp);
        _requests.pop_front();

        return true;
    }

    void on_response(request_stack_type* stack_ptr) noexcept {
        stack_ptr->state = request_state::response_acquired;
    }

    void on_leader_acquired(request_stack_type* stack_ptr) noexcept {
        stack_ptr->state = request_state::leader_osd_acquired;
    }

    void send_request(
      std::shared_ptr<osd::rpc_service_osd_Stub> stub,
      std::unique_ptr<osd::pg_leader_request> leader_req,
      request_type req, response_callback_type cb, void* ctx, uint64_t obj_idx = 0) {
        auto req_stk = std::make_unique<request_stack_type>(stub, std::move(leader_req), std::move(req));
        auto done = google::protobuf::NewCallback(this, &fblock_client::on_leader_acquired, req_stk.get());
        req_stk->stub->process_get_leader(&_ctrlr, req_stk->leader_req.get(), req_stk->leader_resp.get(), done);
        req_stk->resp_cb = cb;
        req_stk->ctx = ctx;
        req_stk->obj_index = obj_idx;

        if (::spdk_env_get_current_core() == _current_core) {
            _requests.push_back(std::move(req_stk));
        } else {
            std::lock_guard<std::mutex> guard(_mutex);
            _requests.push_back(std::move(req_stk));
        }
    }

    int write_object(
      std::string object_name,
      uint64_t offset,
      const std::string &buf,
      uint64_t target_pool_id,
      write_object_callback cb_fn,
      void *source) {
         auto target_pg = calc_target(object_name, target_pool_id);

        SPDK_INFOLOG(
          libblk,
          "write_object pool: %lu pg: %lu object_name: %s offset: %lu length: %lu \n",
          target_pool_id, target_pg, object_name.c_str(), offset, buf.size());

        auto* first_osd = _mon_cli->get_pg_first_available_osd_info(target_pool_id, target_pg);
        if (not first_osd) {
            SPDK_ERRLOG("ERROR: Cant find any available osd of pg %d, pool id %d\n", target_pool_id, target_pg);
            return -1;
        }

        auto leader_req = std::make_unique<osd::pg_leader_request>();
        leader_req->set_pool_id(target_pool_id);
        leader_req->set_pg_id(target_pg);

        auto req = std::make_unique<osd::write_request>();
        req->set_pool_id(target_pool_id);
        req->set_pg_id(target_pg);
        req->set_object_name(object_name);
        req->set_offset(offset);
        req->set_data(buf);

        send_request(get_stub(first_osd), std::move(leader_req), std::move(req), cb_fn, source);

        return 0;
    }

    int read_object(
      std::string object_name,
      uint64_t offset,
      uint64_t length,
      uint64_t target_pool_id,
      read_object_callback cb_fn,
      void *source,
      uint64_t object_idx) {
        auto target_pg = calc_target(object_name, target_pool_id);

        SPDK_INFOLOG(
          libblk,
          "read_object pool: %lu pg:%lu object:%s offset:%lu length: %lu\n",
          target_pool_id, target_pg, object_name.c_str(), offset, length);

        auto* first_osd = _mon_cli->get_pg_first_available_osd_info(target_pool_id, target_pg);
        if (not first_osd) {
            SPDK_ERRLOG("ERROR: Cant find any available osd of pg %d, pool id %d\n", target_pool_id, target_pg);
            return -1;
        }

        auto leader_req = std::make_unique<osd::pg_leader_request>();
        leader_req->set_pool_id(target_pool_id);
        leader_req->set_pg_id(target_pg);

        auto req = std::make_unique<osd::read_request>();
        req->set_pool_id(target_pool_id);
        req->set_pg_id(target_pg);
        req->set_object_name(object_name);
        req->set_offset(offset);
        req->set_length(length);

        send_request(get_stub(first_osd), std::move(leader_req), std::move(req), cb_fn, source, object_idx);

        return 0;
    }

    int delete_object(
      std::string object_name,
      uint64_t target_pool_id,
      delete_callback cb_fn,
      ::spdk_bdev_io *bdev_io) {
        auto target_pg = calc_target(object_name, target_pool_id);

        SPDK_INFOLOG(
          libblk,
          "delete_object pool: %lu pg: %lu object: %s\n",
          target_pool_id, target_pg, object_name.c_str());

        auto* first_osd = _mon_cli->get_pg_first_available_osd_info(target_pool_id, target_pg);
        if (not first_osd) {
            SPDK_ERRLOG("ERROR: Cant find any available osd of pg %d, pool id %d\n", target_pool_id, target_pg);
            return -1;
        }

        auto leader_req = std::make_unique<osd::pg_leader_request>();
        leader_req->set_pool_id(target_pool_id);
        leader_req->set_pg_id(target_pg);

        auto req = std::make_unique<osd::delete_request>();
        req->set_pool_id(target_pool_id);
        req->set_pg_id(target_pg);
        req->set_object_name(object_name);

        send_request(get_stub(first_osd), std::move(leader_req), std::move(req), cb_fn, bdev_io);

        return 0;
    }

private:
    // 计算对象的地址
    unsigned calc_target(const std::string &sstr, uint64_t target_pool_id);
    // 计算pg的掩码
    void calc_pg_masks(uint64_t target_pool_id);

    std::shared_ptr<osd::rpc_service_osd_Stub> get_stub(::osd_info_t *osdinfo);
    std::shared_ptr<osd::rpc_service_osd_Stub> get_stub(const int, const std::string&, const int);

private:
    // For now, we just use one osd and one stub
    connect_cache &_cache;
    std::shared_ptr<osd::rpc_service_osd_Stub> _stub;
    connect_cache::connect_ptr _connect;
    monitor::client* _mon_cli{nullptr};
    uint32_t _pg_mask;
    uint32_t _pg_num;
    utils::simple_poller _poller{};
    std::list<std::unique_ptr<request_stack_type>> _requests{};
    msg::rdma::rpc_controller _ctrlr{};

    ::spdk_thread* _current_thread{::spdk_get_thread()};
    uint32_t _current_core{::spdk_env_get_current_core()};
    std::mutex _mutex{};
};
