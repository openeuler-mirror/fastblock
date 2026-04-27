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

#include "fastblock/monclient/client.h"
#include "fastblock/msg/rpc_controller.h"
#include "fastblock/msg/rdma/client.h"
#include "fastblock/utils/overload.h"
#include "fastblock/utils/simple_poller.h"
#include "fastblock/utils/err_num.h"

#include "fastblock/rpc/osd_msg.pb.h"

#include <infiniband/verbs.h>

#include <spdk/env.h>

#include <cstdlib>
#include <cerrno>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
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
        utils::osd_info_t* osd{nullptr};
        uint64_t leader_request_id{};
        fblock_client* this_client{nullptr};
    };

    struct write_ring_state;

    struct request_stack_type {
        struct ring_write_context {
            std::shared_ptr<write_ring_state> ring_state{nullptr};
            std::unique_ptr<osd::commit_ring_write_request> commit_req{nullptr};
            uint32_t slot_index{0};
            void* data{nullptr};
            ::ibv_mr* mr{nullptr};

            ~ring_write_context() noexcept {
                if (mr) {
                    ::ibv_dereg_mr(mr);
                }
                if (data) {
                    ::spdk_free(data);
                }
            }
        };

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
        std::unique_ptr<ring_write_context> ring_ctx{nullptr};
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
        uint32_t port;
    };

    struct write_ring_slot_info {
        uint64_t remote_addr{0};
        uint32_t remote_key{0};
        uint32_t slot_size{0};
        bool busy{false};
    };

    struct write_ring_state {
        std::shared_ptr<msg::rdma::client::connection> conn{nullptr};
        std::unique_ptr<msg::rdma::rpc_controller> ctrlr{std::make_unique<msg::rdma::rpc_controller>()};
        std::unique_ptr<osd::acquire_write_ring_request> req{nullptr};
        std::unique_ptr<osd::acquire_write_ring_response> resp{nullptr};
        uint64_t queue_id{0};
        uint64_t lease_us{0};
        uint32_t next_slot{0};
        bool is_ready{false};
        bool is_onflight{false};
        std::vector<write_ring_slot_info> slots{};
    };

    struct stop_context {
        fblock_client* this_client{nullptr};
        std::optional<std::function<void()>> cb{std::nullopt};
    };

    struct start_context {
        fblock_client* this_client{nullptr};
        std::function<void()> cb{};
    };

private:

    static uint64_t to_connection_id(const int32_t node_id, const int port) {
        uint64_t ret{};
        auto* conn_id = reinterpret_cast<connection_id*>(&ret);
        conn_id->node_id = node_id;
        conn_id->port = port;

        return ret;
    }

    void print_osd(utils::osd_info_t *osdinfo){
        SPDK_DEBUGLOG(libblk, "osd id: %d\n", osdinfo->node_id);
        for(auto &[_, shard_port] : osdinfo->sharded_ports){
            SPDK_DEBUGLOG(libblk, "  core: %u shard: %u, port: %u\n", shard_port.core_id, shard_port.shard_id, shard_port.port);
        }
    }

    auto get_stub(utils::osd_info_t *osdinfo) {
        auto shard_id = utils::get_current_shard_id() % osdinfo->sharded_ports.size();
        SPDK_DEBUGLOG(libblk, "currentcore: %u, first core: %u, shard_id: %lu\n", ::spdk_env_get_current_core(),
                ::spdk_env_get_first_core(), shard_id);
        // print_osd(osdinfo);
        return get_stub(osdinfo->node_id, osdinfo->address, osdinfo->sharded_ports.at(shard_id).port);
    }

    auto get_stub(leader_osd_info* info) {
        return get_stub(info->leader_id, info->addr, info->port);
    }

    static constexpr uint32_t default_write_ring_slot_count{16};
    static constexpr uint32_t default_write_ring_slot_size{256 * 1024};

    static bool should_use_write_ring() noexcept {
        auto* disable = std::getenv("FASTBLOCK_DISABLE_RING_WRITE");
        return !(disable && disable[0] != '\0' && disable[0] != '0');
    }

    std::shared_ptr<write_ring_state> get_write_ring_state(const int node_id, const int port) {
        auto conn_id = to_connection_id(node_id, port);
        auto it = _write_rings.find(conn_id);
        if (it == _write_rings.end()) {
            return nullptr;
        }

        return it->second;
    }

    void on_write_ring_ready(const uint64_t conn_id) {
        auto it = _write_rings.find(conn_id);
        if (it == _write_rings.end()) {
            return;
        }

        auto& state = it->second;
        state->is_onflight = false;
        if (state->resp->state() != err::E_SUCCESS) {
            SPDK_ERRLOG("acquire write ring failed, state %d\n", state->resp->state());
            return;
        }

        state->queue_id = state->resp->queue_id();
        state->lease_us = state->resp->lease_us();
        state->slots.clear();
        state->slots.reserve(state->resp->slots_size());
        for (int i = 0; i < state->resp->slots_size(); ++i) {
            auto& slot = state->resp->slots(i);
            state->slots.push_back(write_ring_slot_info{
              .remote_addr = slot.remote_addr(),
              .remote_key = slot.remote_key(),
              .slot_size = slot.slot_size(),
              .busy = false,
            });
        }
        state->is_ready = !state->slots.empty();
        SPDK_INFOLOG(
          libblk,
          "write ring ready, conn id %lu, queue id %lu, slot count %lu\n",
          conn_id, state->queue_id, state->slots.size());
    }

    void acquire_write_ring_async(
      const uint64_t conn_id,
      osd::rpc_service_osd_Stub* stub,
      std::shared_ptr<write_ring_state> ring_state) {
        if (ring_state->is_ready || ring_state->is_onflight || not stub) {
            return;
        }

        ring_state->req = std::make_unique<osd::acquire_write_ring_request>();
        ring_state->resp = std::make_unique<osd::acquire_write_ring_response>();
        ring_state->req->set_slot_count(default_write_ring_slot_count);
        ring_state->req->set_slot_size(default_write_ring_slot_size);
        ring_state->req->set_lease_us(30ULL * 1000ULL * 1000ULL);
        ring_state->is_onflight = true;
        stub->process_acquire_write_ring(
          ring_state->ctrlr.get(),
          ring_state->req.get(),
          ring_state->resp.get(),
          google::protobuf::NewCallback(this, &fblock_client::on_write_ring_ready, conn_id));
    }

    std::optional<uint32_t> acquire_write_ring_slot(write_ring_state* state) {
        if (!state || !state->is_ready || state->slots.empty()) {
            return std::nullopt;
        }

        for (size_t i = 0; i < state->slots.size(); ++i) {
            auto idx = (state->next_slot + i) % state->slots.size();
            if (!state->slots[idx].busy) {
                state->slots[idx].busy = true;
                state->next_slot = static_cast<uint32_t>((idx + 1) % state->slots.size());
                return static_cast<uint32_t>(idx);
            }
        }

        return std::nullopt;
    }

    bool post_ring_write(
      request_stack_type* stack_ptr,
      std::shared_ptr<write_ring_state> ring_state,
      osd::write_request* write_req) {
        if (!ring_state || !ring_state->is_ready || !ring_state->conn) {
            return false;
        }

        auto slot_idx = acquire_write_ring_slot(ring_state.get());
        if (!slot_idx.has_value()) {
            return false;
        }

        const auto serialized_size = write_req->ByteSizeLong();
        auto& remote_slot = ring_state->slots[*slot_idx];
        if (serialized_size > remote_slot.slot_size) {
            remote_slot.busy = false;
            return false;
        }

        auto alloc_size = utils::align_up<uint64_t>(serialized_size, 4096);
        auto* data = ::spdk_zmalloc(
          alloc_size, 0x1000, nullptr, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
        if (!data) {
            remote_slot.busy = false;
            return false;
        }

        if (!write_req->SerializeToArray(data, serialized_size)) {
            remote_slot.busy = false;
            ::spdk_free(data);
            return false;
        }

        auto* mr = ::ibv_reg_mr(
          ring_state->conn->fd().pd(),
          data,
          alloc_size,
          IBV_ACCESS_LOCAL_WRITE);
        if (!mr) {
            remote_slot.busy = false;
            ::spdk_free(data);
            return false;
        }

        auto ring_ctx = std::make_unique<request_stack_type::ring_write_context>();
        ring_ctx->ring_state = ring_state;
        ring_ctx->slot_index = *slot_idx;
        ring_ctx->data = data;
        ring_ctx->mr = mr;
        ring_ctx->commit_req = std::make_unique<osd::commit_ring_write_request>();
        ring_ctx->commit_req->set_queue_id(ring_state->queue_id);
        ring_ctx->commit_req->set_slot_index(*slot_idx);
        ring_ctx->commit_req->set_serialized_size(static_cast<uint32_t>(serialized_size));

        ::ibv_sge sge{};
        sge.addr = reinterpret_cast<uint64_t>(mr->addr);
        sge.length = static_cast<uint32_t>(serialized_size);
        sge.lkey = mr->lkey;

        ::ibv_send_wr wr{};
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.wr.rdma.remote_addr = remote_slot.remote_addr;
        wr.wr.rdma.rkey = remote_slot.remote_key;
        SPDK_NOTICELOG(
          "posting ring rdma write, queue id %lu, slot %u, bytes %lu, remote addr 0x%lx, rkey %u\n",
          ring_state->queue_id,
          *slot_idx,
          serialized_size,
          remote_slot.remote_addr,
          remote_slot.remote_key);
        auto reply = std::make_unique<osd::write_reply>();
        stack_ptr->ring_ctx = std::move(ring_ctx);
        stack_ptr->resp = std::move(reply);

        auto err = ring_state->conn->post_external_send_wr(
          &wr,
          [this, stack_ptr]() {
              auto& reply_ref = std::get<std::unique_ptr<osd::write_reply>>(stack_ptr->resp);
              stack_ptr->stub->process_commit_ring_write(
                &_ctrlr,
                stack_ptr->ring_ctx->commit_req.get(),
                reply_ref.get(),
                google::protobuf::NewCallback(this, &fblock_client::on_response, stack_ptr));
          });
        if (err) {
            SPDK_ERRLOG("post rdma write failed: %s\n", err->err.message().c_str());
            remote_slot.busy = false;
            stack_ptr->ring_ctx.reset();
            stack_ptr->resp = std::monostate{};
            return false;
        }
        return true;
    }

    osd::rpc_service_osd_Stub*
    get_stub(const int node_id, std::string addr, const int port) {
        auto conn_id = to_connection_id(node_id, port);
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
                  auto ring_state = std::make_shared<write_ring_state>();
                  ring_state->conn = std::move(conn);
                  _write_rings[conn_id] = ring_state;
                  acquire_write_ring_async(conn_id, _stubs.at(conn_id).get(), ring_state);
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
            if(!_mon_cli->pool_is_exist(pool_id)){
                return err::ERR_NOT_FOUND_POOL;
            }
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
        if(spdk_get_thread() == _current_thread){
            fblock_client::do_send_request(req_stk);
        } else {
            ::spdk_thread_send_msg(_current_thread, fblock_client::do_send_request, req_stk);
        }
        return 0;
}

public:

    fblock_client(monitor::client* mon_cli, ::spdk_thread* thd, std::shared_ptr<msg::rdma::client::options> opts)
      : _rpc_client{std::make_shared<msg::rdma::client>(FB_FMT_1("fblock_%1%", utils::random_string(3)), thd, opts)}
      , _mon_cli{mon_cli}
      , _current_thread{thd} {
        _leader_poller.set_thread(_current_thread);
        _request_poller.set_thread(_current_thread);
        _response_poller.set_thread(_current_thread);
    }

    ~fblock_client() noexcept {
        SPDK_DEBUGLOG(libblk, "call ~fblck_client()\n");
    }

    bool is_terminate() noexcept {
        return _is_terminate;
    }

    bool is_ready() noexcept {
        return _rpc_client->is_start();
    }

    void start(std::function<void()> cb) {
        if (not _current_thread) {
            SPDK_ERRLOG("ERROR: Cant get current spdk thread\n");
            throw std::runtime_error{"cant get current spdk thread"};
        }

        SPDK_DEBUGLOG(libblk, "sending start message\n");
        auto* ctx = new start_context{this, std::move(cb)};
        ::spdk_thread_send_msg(_current_thread, do_start, ctx);
    }

    void stop(std::optional<std::function<void()>>&& cb = std::nullopt) noexcept {
        if (_is_terminate) {
            return;
        }

        auto* ctx = new stop_context{this, std::move(cb)};
        ::spdk_thread_send_msg(_current_thread, do_stop, ctx);
    }

    /************************************************************
     * spdk_thread_send_msg callbacks
     ************************************************************/

    static void do_start(void* arg) {
        auto* ctx = reinterpret_cast<start_context*>(arg);
        ctx->this_client->handle_start(ctx->cb);
        delete ctx;
    }

    static int poll_leader(void* arg) {
        auto* arg_this = reinterpret_cast<fblock_client*>(arg);
        if (arg_this->is_terminate()) {
            return SPDK_POLLER_IDLE;
        }
        if (not arg_this->is_ready()) {
            return SPDK_POLLER_IDLE;
        }

        return arg_this->process_leader_request();
    }

    static int poll_request(void* arg) {
        auto* arg_this = reinterpret_cast<fblock_client*>(arg);
        if (arg_this->is_terminate()) {
            return SPDK_POLLER_IDLE;
        }
        if (not arg_this->is_ready()) {
            return SPDK_POLLER_IDLE;
        }

        return arg_this->process_request();
    }

    static int poll_response(void* arg) {
        auto* arg_this = reinterpret_cast<fblock_client*>(arg);
        if (arg_this->is_terminate()) {
            return SPDK_POLLER_IDLE;
        }
        if (not arg_this->is_ready()) {
            return SPDK_POLLER_IDLE;
        }
        return arg_this->process_response();
    }

    static void do_send_request(void* arg) {
        auto* stack_ptr = reinterpret_cast<request_stack_type*>(arg);
        stack_ptr->this_client->handle_send_request(stack_ptr);
    }

    static void do_stop(void* arg) {
        auto* ctx = reinterpret_cast<stop_context*>(arg);
        ctx->this_client->handle_stop(ctx);
    }

    /************************************************************
     * spdk_thread_send_msg callbacks' callbacks
     ************************************************************/

    void handle_start(std::function<void()> cb) {
        SPDK_INFOLOG(libblk, "Starting block client...\n");
        _rpc_client->start();
        _leader_poller.register_poller(poll_leader, this, 0, "fbcli_leader");
        _request_poller.register_poller(poll_request, this, 0, "fbcli_request");
        _response_poller.register_poller(poll_response, this, 0, "fbcli_response");
        cb();
    }

    void handle_stop(stop_context* ctx) {
        SPDK_INFOLOG(libblk, "Stop the fastblock client\n");
        _is_terminate = true;

        _rpc_client->stop([this, ctx = ctx] () mutable {
            SPDK_INFOLOG(libblk, "rpc client has been stopped\n");
            _leader_poller.unregister_poller([this, ctx = ctx] () mutable {
                SPDK_INFOLOG(libblk, "leader poller has been stopped\n");
                _request_poller.unregister_poller([this, ctx = ctx] () mutable {
                    SPDK_INFOLOG(libblk, "request poller has been stopped\n");
                    _response_poller.unregister_poller([this, ctx = ctx] () mutable {
                        SPDK_INFOLOG(libblk, "response poller has been stopped\n");
                        if (ctx->cb) {
                            try {
                                (ctx->cb.value())();
                            } catch (const std::exception& e) {
                                SPDK_ERRLOG("stop the fb_client error: %s\n", e.what());
                            }
                        }
                        delete ctx;
                    });
                });
            });
        });
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
            if(enqueue_leader_request(struct_key.pool_id, struct_key.pg_id) == err::ERR_NOT_FOUND_POOL){
                SPDK_ERRLOG("pool %d does not exist.\n", struct_key.pool_id);

                auto cb_handler = utils::overload {
                    [this, req_stk] (write_object_callback &cb) {
                        cb(req_stk->ctx, err::ERR_NOT_FOUND_POOL);
                    },

                    [this, req_stk] (read_object_callback &cb) {
                        cb(req_stk->ctx, req_stk->obj_index, std::string{}, err::ERR_NOT_FOUND_POOL);
                    },

                    [this, req_stk] (delete_callback &cb) {
                        auto* bdev_io = reinterpret_cast<::spdk_bdev_io*>(req_stk->ctx);
                        cb(bdev_io, err::ERR_NOT_FOUND_POOL);
                    },

                    [] ([[maybe_unused]] auto& _) {
                        SPDK_ERRLOG("ERROR: Un-allocated response\n");
                        throw std::runtime_error{"un-allocated response"};
                    }                    
                };
                std::visit(cb_handler, req_stk->resp_cb);

                // req_stk->resp_cb(req_stk->ctx, err::ERR_NOT_FOUND_POOL);
                _leader_osd.erase(req_stk->leader_osd_key);
                delete req_stk;
                return;
            }
        } else if (it->second and not it->second->is_onflight) {
            req_stk->stub = get_stub(it->second->leader_id, it->second->addr, it->second->port);
        }

        _requests.emplace_back(req_stk);
    }

    /************************************************************
     * poller fns
     ************************************************************/

    int process_response() {
        if (_on_flight_requests.empty()) {
            return SPDK_POLLER_IDLE;
        }

        auto& head = _on_flight_requests.front();
        if (not head->is_responsed) {
            return SPDK_POLLER_IDLE;
        }

        auto response_handler = utils::overload {
            [this, stack_ptr = head.get()] (std::unique_ptr<osd::write_reply>& resp) {
                auto& req = std::get<std::unique_ptr<osd::write_request>>(stack_ptr->req);
                if (stack_ptr->ring_ctx && stack_ptr->ring_ctx->ring_state) {
                    if (resp->state() == -ENOENT || resp->state() == -EINVAL) {
                        stack_ptr->ring_ctx->ring_state->is_ready = false;
                        stack_ptr->ring_ctx->ring_state->queue_id = 0;
                    }
                    auto slot_idx = stack_ptr->ring_ctx->slot_index;
                    if (slot_idx < stack_ptr->ring_ctx->ring_state->slots.size()) {
                        stack_ptr->ring_ctx->ring_state->slots[slot_idx].busy = false;
                    }
                }
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

        return SPDK_POLLER_BUSY;
    }

    int process_leader_request() {
        if (_leader_requests.empty()) {
            return SPDK_POLLER_IDLE;
        }

        auto n = std::erase_if(
          _leader_requests,
          [this] (auto& kv) {
              auto& [_, stack_ptr] = kv;
              stack_ptr->stub = get_stub(stack_ptr->osd);
              if (not stack_ptr->stub) {
                  return SPDK_POLLER_IDLE;
              }

              SPDK_INFOLOG(libblk, "send get_leader request for pg %lu.%lu to osd %d\n",
                      stack_ptr->leader_req->pool_id(), stack_ptr->leader_req->pg_id(), stack_ptr->osd->node_id);
              auto done = google::protobuf::NewCallback(
                this, &fblock_client::on_leader_acquired, stack_ptr.get());
              stack_ptr->stub->process_get_leader(
                &_ctrlr, stack_ptr->leader_req.get(), stack_ptr->leader_resp.get(), done);
              _on_flight_leader_requests.insert(std::move(kv));
              return SPDK_POLLER_BUSY;
          }
        );

        return SPDK_POLLER_BUSY;
    }

    int process_request() {
        if (_requests.empty()) {
            return SPDK_POLLER_IDLE;
        }

        auto& head = _requests.front();
        auto osd_info_it = _leader_osd.find(head->leader_osd_key);
        if (osd_info_it == _leader_osd.end()) {
            return SPDK_POLLER_IDLE;
        }

        if (not head->stub) {
            if (osd_info_it->second->is_onflight) {
                return SPDK_POLLER_IDLE;
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
                return SPDK_POLLER_IDLE;
            }
        }

        update_leader_state(osd_info_it->second.get());
        if (not osd_info_it->second->is_valid or osd_info_it->second->is_onflight) {
            auto* struct_key = reinterpret_cast<leader_key_type*>(&(head->leader_osd_key));
            if(enqueue_leader_request(struct_key->pool_id, struct_key->pg_id) == err::ERR_NOT_FOUND_POOL){
                SPDK_ERRLOG("pool %d does not exist.\n", struct_key->pool_id);

                auto cb_handler = utils::overload {
                    [this, stack_ptr = head.get()] (write_object_callback &cb) {
                        cb(stack_ptr->ctx, err::ERR_NOT_FOUND_POOL);
                    },

                    [this, stack_ptr = head.get()] (read_object_callback &cb) {
                        cb(stack_ptr->ctx, stack_ptr->obj_index, std::string{}, err::ERR_NOT_FOUND_POOL);
                    },

                    [this, stack_ptr = head.get()] (delete_callback &cb) {
                        auto* bdev_io = reinterpret_cast<::spdk_bdev_io*>(stack_ptr->ctx);
                        cb(bdev_io, err::ERR_NOT_FOUND_POOL);
                    },

                    [] ([[maybe_unused]] auto& _) {
                        SPDK_ERRLOG("ERROR: Un-allocated response\n");
                        throw std::runtime_error{"un-allocated response"};
                    }                    
                };
                std::visit(cb_handler, head->resp_cb);
                _requests.pop_front();
            }

            return SPDK_POLLER_BUSY;
        }

        auto request_handler = utils::overload {
            [this, stack_ptr = head.get(), leader_id = osd_info_it->second->leader_id, leader_port = osd_info_it->second->port] (std::unique_ptr<osd::write_request>& req) {
                if (should_use_write_ring()) {
                    auto conn_id = to_connection_id(
                      static_cast<int32_t>(leader_id),
                      leader_port);
                    auto ring_state = get_write_ring_state(
                      static_cast<int32_t>(leader_id),
                      leader_port);
                    if (ring_state && !ring_state->is_ready && !ring_state->is_onflight) {
                        acquire_write_ring_async(conn_id, stack_ptr->stub, ring_state);
                    }
                    if (ring_state && ring_state->is_ready && post_ring_write(stack_ptr, ring_state, req.get())) {
                        return;
                    }
                }

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

        return SPDK_POLLER_BUSY;
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
        SPDK_DEBUGLOG(libblk, "Got leader osd of pg %lu.%lu: osd id %d osd address: '%s:%d'\n",
                it->second->leader_req->pool_id(), it->second->leader_req->pg_id(), osd_info->leader_id,
                osd_info->addr.c_str(), osd_info->port);

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

    spdk_thread* get_current_thread() {
        return _current_thread;
    }
private:
    // 计算对象的地址
    unsigned calc_target(const std::string &sstr, int32_t target_pool_id);
    // 计算pg的掩码
    void calc_pg_masks(int32_t target_pool_id);

private:
    std::shared_ptr<msg::rdma::client> _rpc_client{nullptr};
    std::unordered_map<uint64_t, std::unique_ptr<osd::rpc_service_osd_Stub>> _stubs{};
    std::unordered_map<uint64_t, std::shared_ptr<write_ring_state>> _write_rings{};
    monitor::client* _mon_cli{nullptr};
    uint32_t _pg_mask;
    uint32_t _pg_num;
    utils::simple_poller _leader_poller{};
    utils::simple_poller _request_poller{};
    utils::simple_poller _response_poller{};

    uint64_t _leader_req_id_gen{0};
    std::unordered_map<uint64_t, std::unique_ptr<leader_request_stack_type>> _leader_requests{}; // leader 请求可以不考虑保序性
    std::unordered_map<uint64_t, std::unique_ptr<leader_request_stack_type>> _on_flight_leader_requests{};
    std::list<std::unique_ptr<request_stack_type>> _requests{};
    std::list<std::unique_ptr<request_stack_type>> _on_flight_requests{};
    msg::rdma::rpc_controller _ctrlr{};

    ::spdk_thread* _current_thread{};

    std::unordered_map<uint64_t, std::unique_ptr<leader_osd_info>> _leader_osd{};

    int32_t _io_queue_size{128};
    int32_t _io_queue_request{1024};
    bool _is_terminate{false};
};
