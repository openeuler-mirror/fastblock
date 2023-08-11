#ifndef PG_GROUP_H_
#define PG_GROUP_H_

#include <map>
#include <memory>

#include "raft/raft.h"
#include "spdk/env.h"
#include "utils/utils.h"
#include "base/core_sharded.h"

std::string pg_id_to_name(uint64_t pool_id, uint64_t pg_id);

class pg_t{
public:
    pg_t(std::shared_ptr<raft_server_t> _raft, std::string& _name)
    : raft(_raft)
    , name(_name){}

    void free_pg();
    void start_raft_periodic_timer();

    std::shared_ptr<raft_server_t> raft;
    struct spdk_poller * timer;
    std::string name;
};

class shard_manager{
public:
    shard_manager(uint32_t shard_id)
    : _shard_id(shard_id) {
        // heartbeat_timer = SPDK_POLLER_REGISTER(, , HEARTBEAT_TIMER_PERIOD_MSEC * 1000);
    }

    void add_pg(std::string& name, std::shared_ptr<pg_t> pg){
        pgs[name] = pg;
    }

    void delete_pg(std::string& name){
        pgs.erase(std::move(name));
    }

    std::shared_ptr<pg_t> get_pg(std::string& name){
        if(pgs.find(name) == pgs.end())
            return nullptr;
        return pgs[name];
    }
private:
    uint32_t _shard_id;  //cpu shard id

    //记录此cpu核上的所有pg
    std::map<std::string, std::shared_ptr<pg_t>> pgs;
    struct spdk_poller * heartbeat_timer;
};

class pg_group_t{
public:
    pg_group_t(int current_node_id)
    : _shard_cores(get_shard_cores())
    , _current_node_id(current_node_id)
    , _client() {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for(i = 0; i < shard_num; i++){
            _shard_mg.push_back(shard_manager(i));
        }
    }

    void create_connect(int node_id, auto&&... args){
        _client.create_connect(node_id, std::forward<decltype(args)>(args)...);
    }

    auto& get_raft_client_proto() noexcept {
        return _client;
    }

    void remove_connect(int node_id){
        _client.remove_connect(node_id);
    }

    int create_pg(std::shared_ptr<state_machine> sm_ptr, uint32_t shard_id, uint64_t pool_id, uint64_t pg_id,
                std::vector<osd_info_t>&& osds, disk_log* log);

    void delete_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id);

    std::shared_ptr<pg_t> get_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id){
        auto name = pg_id_to_name(pool_id, pg_id);
        return _shard_mg[shard_id].get_pg(name);
    }

    std::shared_ptr<pg_t> get_pg(uint32_t shard_id, std::string& name){
        return _shard_mg[shard_id].get_pg(name);
    }

    int get_current_node_id(){
        return _current_node_id;
    }

private:
    int _pg_add(uint32_t shard_id, std::shared_ptr<raft_server_t> raft, uint64_t pool_id, uint64_t pg_id){
        auto name = pg_id_to_name(pool_id, pg_id);
        auto pg = std::make_shared<pg_t>(raft, name);
        _shard_mg[shard_id].add_pg(name, pg);
        return 0;
    }

    int _pg_remove(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id){
        auto name = pg_id_to_name(pool_id, pg_id);
        auto pg = _shard_mg[shard_id].get_pg(name);
        pg->free_pg();
        _shard_mg[shard_id].delete_pg(name);
        return 0;
    }

    //所有的pg按核区分保持在_core_mg中
    std::vector<uint32_t> _shard_cores;
    std::vector<shard_manager> _shard_mg;
    int _current_node_id;
    raft_client_protocol _client;
};

#endif