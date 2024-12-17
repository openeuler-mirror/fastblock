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

#include <map>
#include <pthread.h>
#include <vector>
#include <cstdint>

#include "fastblock/msg/rdma/client.h"
#include "fastblock/base/core_sharded.h"
#include "fastblock/utils/err_num.h"

class connect_cache{
public:
    using connect_ptr = std::shared_ptr<msg::rdma::client::connection>;
    using transport_client_ptr = std::shared_ptr<msg::rdma::client>;

    connect_cache(::spdk_cpuset* cpumask, std::shared_ptr<msg::rdma::client::options> opts, int sock_id = SPDK_ENV_SOCKET_ID_ANY)
      : _mutex(PTHREAD_MUTEX_INITIALIZER)
      , _shard_cores(core_sharded::get_shard_cores()) {
          auto shard_num = _shard_cores.size();
          for(uint32_t i = 0; i < shard_num; i++){
              std::string cli_name = "connect_cache" + std::to_string(i);
              auto _transport = std::make_shared<msg::rdma::client>(cli_name, core_sharded::get_thread(i), opts, sock_id);
              _transports.push_back(_transport);
              _transport->start();
              _cache.push_back(std::map<int, connect_ptr>());
          }
      }

    connect_cache(const connect_cache&) = delete;
    connect_cache& operator=(const connect_cache&) = delete;

    void create_connect(
      int32_t shard_id,
      int node_id,
      std::string addr,
      uint16_t port,
      std::function<void(bool, msg::rdma::client::connection*)> raft_cb) {
        _transports[shard_id]->emplace_connection(
          addr, port,
          [this, shard_id, node_id, raft_cb = std::move(raft_cb)]
          (bool is_ok, std::shared_ptr<msg::rdma::client::connection> conn) {
              if (is_ok) {
                  _cache[shard_id][node_id] = conn;
              }
              raft_cb(is_ok, conn.get());
          }
        );
    }

    void create_connect(uint32_t shard_id, int node_id, std::string addr, uint16_t port, utils::context* ctx, auto&& raft_cb) {
        _transports[shard_id]->emplace_connection(
          addr, port,
          [this, shard_id, node_id, ctx, raft_cb = std::move(raft_cb)]
          (bool is_ok, std::shared_ptr<msg::rdma::client::connection> conn) {
              if (is_ok) {
                //   ::pthread_mutex_lock(&_mutex);
                  _cache[shard_id][node_id] = conn;
                //   ::pthread_mutex_unlock(&_mutex);
                  raft_cb(conn.get());
                  ctx->complete(err::E_SUCCESS);
              } else {
                  ctx->complete(err::RAFT_ERR_UNKNOWN);
              }
          }
        );

        return;
    }

    bool contains(uint32_t shard_id, int node_id){
        if(shard_id >= _shard_cores.size())
            return false;
        return _cache[shard_id].find(node_id) != _cache[shard_id].end();
    }

    connect_ptr get_connect(uint32_t shard_id, int node_id){
        if(shard_id >= _shard_cores.size())
            return nullptr;
        auto iter = _cache[shard_id].find(node_id);
        if(iter == _cache[shard_id].end())
            return nullptr;
        return iter->second;
    }

    void remove_connect(uint32_t shard_id, int node_id, utils::context* ctx, auto&& raft_cb){
        if(shard_id >= _shard_cores.size()){
            ctx->complete(err::RAFT_ERR_UNKNOWN);
            return;
        }
        auto iter = _cache[shard_id].find(node_id);
        if(iter == _cache[shard_id].end()){
            ctx->complete(err::RAFT_ERR_UNKNOWN);
            return;
        }

        _transports[shard_id]->remove_connection(iter->second,
          [this, node_id, shard_id, ctx, raft_cb = std::move(raft_cb)](bool is_ok){
            if(is_ok){
                // ::pthread_mutex_lock(&_mutex);
                _cache[shard_id].erase(node_id);
                // ::pthread_mutex_unlock(&_mutex);
                raft_cb();
                ctx->complete(err::E_SUCCESS);
            } else {
                ctx->complete(err::RAFT_ERR_UNKNOWN);
            }
          });
    }

private:
    struct stop_context{
        std::function<void()> cb_fn;
    };

public:
    void stop(std::optional<std::function<void()>>&& on_stop = std::nullopt) noexcept {
        auto ctx = new stop_context{};
        if(on_stop.has_value()){
            ctx->cb_fn = on_stop.value();
        }else {
            ctx->cb_fn = [](){};
        }
        auto thread = spdk_get_thread();
        auto func = [thread, ctx](void *, int ){
            auto cur_thread = spdk_get_thread();
            if(thread != cur_thread){
                spdk_thread_send_msg(thread, _stop_done, ctx);
            } else {
                _stop_done(ctx);
            }
        };
        auto shard_num = _shard_cores.size();
        utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, func, nullptr);
        for (uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
            core_sharded::get_core_sharded().invoke_on(
              shard_id,
              [this, shard_id, complete](){
                _transports[shard_id]->stop([complete](){
                  complete->complete(0);
                });
              }
            );
        }
    }

private:
    static void _stop_done(void *arg){
        struct stop_context *ctx = (struct stop_context *)arg;
        if(ctx){
            ctx->cb_fn();
            delete ctx;
        }
    }

    pthread_mutex_t _mutex;
    //每个连接都需要一个id
    std::vector<transport_client_ptr>  _transports;
    std::vector<uint32_t> _shard_cores;
    //每个cpu核上有一个map
    std::vector<std::map<int, connect_ptr>> _cache;
};
