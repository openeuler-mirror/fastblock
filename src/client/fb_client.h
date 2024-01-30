/* Copyright (c) 2024 ChinaUnicom
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

#include "monclient/client.h"
#include "msg/rpc_controller.h"
#include "msg/rdma/client.h"
#include "utils/overload.h"
#include "utils/simple_poller.h"
#include "rpc/osd_msg.pb.h"

#include <google/protobuf/stubs/callback.h>

#include <variant>

typedef void (*read_callback)(struct spdk_bdev_io *, char *, uint64_t, int32_t);

typedef void (*write_callback)(struct spdk_bdev_io *, int32_t);

typedef void (*delete_callback)(struct spdk_bdev_io *, int32_t);

typedef void (*read_object_callback)(void *src, uint64_t object_idx, const std::string &data, int32_t state);
typedef void (*write_object_callback)(void *src, int32_t state);

class fblock_client {
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

struct leader_request_stack_type {
    osd::rpc_service_osd_Stub* stub{nullptr};
    std::unique_ptr<osd::pg_leader_request> leader_req{nullptr};
    std::unique_ptr<osd::pg_leader_response> leader_resp{nullptr};
    ::osd_info_t* osd{nullptr};
    uint64_t leader_request_id{};
    fblock_client* this_client{nullptr};
};

struct request_stack_type {
    osd::rpc_service_osd_Stub* stub{nullptr};
    request_type req{std::monostate{}};
    response_type resp{std::monostate{}};
    response_callback_type resp_cb{std::monostate{}};
    uint64_t obj_index{0};
    void* ctx{nullptr};
    bool is_responsed{false};
    uint64_t leader_osd_key{};
    fblock_client* this_client{nullptr};
    bool is_connecting{false};
};

struct leader_key_type {
    int32_t pool_id;
    int32_t pg_id;
};

struct leader_osd_info {
    int32_t leader_id{-1};
    std::string addr{};
    int32_t port{};
    std::chrono::system_clock::time_point epoch{};
    bool is_valid{false};
    bool is_onflight{true};
};

struct connection_id {
    int32_t node_id;
    uint32_t core_no;
};

private:

    static uint64_t to_connection_id(const int32_t node_id, const uint32_t core_no) {
        uint64_t ret{};
        auto* conn_id = reinterpret_cast<connection_id*>(&ret);
        conn_id->node_id = node_id;
        conn_id->core_no = core_no;

        return ret;
    }

    auto get_stub(::osd_info_t *osdinfo) {
        return get_stub(osdinfo->node_id, osdinfo->address, osdinfo->port);
    }

    auto get_stub(leader_osd_info* info) {
        return get_stub(info->leader_id, info->addr, info->port);
    }

    osd::rpc_service_osd_Stub*
    get_stub(const int node_id, std::string addr, const int port) {
        auto conn_id = to_connection_id(node_id, ::spdk_env_get_current_core());
        auto stub_it = _stubs.find(conn_id);
        if (stub_it == _stubs.end()) {
            _stubs.emplace(conn_id, nullptr);
            SPDK_DEBUGLOG(
              libblk,
              "emplaced stubs of node_id %d, current core is %d, conn id %lu, addr is '%s:%d'\n",
               ::spdk_env_get_current_core(), node_id, conn_id, addr.c_str(), port);
            _rpc_client->emplace_connection(
              addr, port,
              [this, conn_id, addr, port] (bool is_connected, std::shared_ptr<msg::rdma::client::connection> conn) {
                  if (not is_connected) {
                      SPDK_ERRLOG("ERROR: Connect to %s:%d failed\n", addr.c_str(), port);
                      throw std::runtime_error{"make connection failed\n"};
                  }

                  auto stub = std::make_unique<osd::rpc_service_osd_Stub>(conn.get());
                  _stubs.at(conn_id) = std::move(stub);
              }
            );

            return nullptr;
        }

        return stub_it->second.get();
    }

    uint64_t make_leader_key(const int32_t pool_id, const int32_t pg_id) noexcept {
        uint64_t leader_osd_key{};
        auto* struct_key = reinterpret_cast<leader_key_type*>(&leader_osd_key);
        struct_key->pg_id = pg_id;
        struct_key->pool_id = pool_id;

        return leader_osd_key;
    }

    leader_key_type from_leader_key(uint64_t key_val) noexcept {
        auto* struct_key = reinterpret_cast<leader_key_type*>(&key_val);
        return { struct_key->pool_id, struct_key->pg_id };
    }

    int enqueue_leader_request(const int32_t pool_id, const int32_t pg_id) {
        auto* first_osd = _mon_cli->get_pg_first_available_osd_info(pool_id, pg_id);
        if (not first_osd) {
            SPDK_ERRLOG("ERROR: Cant find any available osd of pg %d, pool id %d\n", pool_id, pg_id);
            return EAGAIN;
        }

        auto req = std::make_unique<leader_request_stack_type>();
        req->leader_resp = std::make_unique<osd::pg_leader_response>();
        req->leader_req = std::make_unique<osd::pg_leader_request>();
        req->leader_req->set_pool_id(pool_id);
        req->leader_req->set_pg_id(pg_id);
        req->osd = first_osd;
        req->this_client = this;
        req->leader_request_id = _leader_req_id_gen++;
        _leader_requests.emplace(req->leader_request_id, std::move(req));

        return 0;
    }

    void update_leader_state(leader_osd_info* info) {
        if (info->leader_id == -1) {
            return;
        }

        auto* mon_osd_info = _mon_cli->get_osd_info(info->leader_id);
        if (mon_osd_info and mon_osd_info->isin and mon_osd_info->isup) {
            info->is_valid = true;
            return;
        }

        auto last_osd_at = _mon_cli->last_cluster_map_at();
        if (info->epoch > last_osd_at) {
            info->is_valid = true;
            return;
        }

        info->is_valid = false;
    }

    int send_request(
      const int32_t pool_id, const int32_t pg_id,
      request_type req, response_callback_type cb,
      void* ctx, uint64_t obj_idx = 0) {
        auto* req_stk = new request_stack_type{};
        req_stk->req = std::move(req);
        req_stk->resp_cb = cb;
        req_stk->ctx = ctx;
        req_stk->obj_index = obj_idx;
        req_stk->leader_osd_key = make_leader_key(pool_id, pg_id);
        req_stk->this_client = this;
        SPDK_DEBUGLOG(
          libblk,
          "pool_id: %d, pg_id: %d, leader_osd_key: %lu\n",
          pool_id, pg_id, req_stk->leader_osd_key);
        ::spdk_thread_send_msg(_current_thread, fblock_client::do_send_request, req_stk);
        return 0;
}

public:

    fblock_client(monitor::client* mon_cli, ::spdk_cpuset* cpumask, std::shared_ptr<msg::rdma::client::options> opts)
      : _rpc_client{std::make_shared<msg::rdma::client>("fblock_client", cpumask, opts)}
      , _mon_cli{mon_cli} {}

    ~fblock_client() noexcept {
        SPDK_DEBUGLOG(libblk, "call ~fblck_client()\n");
    }

    bool is_terminate() noexcept {
        return _is_terminate;
    }

    bool is_ready() noexcept {
        return _rpc_client->is_start();
    }

    void start() {
        if (not _current_thread) {
            SPDK_ERRLOG("ERROR: Cant get current spdk thread\n");
            throw std::runtime_error{"cant get current spdk thread"};
        }

        SPDK_DEBUGLOG(libblk, "sending start message\n");
        ::spdk_thread_send_msg(_current_thread, do_start, this);
    }

    void stop() noexcept {
        if (_is_terminate) {
            return;
        }

        _is_terminate = true;
        _rpc_client->stop();
        _poller.unregister();
    }

    /************************************************************
     * spdk_thread_send_msg callbacks
     ************************************************************/

    static void do_start(void* arg) {
        auto* arg_this = reinterpret_cast<fblock_client*>(arg);
        arg_this->handle_start();
    }

    static int do_poll(void* arg) {
        auto* arg_this = reinterpret_cast<fblock_client*>(arg);
        if (arg_this->is_terminate()) {
            return SPDK_POLLER_IDLE;
        }
        if (not arg_this->is_ready()) {
            return SPDK_POLLER_IDLE;
        }
        auto is_busy = arg_this->process_leader_request() | arg_this->process_request() | arg_this->process_response();

        return is_busy ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
    }

    static void do_send_request(void* arg) {
        auto* stack_ptr = reinterpret_cast<request_stack_type*>(arg);
        stack_ptr->this_client->handle_send_request(stack_ptr);
    }

    /************************************************************
     * spdk_thread_send_msg callbacks' callbacks
     ************************************************************/

    void handle_start() {
        SPDK_INFOLOG(libblk, "Starting block client...\n");
        _rpc_client->start();
        _poller.poller = SPDK_POLLER_REGISTER(do_poll, this, 0);
    }

    void handle_send_request(request_stack_type* req_stk) {
        auto it = _leader_osd.find(req_stk->leader_osd_key);
        bool should_acquire_leader{false};
        if (it == _leader_osd.end()) {
            SPDK_DEBUGLOG(libblk, "cant find the leader osd %ld\n", req_stk->leader_osd_key);
            should_acquire_leader = true;
        } else if (it->second->is_onflight) {
            SPDK_DEBUGLOG(libblk, "leader osd %ld request on flight\n", req_stk->leader_osd_key);
            should_acquire_leader = false;
        } else if (not it->second->is_valid) {
            SPDK_DEBUGLOG(libblk, "leader osd %ld is not valid\n", req_stk->leader_osd_key);
            should_acquire_leader = true;
        }

        SPDK_DEBUGLOG(
          libblk,
          "leader_osd_key: %lu, should_acquire_leader: %d\n",
          req_stk->leader_osd_key, should_acquire_leader);

        if (should_acquire_leader) {
            _leader_osd.emplace(req_stk->leader_osd_key, std::make_unique<leader_osd_info>());
            SPDK_DEBUGLOG(libblk, "_leader_osd emplaced %lu\n", req_stk->leader_osd_key);
            auto struct_key = from_leader_key(req_stk->leader_osd_key);
            enqueue_leader_request(struct_key.pool_id, struct_key.pg_id);
        } else if (it->second and not it->second->is_onflight) {
            req_stk->stub = get_stub(it->second->leader_id, it->second->addr, it->second->port);
        }

        _requests.emplace_back(req_stk);
    }

    /************************************************************
     * poller fns
     ************************************************************/

    bool process_response() {
        if (_on_flight_requests.empty()) {
            return false;
        }

        auto& head = _on_flight_requests.front();
        if (not head->is_responsed) {
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
        _on_flight_requests.pop_front();

        return true;
    }

    bool process_leader_request() {
        if (_leader_requests.empty()) {
            return false;
        }

        auto n = std::erase_if(
          _leader_requests,
          [this] (auto& kv) {
              auto& [_, stack_ptr] = kv;
              stack_ptr->stub = get_stub(stack_ptr->osd);
              if (not stack_ptr->stub) {
                  return false;
              }

              auto done = google::protobuf::NewCallback(
                this, &fblock_client::on_leader_acquired, stack_ptr.get());
              stack_ptr->stub->process_get_leader(
                &_ctrlr, stack_ptr->leader_req.get(), stack_ptr->leader_resp.get(), done);
              _on_flight_leader_requests.insert(std::move(kv));
              return true;
          }
        );

        return true;
    }

    bool process_request() {
        if (_requests.empty()) {
            return false;
        }

        auto& head = _requests.front();
        auto osd_info_it = _leader_osd.find(head->leader_osd_key);
        if (osd_info_it == _leader_osd.end()) {
            return false;
        }

        if (not head->stub) {
            if (osd_info_it->second->is_onflight) {
                return false;
            }

            SPDK_DEBUGLOG(
              libblk,
              "head->leader_osd_key: %lu, leader_id: %u, addr: '%s:%d' is_onflight: %d, is_valid: %d\n",
              head->leader_osd_key,
              osd_info_it->second->leader_id,
              osd_info_it->second->addr.c_str(),
              osd_info_it->second->port,
              osd_info_it->second->is_onflight,
              osd_info_it->second->port);

            head->stub = get_stub(
              osd_info_it->second->leader_id,
              osd_info_it->second->addr,
              osd_info_it->second->port);
            if (not head->stub) {
                return false;
            }
        }

        update_leader_state(osd_info_it->second.get());
        if (not osd_info_it->second->is_valid or osd_info_it->second->is_onflight) {
            auto* struct_key = reinterpret_cast<leader_key_type*>(&(head->leader_osd_key));
            enqueue_leader_request(struct_key->pool_id, struct_key->pg_id);

            return true;
        }

        auto request_handler = utils::overload {
            [this, stack_ptr = head.get()] (std::unique_ptr<osd::write_request>& req) {
                auto reply = std::make_unique<osd::write_reply>();
                stack_ptr->stub->process_write(
                  &_ctrlr, req.get(), reply.get(),
                  google::protobuf::NewCallback(this, &fblock_client::on_response, stack_ptr));
                stack_ptr->resp = std::move(reply);
            },

            [this, stack_ptr = head.get()] (std::unique_ptr<osd::read_request>& req) {
                auto reply = std::make_unique<osd::read_reply>();
                stack_ptr->stub->process_read(
                  &_ctrlr, req.get(), reply.get(),
                  google::protobuf::NewCallback(this, &fblock_client::on_response, stack_ptr));
                stack_ptr->resp = std::move(reply);
            },

            [this, stack_ptr = head.get()] (std::unique_ptr<osd::delete_request>& req) {
                auto reply = std::make_unique<osd::delete_reply>();
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
        _on_flight_requests.push_back(std::move(head));
        _requests.pop_front();

        return true;
    }

private:

    void on_leader_acquired(leader_request_stack_type* stack_ptr) {
        auto it = _on_flight_leader_requests.find(stack_ptr->leader_request_id);
        if (it == _on_flight_leader_requests.end()) {
            SPDK_ERRLOG("Cant find the leader request stack of id %ld\n", stack_ptr->leader_request_id);
            throw std::runtime_error{"cant find the leader request stack"};
        }

        auto leader_key = make_leader_key(
          it->second->leader_req->pool_id(),
          it->second->leader_req->pg_id());
        auto info_it = _leader_osd.find(leader_key);
        if (info_it == _leader_osd.end()) {
            SPDK_ERRLOG(
              "ERROR: Cant find the leader osd info record of pool id %ld, pg id %ld\n",
              it->second->leader_req->pool_id(), it->second->leader_req->pg_id());

            throw std::runtime_error{"Cant find the leader osd info record"};
        }
        auto* osd_info = info_it->second.get();
        SPDK_INFOLOG(libblk, "Got leader osd %ld, leader_key is %lu\n", stack_ptr->leader_request_id, leader_key);
        osd_info->is_onflight = false;
        auto* resp = it->second->leader_resp.get();
        osd_info->epoch = std::chrono::system_clock::now();
        osd_info->leader_id = resp->leader_id();
        osd_info->addr = resp->leader_addr();
        osd_info->port = resp->leader_port();
        SPDK_DEBUGLOG(libblk, "leader osd address: '%s:%d'\n", osd_info->addr.c_str(), osd_info->port);

        update_leader_state(info_it->second.get());
        if (not info_it->second->is_valid) {
            SPDK_NOTICELOG(
              "Got a invalid leader osd(%s:%d) with id %d, will retry\n",
              resp->leader_addr().c_str(),
              resp->leader_port(),
              resp->leader_id());

            auto done = google::protobuf::NewCallback(
              this, &fblock_client::on_leader_acquired, stack_ptr);
            stack_ptr->stub->process_get_leader(
              &_ctrlr, stack_ptr->leader_req.get(), stack_ptr->leader_resp.get(), done);

            return;
        }

        _on_flight_leader_requests.erase(it);
    }

    void on_response(request_stack_type* stack_ptr) noexcept {
        stack_ptr->is_responsed = true;
    }

public:

    int write_object(
      std::string object_name,
      uint64_t offset,
      const std::string &buf,
      int32_t target_pool_id,
      write_object_callback cb_fn,
      void *source) {
        auto target_pg = calc_target(object_name, target_pool_id);

        auto req = std::make_unique<osd::write_request>();
        req->set_pool_id(target_pool_id);
        req->set_pg_id(target_pg);
        req->set_object_name(object_name);
        req->set_offset(offset);
        req->set_data(buf);
        send_request(target_pool_id, target_pg, std::move(req), cb_fn, source);

        SPDK_INFOLOG(
          libblk,
          "write_object pool: %u pg: %u object_name: %s offset: %lu length: %lu \n",
          target_pool_id, target_pg, object_name.c_str(), offset, buf.size());

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
          "read_object pool: %lu pg: %u object: %s offset: %lu length: %lu\n",
          target_pool_id, target_pg, object_name.c_str(), offset, length);

        auto req = std::make_unique<osd::read_request>();
        req->set_pool_id(target_pool_id);
        req->set_pg_id(target_pg);
        req->set_object_name(object_name);
        req->set_offset(offset);
        req->set_length(length);

        send_request(target_pool_id, target_pg, std::move(req), cb_fn, source, object_idx);

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
          "delete_object pool: %lu pg: %u object: %s\n",
          target_pool_id, target_pg, object_name.c_str());

        auto req = std::make_unique<osd::delete_request>();
        req->set_pool_id(target_pool_id);
        req->set_pg_id(target_pg);
        req->set_object_name(object_name);

        send_request(target_pool_id, target_pg, std::move(req), cb_fn, bdev_io);

        return 0;
    }

private:
    // 计算对象的地址
    unsigned calc_target(const std::string &sstr, int32_t target_pool_id);
    // 计算pg的掩码
    void calc_pg_masks(int32_t target_pool_id);

private:
    std::shared_ptr<msg::rdma::client> _rpc_client{nullptr};
    std::unordered_map<uint64_t, std::unique_ptr<osd::rpc_service_osd_Stub>> _stubs{};
    monitor::client* _mon_cli{nullptr};
    uint32_t _pg_mask;
    uint32_t _pg_num;
    utils::simple_poller _poller{};

    uint64_t _leader_req_id_gen{0};
    std::unordered_map<uint64_t, std::unique_ptr<leader_request_stack_type>> _leader_requests{}; // leader 请求可以不考虑保序性
    std::unordered_map<uint64_t, std::unique_ptr<leader_request_stack_type>> _on_flight_leader_requests{};
    std::list<std::unique_ptr<request_stack_type>> _requests{};
    std::list<std::unique_ptr<request_stack_type>> _on_flight_requests{};
    msg::rdma::rpc_controller _ctrlr{};

    ::spdk_thread* _current_thread{::spdk_get_thread()};
    uint32_t _current_core{::spdk_env_get_current_core()};

    std::unordered_map<uint64_t, std::unique_ptr<leader_osd_info>> _leader_osd{};

    int32_t _io_queue_size{128};
    int32_t _io_queue_request{1024};
    bool _is_terminate{false};
};
