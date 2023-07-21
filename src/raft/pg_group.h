#ifndef PG_GROUP_H_
#define PG_GROUP_H_

#include <map>
#include <memory>

#include "raft/raft.h"
#include "spdk/env.h"
#include "utils/utils.h"

#define  HEARTBEAT_TIMER_PERIOD_MSEC  1000   //毫秒

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

class core_manager{
public:
    core_manager(uint32_t core)
    : _core(core) {
        // heartbeat_timer = SPDK_POLLER_REGISTER(, , HEARTBEAT_TIMER_PERIOD_MSEC * 1000);
    }

    void add_pg(std::string& name, std::shared_ptr<pg_t> pg){
        pgs[name] = pg;
    }

    void delete_pg(std::string& name){
        pgs.erase(std::move(name));
    }

    std::shared_ptr<pg_t> get_pg(std::string& name){
        if(pgs.count(name) == 0)
            return nullptr;
        return pgs[name];
    }
private:
    uint32_t _core;  //cpu core

    //记录此cpu核上的所有pg
    std::map<std::string, std::shared_ptr<pg_t>> pgs;
    struct spdk_poller * heartbeat_timer; 
};

class pg_group_t{
public:
    pg_group_t(int current_node_id, uint32_t core_num)
    : _core_num(core_num)
    , _current_node_id(current_node_id) {
        uint32_t i = 0;
        for(i = 0; i < _core_num; i++){
            _core_mg.push_back(core_manager(i));
        }
    }


    int create_pg(std::shared_ptr<state_machine> sm_ptr, uint32_t core_id, uint64_t pool_id, uint64_t pg_id,
                std::vector<osd_info_t>&& osds, storage::log&& log);

    void delete_pg(uint32_t core_id, uint64_t pool_id, uint64_t pg_id);

    std::shared_ptr<pg_t> get_pg(uint32_t core_id, uint64_t pool_id, uint64_t pg_id){
        auto name = pg_id_to_name(pool_id, pg_id);
        return _core_mg[core_id].get_pg(name);
    }

    std::shared_ptr<pg_t> get_pg(uint32_t core_id, std::string& name){
        return _core_mg[core_id].get_pg(name);
    }

    int get_current_node_id(){
        return _current_node_id;
    }

    uint32_t get_core_num(){
        return _core_num;
    }
private:
    int _pg_add(uint32_t core_id, std::shared_ptr<raft_server_t> raft, uint64_t pool_id, uint64_t pg_id){
        auto name = pg_id_to_name(pool_id, pg_id);
        auto pg = std::make_shared<pg_t>(raft, name);
        _core_mg[core_id].add_pg(name, pg);
        return 0;        
    }

    int _pg_remove(uint32_t core_id, uint64_t pool_id, uint64_t pg_id){
        auto name = pg_id_to_name(pool_id, pg_id);
        auto pg = _core_mg[core_id].get_pg(name);
        pg->free_pg();
        _core_mg[core_id].delete_pg(name);
        return 0;        
    }

    //所有的pg按核区分保持在_core_mg中
    uint32_t _core_num;
    std::vector<core_manager> _core_mg;
    int _current_node_id;
};

#endif