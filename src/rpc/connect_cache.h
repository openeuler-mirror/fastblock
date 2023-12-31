/* Copyright (c) 2023 ChinaUnicom
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
#include "msg/transport_client.h"
#include "base/core_sharded.h"

class connect_cache{
public:
    using connect_ptr = std::shared_ptr<msg::rdma::transport_client::connection>;
    using transport_client_ptr = std::unique_ptr<msg::rdma::transport_client>;

    connect_cache(const connect_cache&) = delete;
    connect_cache& operator=(const connect_cache&) = delete;

    connect_ptr create_connect(uint32_t shard_id, int node_id, auto&&... args){
        pthread_mutex_lock(&_mutex);
        auto id = _id++;
        auto connect = _transport->emplace_connection(id, std::forward<decltype(args)>(args)...);
        _cache[shard_id][node_id] = std::make_pair(id, connect);
        pthread_mutex_unlock(&_mutex);
        return connect;
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
        return iter->second.second;
    }

    void remove_connect(uint32_t shard_id, int node_id){
        if(shard_id >= _shard_cores.size())
            return;
        auto iter = _cache[shard_id].find(node_id);
        if(iter == _cache[shard_id].end())
            return;
        auto id = iter->second.first;
        _transport->erase_connection(id);
        _cache[shard_id].erase(node_id);
    }

    static connect_cache& get_connect_cache(){
        static connect_cache s_connect_cache;
        return s_connect_cache;
    }

    void stop() noexcept {
        _transport->stop();
    }

private:
    connect_cache()
    : _mutex(PTHREAD_MUTEX_INITIALIZER)
    , _id(0)
    , _shard_cores(get_shard_cores()) {
        _transport = std::make_unique<msg::rdma::transport_client>();
        _transport->start();

        auto shard_num = _shard_cores.size();
        for(uint32_t i = 0; i < shard_num; i++){
            _cache.push_back(std::map<int, std::pair<uint64_t, connect_ptr>>());
        }
    }
    pthread_mutex_t _mutex;
    //每个连接都需要一个id
    uint64_t _id;
    transport_client_ptr  _transport;
    std::vector<uint32_t> _shard_cores;
    //每个cpu核上有一个map
    std::vector<std::map<int, std::pair<uint64_t, connect_ptr>>> _cache;
};