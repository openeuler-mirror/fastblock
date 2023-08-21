#ifndef  PARTITION_MANAGER_H_
#define  PARTITION_MANAGER_H_

#include "raft/pg_group.h"
#include "osd/osd_sm.h"
#include "osd/mon_client.h"
#include "base/core_sharded.h"

struct shard_revision {
    uint32_t _shard;
    int64_t _revision;
};

class partition_manager{
public:
    partition_manager(
            int node_id, std::string& mon_addr, int mon_port, std::string& osd_addr, 
            int osd_port, std::string& osd_uuid)
    : _pgs(node_id)
    , _next_shard(0)
    , _shard(core_sharded::get_core_sharded())
    , _shard_cores(get_shard_cores())
    , _mon(mon_addr, mon_port, node_id, osd_addr, osd_port, osd_uuid, this) {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for(i = 0; i < shard_num; i++){
            _sm_table.push_back(std::map<std::string, std::shared_ptr<osd_sm>>());
        }
    }

    void start(context *complete){
        _pgs.start(complete);
    }

    void stop(){
        _pgs.stop();
    }

    int connect_mon(){
        return _mon.connect_mon();
    }

    int create_partition(uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t>&& osds, int64_t revision_id);
    int delete_partition(uint64_t pool_id, uint64_t pg_id);

    bool get_pg_shard(uint64_t pool_id, uint64_t pg_id, uint32_t &shard_id);

    void create_pg(uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t> osds, uint32_t shard_id, int64_t revision_id);
    void delete_pg(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id);

    std::shared_ptr<osd_sm> get_osd_sm(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        if(_sm_table[shard_id].count(name) == 0)
            return nullptr;
        return _sm_table[shard_id][name];
    }
    
    std::shared_ptr<raft_server_t> get_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        return _pgs.get_pg(shard_id, name);
    }

    core_sharded& get_shard(){
        return _shard;
    }

    pg_group_t& get_pg_group(){
        return _pgs;
    }

    void add_osd_sm(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id, std::shared_ptr<osd_sm> sm){
        auto name = pg_id_to_name(pool_id, pg_id);
        _sm_table[shard_id][std::move(name)] = sm; 
    }

    mon_client& get_mon(){
        return _mon;
    }

    int get_current_node_id(){
        return _pgs.get_current_node_id();
    }
private:
    uint32_t get_next_shard_id(){
        uint32_t shard_id = _next_shard;
        _next_shard = (_next_shard + 1) % _shard_cores.size();
        return shard_id;
    }

    int _add_pg_shard(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id, int64_t revision_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        _shard_table[std::move(name)] = shard_revision{shard_id, revision_id};
        return 0;
    }

    int _remove_pg_shard(uint64_t pool_id, uint64_t pg_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        auto ret = _shard_table.erase(std::move(name));
        if(ret == 0)
            return -EEXIST;
        return 0;
    }

    pg_group_t _pgs;
    //记录pg到cpu核的对应关系
    std::map<std::string, shard_revision> _shard_table;
    uint32_t _next_shard;
    core_sharded&  _shard;
    std::vector<uint32_t> _shard_cores;
    std::vector<std::map<std::string, std::shared_ptr<osd_sm>>> _sm_table;
    mon_client _mon;
};

#endif