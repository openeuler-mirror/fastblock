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
#include <memory>

#include "raft/raft.h"
#include "spdk/env.h"
#include "utils/utils.h"
#include "base/core_sharded.h"
#include "localstore/kv_store.h"

namespace monitor {
    class client;
}

class shard_manager
{
public:
    friend class pg_group_t;

    struct node_heartbeat
    {
        node_heartbeat(
            raft_node_id_t t,
            heartbeat_request *req)
            : target(t), request(req) {}

        raft_node_id_t target;
        heartbeat_request *request;
    };

    shard_manager(uint32_t shard_id, pg_group_t *group)
        : _shard_id(shard_id), _group(group) {}

    void add_pg(std::string &name, std::shared_ptr<raft_server_t> pg)
    {
        _pgs[name] = pg;
    }

    void delete_pg(std::string name)
    {
        _pgs.erase(std::move(name));
    }

    std::shared_ptr<raft_server_t> get_pg(std::string &name)
    {
        if (_pgs.find(name) == _pgs.end())
            return nullptr;
        return _pgs[name];
    }

    void start();

    void stop()
    {
        spdk_poller_unregister(&_heartbeat_timer);
        for(auto &[name, raft] : _pgs){
            raft->stop();
        }
    }

    uint32_t get_shard_id()
    {
        return _shard_id;
    }

    void dispatch_heartbeats();
    std::vector<node_heartbeat> get_heartbeat_requests();

private:
    uint32_t _shard_id; // cpu shard id
    pg_group_t *_group;

    // 记录此cpu核上的所有pg
    std::map<std::string, std::shared_ptr<raft_server_t>> _pgs;
    struct spdk_poller *_heartbeat_timer{};
};

using pg_complete = std::function<void (void *, int)>;


constexpr int32_t DEFAULT_HEARTBEAT_TIMER_PERIOD_MSEC = 1000;   //毫秒
constexpr int32_t DEFAULT_ELECTION_TIMER_PERIOD_MSEC = 2000; //毫秒
constexpr int32_t DEFAULT_LEASE_MAINTENANCE_GRACE = 2000;   //毫秒

class pg_group_t
{
public:
    pg_group_t(int current_node_id, std::shared_ptr<connect_cache> conn_cache, 
            int raft_heartbeat_period_time_msec = DEFAULT_HEARTBEAT_TIMER_PERIOD_MSEC,
            int raft_lease_time_msec = DEFAULT_LEASE_MAINTENANCE_GRACE,
            int raft_election_timeout_msec = DEFAULT_ELECTION_TIMER_PERIOD_MSEC)
      : _shard_cores(get_shard_cores())
      , _current_node_id(current_node_id)
      , _client{conn_cache}
      , _shard(core_sharded::get_core_sharded())
      , _raft_heartbeat_period_time_msec(raft_heartbeat_period_time_msec)
      , _raft_lease_time_msec(raft_lease_time_msec)
      , _raft_election_timeout_msec(raft_election_timeout_msec) {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for (i = 0; i < shard_num; i++)
        {
            _shard_mg.push_back(shard_manager(i, this));
        }
    }

    void create_connect(int node_id, auto&&...args)
    {
        _client.create_connect(node_id, std::forward<decltype(args)>(args)...);
    }

    auto &get_raft_client_proto() noexcept
    {
        return _client;
    }

    void remove_connect(int node_id, auto&&...args)
    {
        _client.remove_connect(node_id, std::forward<decltype(args)>(args)...);
    }

    int create_pg(std::shared_ptr<state_machine> sm_ptr, uint32_t shard_id, uint64_t pool_id, uint64_t pg_id,
                  std::vector<utils::osd_info_t> &&osds, disk_log *log, std::shared_ptr<monitor::client> mon_client);
    
    void load_pg(std::shared_ptr<state_machine> sm_ptr, uint32_t shard_id, uint64_t pool_id, uint64_t pg_id,
                disk_log *log, pg_complete cb_fn, void *arg, std::shared_ptr<monitor::client> mon_client);    

    void delete_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id, pg_complete cb_fn, void *arg);
    void active_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id);

    std::shared_ptr<raft_server_t> get_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id)
    {
        auto name = pg_id_to_name(pool_id, pg_id);
        return _shard_mg[shard_id].get_pg(name);
    }

    std::shared_ptr<raft_server_t> get_pg(uint32_t shard_id, std::string &name)
    {
        return _shard_mg[shard_id].get_pg(name);
    }

    int get_current_node_id()
    {
        return _current_node_id;
    }

    void start(utils::complete_fun fun, void *arg);

    void stop(){
        stop_shard_manager();
    }

    void stop(uint64_t shard_id){
        _shard_mg[shard_id].stop();
    }

    void start_shard_manager(utils::complete_fun fun, void *arg);

    void stop_shard_manager()
    {
        uint32_t i = 0;
        auto shard_num = _shard_mg.size();
        for (i = 0; i < shard_num; i++)
        {
            _shard.invoke_on(
                i,
                [this, shard_id = i]()
                {
                    _shard_mg[shard_id].stop();
                });
        }
    }

    void change_pg_membership(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id, std::vector<raft_node_info>&& new_osds, utils::context* complete);
    void load_pgs_map(std::map<uint64_t, std::vector<utils::pg_info_type>> &pools);

private:
    int _pg_add(uint32_t shard_id, std::shared_ptr<raft_server_t> raft, uint64_t pool_id, uint64_t pg_id)
    {
        auto name = pg_id_to_name(pool_id, pg_id);
        _shard_mg[shard_id].add_pg(name, raft);
        return 0;
    }

    int _pg_remove(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id, pg_complete cb_fn, void *arg)
    {
        auto name = pg_id_to_name(pool_id, pg_id);
        auto raft = _shard_mg[shard_id].get_pg(name);
        if(!raft){
            cb_fn(arg, 0);
            return 0;
        }

        raft->raft_destroy(
          [this, name, shard_id, cb_fn = std::move(cb_fn)](void* arg, int rberrno){
            if(rberrno == 0){
                _shard_mg[shard_id].delete_pg(name);
            }
            cb_fn(arg, rberrno);
          },
          arg
        );
        return 0;
    }

    // 所有的pg按核区分保持在_core_mg中
    std::vector<uint32_t> _shard_cores;
    std::vector<shard_manager> _shard_mg;
    int _current_node_id;
    raft_client_protocol _client;
    core_sharded &_shard;
    //心跳发送间隔时间，单位毫秒
    int  _raft_heartbeat_period_time_msec;
    //lease time，单位毫秒
    int  _raft_lease_time_msec;
    //选举超时时间，单位毫秒
    int  _raft_election_timeout_msec;
};