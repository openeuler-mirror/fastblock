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

#include "base/core_sharded.h"
#include "client/pg_client.h"
#include "client/types.h"
#include "monclient/client.h"
#include "msg/rdma/client.h"
#include "rpc/osd_msg.pb.h"

namespace fastblock {
namespace client {

class connection_pool {

private:

    struct osd_stub {
        std::unique_ptr<osd::rpc_service_osd_Stub> stub{nullptr};
        leader_osd_info* osd_info{nullptr};
    };

    struct thread_context {
        ::spdk_thread* thread{nullptr};
        uint32_t core_id{0};
        std::shared_ptr<msg::rdma::client> rpc_client{nullptr};
        std::unordered_map<uint64_t, std::unique_ptr<osd_stub>> stubs{};
        std::unordered_map<uint64_t, std::list<std::function<void(osd::rpc_service_osd_Stub*)>>> onflight_leader_stub_cbs{};
    };

    struct start_context {
        connection_pool* this_pool{nullptr};
        std::function<void(bool)> cb{};
        ssize_t start_count{0};
    };

    struct stop_context {
        connection_pool* this_pool{nullptr};
        std::function<void()> cb{};
        ssize_t stop_count{0};
    };

    struct emplace_stub_context {
        int32_t pool_id{0};
        int32_t pg_id{0};
        leader_osd_info* osd_info{nullptr};
        std::unique_ptr<osd::rpc_service_osd_Stub> stub{nullptr};
        thread_context* thd_ctx{nullptr};
        connection_pool* this_pool{nullptr};
    };

    struct emplace_leader_stub_context {
        leader_osd_info* osd_info{nullptr};
        thread_context* thd_ctx{nullptr};
        connection_pool* this_pool{nullptr};
    };

    struct get_leader_stub_context {
        connection_pool* this_pool{nullptr};
        int32_t pool_id{0};
        int32_t pg_id{0};
        std::function<void(osd::rpc_service_osd_Stub*)> cb{};
        thread_context* thd_ctx{nullptr};
    };

public:

    connection_pool() = delete;

    connection_pool(
      const size_t n_worker,
      std::vector<core_sharded::core_id_type>& cores,
      std::shared_ptr<msg::rdma::client::options> opts,
      monitor::client* mon, pg_client* pg_cli) : _mon_cli{mon}, _pg_cli{pg_cli} {
        auto core_it = cores.begin();
        for (size_t i = 0; i < n_worker; ++i) {
            auto mask = core_sharded::make_cpumask(*core_it);
            auto name = FMT_1("fb_conn_%1%", i);
            auto* thread = ::spdk_thread_create(name.c_str(), mask.get());
            _ctxs.emplace_back(
              thread, *core_it,
              std::make_shared<msg::rdma::client>(FMT_1("fb_rpcli_%1%", i), mask.get(), opts));
            SPDK_DEBUGLOG(blkcli, "created worker %ld on core %d\n", i, *core_it);

            ++core_it;
            if (core_it == cores.end()) { core_it = cores.begin(); }
        }
        _mgr = _ctxs.front().thread;
    }

    connection_pool(
      std::vector<core_sharded::core_id_type>& cores,
      std::shared_ptr<msg::rdma::client::options> opts,
      monitor::client* mon, pg_client* pg_cli) : connection_pool{cores.size(), cores, opts, mon, pg_cli} {}

    connection_pool(const connection_pool&) = delete;

    connection_pool(connection_pool&&) = default;

    connection_pool& operator=(const connection_pool&) = delete;

    connection_pool& operator=(connection_pool&&) = delete;

    ~connection_pool() noexcept = default;

public:

    /************************************************************
     * spdk_thread_send_msg callbacks' callbacks
     ************************************************************/

    static void on_start(void* arg) {
        auto ctx = reinterpret_cast<start_context*>(arg);
        ctx->this_pool->handle_start(ctx);
    }

    static void on_rpc_client_started(void* arg) {
        auto ctx = reinterpret_cast<start_context*>(arg);
        ctx->this_pool->handle_rpc_client_started(ctx);
    }

    static void on_stop(void* arg) {
        auto* ctx = reinterpret_cast<stop_context*>(arg);
        ctx->this_pool->handle_stop(ctx);
    }

    static void on_rpc_client_stopped(void* arg) {
        auto* ctx = reinterpret_cast<stop_context*>(arg);
        ctx->this_pool->handle_rpc_client_stop(ctx);
    }

    static void on_get_leader_stub(void* arg) {
        auto ctx = reinterpret_cast<get_leader_stub_context*>(arg);
        ctx->this_pool->handle_get_leader_stub(ctx);
    }

    static void on_emplace_stub(void* arg) {
        auto* ctx = reinterpret_cast<emplace_stub_context*>(arg);
        ctx->this_pool->handle_emplace_stub(std::unique_ptr<emplace_stub_context>(ctx));
    }

    static void on_emplace_leader_stub(void* arg) {
        auto* ctx = reinterpret_cast<emplace_leader_stub_context*>(arg);
        ctx->this_pool->handle_emplace_leader_stub(std::unique_ptr<emplace_leader_stub_context>(ctx));
    }

    /************************************************************
     * spdk_thread_send_msg callbacks
     ************************************************************/

    void handle_emplace_stub(std::unique_ptr<emplace_stub_context> ctx) {
        auto* stub_ptr = ctx->stub.get();
        SPDK_DEBUGLOG(
          blkcli,
          "emplace new stub with pool id %d, pg id %d, osd_info addr is %p, stub addr is %p\n",
          ctx->pool_id, ctx->pg_id, ctx->osd_info, stub_ptr);
        auto leader_key = make_leader_key(ctx->pool_id, ctx->pg_id);
        auto it = ctx->thd_ctx->stubs.find(leader_key);
        if (it == ctx->thd_ctx->stubs.end()) {
            ctx->thd_ctx->stubs.emplace(
              make_leader_key(ctx->pool_id, ctx->pg_id),
              std::make_unique<osd_stub>(std::move(ctx->stub), ctx->osd_info));
        } else {
            it->second->osd_info = ctx->osd_info;
            it->second->stub = std::move(ctx->stub);
        }
        auto is_leader_stub = !!ctx->osd_info;
        SPDK_DEBUGLOG(
          blkcli,
          "is_leader_stub %d, pool id %d, pg id %d\n",
          is_leader_stub, ctx->pool_id, ctx->pg_id);

        if (is_leader_stub) {
            auto leader_key = make_leader_key(ctx->pool_id, ctx->pg_id);
            auto cbs_it = ctx->thd_ctx->onflight_leader_stub_cbs.find(leader_key);
            if (cbs_it == ctx->thd_ctx->onflight_leader_stub_cbs.end()) {
                SPDK_ERRLOG(
                  "Cant find the get leader osd callback list of pool %d, pg %d\n",
                  ctx->pool_id, ctx->pg_id);
                return;
            }

            for (auto&& cb : cbs_it->second) {
                cb(stub_ptr);
            }
            ctx->thd_ctx->onflight_leader_stub_cbs.erase(cbs_it);
        }
    }

    void handle_emplace_leader_stub(std::unique_ptr<emplace_leader_stub_context> ctx) {
        auto* osd_info = ctx->osd_info;
        auto* thd_ctx = ctx->thd_ctx;
        thd_ctx->rpc_client->emplace_connection(
          osd_info->addr,
          osd_info->port,
          [osd_info, thd_ctx, this] (bool is_connected, std::shared_ptr<msg::rdma::client::connection> conn) {
              if (not is_connected) {
                  SPDK_ERRLOG(
                    "ERROR: Connect to %s:%d failed\n",
                    osd_info->addr.c_str(), osd_info->port);
                  throw std::runtime_error{"make connection failed\n"};
              }
              SPDK_NOTICELOG(
                "Connected to leader osd(%s:%d) of pool %d, pg %d\n",
                osd_info->addr.c_str(), osd_info->port, osd_info->pool_id, osd_info->pg_id);
              auto stub = std::make_unique<osd::rpc_service_osd_Stub>(conn.get());
              auto stub_ptr = stub.get();
              auto* emplace_ctx = new emplace_stub_context{
                osd_info->pool_id,
                osd_info->pg_id,
                osd_info,
                std::move(stub),
                thd_ctx,
                this};
              ::spdk_thread_send_msg(thd_ctx->thread, on_emplace_stub, emplace_ctx);
          }
        );
    }

    void handle_start(start_context* ctx) {
        SPDK_INFOLOG(blkcli, "Starting block client connection pool...\n");
        ctx->start_count = _ctxs.size();

        for (size_t i{0}; i < _ctxs.size(); ++i) {
            _ctxs[i].rpc_client->start([this, ctx] () mutable {
                ::spdk_thread_send_msg(_mgr, on_rpc_client_started, ctx);
            });
        }
    }

    void handle_rpc_client_started(start_context* ctx) {
        ctx->start_count -= 1;
        if (ctx->start_count > 0) {
            SPDK_NOTICELOG("the %ldth rcp client started\n", _ctxs.size() - ctx->start_count);
            return;
        }

        SPDK_INFOLOG(blkcli, "all block client connection started\n");
        try {
            (ctx->cb)(true);
        } catch (const std::exception& e) {
            SPDK_ERRLOG("start fblock_client error: %s\n", e.what());
        }
        delete ctx;
    }

    void handle_stop(stop_context* ctx) {
        SPDK_NOTICELOG("Stopping block client connection pool...\n");
        ctx->stop_count = _ctxs.size();
        for (size_t i{0}; i < _ctxs.size(); ++i) {
            _ctxs[i].rpc_client->stop([this, ctx] () mutable {
                ::spdk_thread_send_msg(_mgr, on_rpc_client_stopped, ctx);
            });
        }
    }

    void handle_rpc_client_stop(stop_context* ctx) {
        ctx->stop_count -= 1;
        if (ctx->stop_count > 0) {
            SPDK_NOTICELOG("the %ldth rcp client stopped\n", _ctxs.size() - ctx->stop_count);
            return;
        }

        SPDK_NOTICELOG("all block client connection stopped, starting free worker spdk threads\n");
        for (auto& ctx : _ctxs) {
            if (ctx.thread == _mgr) {
                continue;
            }

            ::spdk_set_thread(ctx.thread);
            ::spdk_thread_exit(ctx.thread);
        }
        ::spdk_set_thread(_mgr);
        try {
            (ctx->cb)();
        } catch (const std::exception& e) {
            SPDK_ERRLOG("start fblock_client error: %s\n", e.what());
        }
        delete ctx;
        ::spdk_thread_exit(_mgr);
        ::spdk_set_thread(nullptr);
    }

    void handle_get_leader_stub(get_leader_stub_context* ctx) {
        SPDK_DEBUGLOG(blkcli, "handle_get_leader_stub(): pool id is %d\n", ctx->pool_id);
        auto stub_key = make_leader_key(ctx->pool_id, ctx->pg_id);
        auto* thd_ctx = ctx->thd_ctx;
        auto onflight_it = thd_ctx->onflight_leader_stub_cbs.find(stub_key);
        if (onflight_it != thd_ctx->onflight_leader_stub_cbs.end()) {
            onflight_it->second.emplace_back(std::move(ctx->cb));
            SPDK_DEBUGLOG(blkcli, "get leader request for pool %d has been submitted, skip this request\n", ctx->pool_id);
            return;
        }

        auto stub_it = ctx->thd_ctx->stubs.find(stub_key);
        if (stub_it != ctx->thd_ctx->stubs.end() and stub_it->second->osd_info) {
            if (pg_client::is_leader_changed(stub_it->second->osd_info)) {
                SPDK_NOTICELOG("leader changed of pool %d, pg %d\n", ctx->pool_id, ctx->pg_id);
                ctx->thd_ctx->stubs.erase(stub_it);
                stub_it = ctx->thd_ctx->stubs.end();
            }
            SPDK_DEBUGLOG(blkcli, "pg leader is not changed for pool %d\n", ctx->pool_id);
        }

        auto on_leader_acquired = [this, thd_ctx = ctx->thd_ctx] (leader_osd_info* info) {
            SPDK_INFOLOG(
              blkcli,
              "acquired leader of %d, addr %s:%d\n",
              info->leader_id, info->addr.c_str(), info->port);
            auto* ctx = new emplace_leader_stub_context{info, thd_ctx, this};
            ::spdk_thread_send_msg(thd_ctx->thread, on_emplace_leader_stub, ctx);
        };

        if (stub_it == ctx->thd_ctx->stubs.end()) {
            auto [it, _] = thd_ctx->onflight_leader_stub_cbs.emplace(
              stub_key,
              std::list<std::function<void(osd::rpc_service_osd_Stub*)>>{});
            it->second.emplace_back(std::move(ctx->cb));
            SPDK_DEBUGLOG(
              blkcli,
              "new get leader callback list, pool %d, pg %d\n",
              ctx->pool_id, ctx->pg_id);

            auto* first_osd = _mon_cli->get_pg_first_available_osd_info(ctx->pool_id, ctx->pg_id);
            if (not first_osd) {
                SPDK_ERRLOG(
                  "ERROR: Cant find any available osd of pg %d, pool id %d\n",
                  ctx->pool_id, ctx->pg_id);
                ctx->cb(nullptr);
                return;
            }
            SPDK_DEBUGLOG(blkcli, "first osd addr is %s:%d\n", first_osd->address.c_str(), first_osd->port);

            thd_ctx->rpc_client->emplace_connection(
              first_osd->address,
              first_osd->port,
              [this, first_osd, leader_cb = std::move(on_leader_acquired), raw_ctx = ctx]
              (bool is_connected, std::shared_ptr<msg::rdma::client::connection> conn) {
                  SPDK_DEBUGLOG(
                    blkcli,
                    "emplaced stubs of node_id %d, addr is '%s:%d'\n",
                    ::spdk_env_get_current_core(), first_osd->address.c_str(), first_osd->port);

                  auto ctx = std::unique_ptr<get_leader_stub_context>{raw_ctx};
                  if (not is_connected) {
                      SPDK_ERRLOG(
                        "ERROR: Connect to %s:%d failed\n",
                        first_osd->address.c_str(), first_osd->port);
                      throw std::runtime_error{"make connection failed\n"};
                  }
                  SPDK_INFOLOG(
                    blkcli,
                    "Connected to %s:%d \n",
                    first_osd->address.c_str(),
                    first_osd->port);
                  auto stub = std::make_unique<osd::rpc_service_osd_Stub>(conn.get());
                  auto stub_ptr = stub.get();
                  auto* emplace_ctx = new emplace_stub_context{
                    ctx->pool_id,
                    ctx->pg_id,
                    nullptr,
                    std::move(stub),
                    ctx->thd_ctx,
                    this};
                  ::spdk_thread_send_msg(ctx->thd_ctx->thread, on_emplace_stub, emplace_ctx);
                  _pg_cli->query_leader(
                    ctx->pool_id,
                    ctx->pg_id,
                    stub_ptr,
                    std::move(leader_cb));
              });

            return;
        }

        SPDK_DEBUGLOG(
          blkcli,
          "leader_osd_info is still valid, pool %d, pg %d\n",
          ctx->pool_id, ctx->pg_id);
        ctx->cb(stub_it->second->stub.get());
    }

public:

    /************************************************************
     * public methods
     ************************************************************/

    auto start(std::function<void(bool)>&& cb) {
        auto* ctx = new start_context{this, std::move(cb)};
        return ::spdk_thread_send_msg(_mgr, on_start, ctx);
    }

    auto stop(std::function<void()>&& cb) {
        if (_is_terminated) {
            cb();
        }

        _is_terminated = true;
        auto* ctx = new stop_context{this, std::move(cb)};
        ::spdk_thread_send_msg(_mgr, on_stop, ctx);
    }

    auto get_leader_stub(
      const int32_t pool_id,
      const int32_t pg_id,
      const std::string& object_name,
      std::function<void(osd::rpc_service_osd_Stub*)>&& cb) {
        auto obj_hash = std::hash<std::string>{}(object_name);
        auto& thd_ctx = _ctxs.at(obj_hash % _ctxs.size());
        auto* ctx = new get_leader_stub_context{this, pool_id, pg_id, std::move(cb), &thd_ctx};
        SPDK_DEBUGLOG(blkcli, "get leader message sent\n");
        return ::spdk_thread_send_msg(thd_ctx.thread, on_get_leader_stub, ctx);
    }

private:
    bool _is_started{false};
    bool _is_terminated{false};
    monitor::client* _mon_cli{nullptr};
    pg_client* _pg_cli{nullptr};
    ::spdk_thread* _thread{nullptr};
    std::vector<thread_context> _ctxs{};
    ::spdk_thread* _mgr{};
};
} // namespace client
} // namespace fastblock
