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

#include "client/types.h"
#include "monclient/client.h"
#include "msg/rpc_controller.h"
#include "msg/rdma/client.h"
#include "utils/simple_poller.h"
#include "utils/utils.h"
#include "rpc/osd_msg.pb.h"

#include <google/protobuf/stubs/callback.h>

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/thread.h>

#include <functional>

namespace fastblock {
namespace client {

class pg_client {

public:

    struct control_context {
        pg_client* this_cli{nullptr};
        std::function<void()> cb{};
    };

private:

    struct connection_id {
        int32_t node_id;
        uint32_t core_no;
    };

    struct leader_request_stack_type {
        uint64_t leader_osd_key{0};
        int32_t pool_id;
        int32_t pg_id;
        osd::rpc_service_osd_Stub* stub{nullptr};
        std::unique_ptr<osd::pg_leader_request> leader_req{nullptr};
        std::unique_ptr<osd::pg_leader_response> leader_resp{nullptr};
        uint64_t leader_request_id{};
        std::function<void(leader_osd_info*)> cb{};
        pg_client* this_client{nullptr};
    };

public:

    pg_client(::spdk_thread* work_thread, monitor::client* mon)
      : _thread{work_thread}
      , _mon_cli{mon} {}

    pg_client(const pg_client&) = delete;

    pg_client(pg_client&&) = default;

    pg_client& operator=(const pg_client&) = delete;

    pg_client& operator=(pg_client&&) = delete;

    ~pg_client() =  default;

public:

    /************************************************************
     * spdk_thread_send_msg callbacks' callbacks
     ************************************************************/

    int handle_poll() {
        if (_is_terminated or _leader_requests.empty()) {
            return SPDK_POLLER_IDLE;
        }

        std::erase_if(
          _leader_requests,
          [this] (std::pair<const uint64_t, std::unique_ptr<leader_request_stack_type>>& kv) {
              auto& [_, stack_ptr] = kv;

              auto it = _leader_osd.find(stack_ptr->leader_osd_key);
              bool should_acquire_leader{false};
              if (it == _leader_osd.end()) {
                  SPDK_DEBUGLOG(blkcli, "cant find the leader osd %ld\n", stack_ptr->leader_osd_key);
                  should_acquire_leader = true;
              } else if (it->second->is_onflight) {
                  return false;
                  // should_acquire_leader = false;
              } else if (not it->second->is_valid) {
                  SPDK_DEBUGLOG(blkcli, "leader osd %ld is not valid\n", stack_ptr->leader_osd_key);
                  should_acquire_leader = true;
              }

              SPDK_DEBUGLOG(
                blkcli,
                "leader_osd_key: %lu, should_acquire_leader: %d, pool id %d, pg id %d\n",
                stack_ptr->leader_osd_key, should_acquire_leader,
                it == _leader_osd.end() ? -1 : it->second->pool_id,
                it == _leader_osd.end() ? -1 : it->second->pg_id);

              if (not should_acquire_leader) {
                  try {
                      SPDK_DEBUGLOG(
                        blkcli,
                        "cached leader osd, call cb, pool id %d, pg id%d\n",
                        it->second->pool_id, it->second->pg_id);
                      stack_ptr->cb(it->second.get());
                  } catch (const std::exception& e) {
                      SPDK_ERRLOG("query leader callback error: %s\n", e.what());
                  }

                  return true;
              }

              _leader_osd.emplace(stack_ptr->leader_osd_key, std::make_unique<leader_osd_info>());
              stack_ptr->leader_resp = std::make_unique<osd::pg_leader_response>();
              stack_ptr->leader_req = std::make_unique<osd::pg_leader_request>();
              stack_ptr->leader_req->set_pool_id(stack_ptr->pool_id);
              stack_ptr->leader_req->set_pg_id(stack_ptr->pg_id);

              SPDK_INFOLOG(
                blkcli,
                "send get_leader request for pg %lu.%lu\n",
                stack_ptr->leader_req->pool_id(),
                stack_ptr->leader_req->pg_id());
              auto done = google::protobuf::NewCallback(
                this, &pg_client::on_leader_acquired, stack_ptr.get());
              stack_ptr->stub->process_get_leader(
                &_ctrlr, stack_ptr->leader_req.get(), stack_ptr->leader_resp.get(), done);
              _on_flight_leader_requests.insert(std::move(kv));
              return true;
          }
        );

        return SPDK_POLLER_BUSY;
    }

    void handle_start(std::function<void()>&& cb) {
        _poller = std::make_unique<::utils::simple_poller>();
        _poller->register_poller(pg_client::poller, this, 0);
        try {
            cb();
        } catch (const std::exception& e) {
            SPDK_ERRLOG("callback of starting error: %s\n", e.what());
        }
        SPDK_NOTICELOG("pg_client started\n");
    }

    void handle_query_leader(std::unique_ptr<leader_request_stack_type> req) {
        req->leader_request_id = _leader_req_id_gen++;
        _leader_requests.emplace(req->leader_request_id, std::move(req));
    }

    /************************************************************
     * spdk_thread_send_msg callbacks
     ************************************************************/

    static int poller(void* arg) {
        auto this_cli = reinterpret_cast<pg_client*>(arg);
        return this_cli->handle_poll();
    }

    static void on_start(void* arg) {
        auto* ctx = reinterpret_cast<control_context*>(arg);
        ctx->this_cli->handle_start(std::move(ctx->cb));
        delete ctx;
    }

    static void on_query_leader(void* arg) {
        auto* ctx = reinterpret_cast<leader_request_stack_type*>(arg);
        ctx->this_client->handle_query_leader(std::unique_ptr<leader_request_stack_type>{ctx});
    }

private:

    /************************************************************
     * private methods
     ************************************************************/

    static uint64_t to_connection_id(const int32_t node_id, const uint32_t core_no) {
        uint64_t ret{};
        auto* conn_id = reinterpret_cast<connection_id*>(&ret);
        conn_id->node_id = node_id;
        conn_id->core_no = core_no;

        return ret;
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

    void on_leader_acquired(leader_request_stack_type* stack_ptr) {
        auto it = _on_flight_leader_requests.find(stack_ptr->leader_request_id);
        if (it == _on_flight_leader_requests.end()) {
            SPDK_ERRLOG(
              "Cant find the leader request stack of id %ld, pool id %d, pg id %d\n",
              stack_ptr->leader_request_id, stack_ptr->pool_id, stack_ptr->pg_id);
            throw std::runtime_error{"cant find the leader request stack"};
        }

        auto leader_key = make_leader_key(stack_ptr->pool_id, stack_ptr->pg_id);
        auto info_it = _leader_osd.find(leader_key);
        if (info_it == _leader_osd.end()) {
            SPDK_ERRLOG(
              "ERROR: Cant find the leader osd info record of pool id %ld, pg id %ld\n",
              it->second->leader_req->pool_id(), it->second->leader_req->pg_id());

            throw std::runtime_error{"Cant find the leader osd info record"};
        }
        auto* osd_info = info_it->second.get();
        SPDK_INFOLOG(
          blkcli,
          "Got leader osd %ld, pool id %d, pg id %d\n",
          stack_ptr->leader_request_id,
          stack_ptr->pool_id,
          stack_ptr->pg_id);
        osd_info->is_onflight = false;
        auto* resp = it->second->leader_resp.get();
        osd_info->epoch = std::chrono::system_clock::now();
        osd_info->leader_id = resp->leader_id();
        osd_info->addr = resp->leader_addr();
        osd_info->port = resp->leader_port();
        osd_info->pg_id = it->second->pg_id;
        osd_info->pool_id = it->second->pool_id;

        SPDK_DEBUGLOG(
          blkcli,
          "Got leader osd of pool id %d, pg id %d, osd id %d osd address: '%s:%d'\n",
          osd_info->pool_id,
          osd_info->pg_id,
          osd_info->leader_id,
          osd_info->addr.c_str(),
          osd_info->port);

        update_leader_state(info_it->second.get());
        if (not info_it->second->is_valid) {
            SPDK_NOTICELOG(
              "Got a invalid leader osd(%s:%d) with id %d, pool id %ld, pg id %ld, will retry\n",
              resp->leader_addr().c_str(),
              resp->leader_port(),
              resp->leader_id(),
              stack_ptr->leader_req->pool_id(),
              stack_ptr->leader_req->pg_id());

            auto done = google::protobuf::NewCallback(
              this, &pg_client::on_leader_acquired, stack_ptr);
            stack_ptr->stub->process_get_leader(
              &_ctrlr, stack_ptr->leader_req.get(), stack_ptr->leader_resp.get(), done);

            return;
        }

        try {
            SPDK_DEBUGLOG(
              blkcli,
              "call cb of leader req, pool id %d, pg id %d, osd addr %s:%d\n",
              osd_info->pool_id, osd_info->pg_id, osd_info->addr.c_str(), osd_info->port);
            stack_ptr->cb(osd_info);
        } catch (const std::exception& e) {
            SPDK_ERRLOG("error when calling leader callback: %s\n", e.what());
        }
        _on_flight_leader_requests.erase(it);
    }

public:

    /************************************************************
     * public methods
     ************************************************************/

    void start(std::function<void()>&& cb) {
        if (_is_started) { return; }
        auto* ctx = new control_context{this, std::move(cb)};
        ::spdk_thread_send_msg(_thread, on_start, ctx);
        _is_started = true;
    }

    void stop(std::function<void()>&& cb) {
        _is_terminated = true;
        _poller->unregister_poller([user_cb = std::move(cb)] () {
            try {
                user_cb();
            } catch (const std::exception& e) {
                SPDK_ERRLOG("callback of stopping error: %s\n", e.what());
            }
            SPDK_NOTICELOG("pg_client stopped\n");
        });
    }

    void query_leader(
      const int32_t pool_id,
      const int32_t pg_id,
      osd::rpc_service_osd_Stub* stub,
      std::function<void(leader_osd_info*)>&& cb) {
        SPDK_DEBUGLOG(
          blkcli,
          "query leader osd for pool id %d, pg id %d\n",
          pool_id, pg_id);
        auto* req = new leader_request_stack_type{};
        req->pool_id = pool_id;
        req->pg_id = pg_id;
        req->this_client = this;
        req->cb = std::move(cb);
        req->stub = stub;
        req->leader_osd_key = make_leader_key(pool_id, pg_id);
        ::spdk_thread_send_msg(_thread, on_query_leader, req);
    }

    /************************************************************
     * public static methods
     ************************************************************/
    static bool is_leader_changed(leader_osd_info* osd_info) {
        return (not osd_info->is_valid) or osd_info->is_onflight;
    }

private:

    bool _is_started{false};
    bool _is_terminated{false};
    ::spdk_thread* _thread{nullptr};
    monitor::client* _mon_cli{nullptr};
    uint64_t _leader_req_id_gen{0};
    std::unordered_map<uint64_t, std::unique_ptr<leader_request_stack_type>> _leader_requests{};
    std::unordered_map<uint64_t, std::unique_ptr<leader_request_stack_type>> _on_flight_leader_requests{};
    std::unordered_map<uint64_t, std::unique_ptr<leader_osd_info>> _leader_osd{};
    std::unique_ptr<::utils::simple_poller> _poller{nullptr};
    msg::rdma::rpc_controller _ctrlr{};
};

}
}
