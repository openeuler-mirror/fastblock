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
#include "msg/rdma/client.h"
#include "base/core_sharded.h"

class connect_cache{
public:
    using connect_ptr = std::shared_ptr<msg::rdma::client::connection>;
    using transport_client_ptr = std::shared_ptr<msg::rdma::client>;

    connect_cache(::spdk_cpuset* cpumask, std::shared_ptr<msg::rdma::client::options> opts, int sock_id = SPDK_ENV_SOCKET_ID_ANY)
      : _mutex(PTHREAD_MUTEX_INITIALIZER)
      , _shard_cores(core_sharded::get_shard_cores()) {
          std::string cli_name{"connect_cache"};
          _transport = std::make_shared<msg::rdma::client>(cli_name, cpumask, opts, sock_id);
          _transport->start();

          auto shard_num = _shard_cores.size();
          for(uint32_t i = 0; i < shard_num; i++){
              _cache.push_back(std::map<int, connect_ptr>());
          }
      }

    connect_cache(const connect_cache&) = delete;
    connect_cache& operator=(const connect_cache&) = delete;

    void create_connect(uint32_t shard_id, int node_id, std::string addr, uint16_t port, auto&& cb, auto&& raft_cb) {
        _transport->emplace_connection(
          addr, port,
          [this, shard_id, node_id, cb = std::move(cb), raft_cb = std::move(raft_cb)]
          (bool is_ok, std::shared_ptr<msg::rdma::client::connection> conn) {
              if (is_ok) {
                  ::pthread_mutex_lock(&_mutex);
                  _cache[shard_id][node_id] = conn;
                  ::pthread_mutex_unlock(&_mutex);
                  raft_cb(conn.get());
              }
              cb(is_ok, conn);
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

    void remove_connect(uint32_t shard_id, int node_id, auto&& cb, auto&& raft_cb){
        if(shard_id >= _shard_cores.size()){
            cb(false);
            return;
        }
        auto iter = _cache[shard_id].find(node_id);
        if(iter == _cache[shard_id].end()){
            cb(false);
            return;
        }

        _transport->remove_connection(iter->second,
          [this, node_id, shard_id, cb = std::move(cb), raft_cb = std::move(raft_cb)](bool is_ok){
            if(is_ok){
                ::pthread_mutex_lock(&_mutex);
                _cache[shard_id].erase(node_id);
                ::pthread_mutex_unlock(&_mutex);
                raft_cb();
            }
            cb(is_ok);
          });
    }

    void stop(auto&&... args) noexcept {
        _transport->stop(std::forward<decltype(args)>(args)...);
    }

private:

    pthread_mutex_t _mutex;
    //每个连接都需要一个id
    transport_client_ptr  _transport;
    std::vector<uint32_t> _shard_cores;
    //每个cpu核上有一个map
    std::vector<std::map<int, connect_ptr>> _cache;
};
