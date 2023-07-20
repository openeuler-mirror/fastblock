#ifndef  PARTITION_MANAGER_H_
#define  PARTITION_MANAGER_H_

#include "storage/log_manager.h"
#include "raft/pg_group.h"
#include "spdk/thread.h"
#include "osd/osd_sm.h"

struct shard_revision {
    uint32_t _shard;
    int64_t _revision;
};

class partition_manager{
public:
    partition_manager(int node_id, uint32_t core_num, const std::string&  logdir, const std::string& datadir)
    : _pgs(node_id, core_num)
    , _next_core(0)
    , _core_num(core_num)
    , _logdir(logdir)
    , _datadir(datadir) {
        uint32_t i = 0;
        for(i = 0; i < _core_num; i++){
            _sm_table.push_back(std::map<std::string, std::shared_ptr<osd_sm>>());
        }
    }

    int create_partition(uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t>&& osds, int64_t revision_id);
    int delete_partition(uint64_t pool_id, uint64_t pg_id);

    bool get_pg_core(uint64_t pool_id, uint64_t pg_id, uint32_t &core_id);

    std::string get_logdir(){
        return _logdir;
    }

    std::string get_datadir(){
        return _datadir;
    }

    void create_spdk_threads();
    void create_pg(uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t> osds, uint32_t core_id, int64_t revision_id);
    void delete_pg(uint64_t pool_id, uint64_t pg_id, uint32_t core_id);

    std::shared_ptr<osd_sm> get_osd_sm(uint64_t pool_id, uint64_t pg_id, uint32_t core_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        if(_sm_table[core_id].count(name) == 0)
            return nullptr;
        return _sm_table[core_id][name];
    }
    
    std::shared_ptr<pg_t> get_pg(uint32_t core_id, uint64_t pool_id, uint64_t pg_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        return _pgs.get_pg(core_id, name);
    }
public:
    uint32_t get_next_core_id(){
        uint32_t core_id = _next_core;
        _next_core = (_next_core + 1) % _core_num;
        return core_id;
    }

    int _add_pg_core(uint64_t pool_id, uint64_t pg_id, uint32_t core_id, int64_t revision_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        _core_table[std::move(name)] = shard_revision{core_id, revision_id};
        return 0;
    }

    int _pg_core_remove(uint64_t pool_id, uint64_t pg_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        auto ret = _core_table.erase(std::move(name));
        if(ret == 0)
            return -EEXIST;
        return 0;
    }

    storage::log_manager _log;
    pg_group_t _pgs;
    //记录pg到cpu核的对应关系
    std::map<std::string, shard_revision> _core_table;
    uint32_t _next_core;
    const uint32_t _core_num;
    std::string  _logdir;
    std::string  _datadir;
    std::vector<struct spdk_thread *> _threads;
    std::vector<std::map<std::string, std::shared_ptr<osd_sm>>> _sm_table;
};

#endif