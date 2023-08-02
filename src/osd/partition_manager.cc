#include "partition_manager.h"
#include "storage/pp_config.h"
#include "spdk/env.h"
#include "spdk/log.h"

bool partition_manager::get_pg_shard(uint64_t pool_id, uint64_t pg_id, uint32_t &shard_id){
    std::string name = pg_id_to_name(pool_id, pg_id);
    if(_shard_table.count(name) == 0)
        return false;

    shard_id = _shard_table[name]._shard;
    return true;
}

void partition_manager::create_pg(
        uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t> osds, 
        uint32_t shard_id, int64_t revision_id){
    auto sm = std::make_shared<osd_sm>(_datadir);
    auto name = pg_id_to_name(pool_id, pg_id);
    _sm_table[shard_id][std::move(name)] = sm; 

    storage::pp_config pp_cfg(pool_id, pg_id, _logdir, _datadir, revision_id);
    auto log =  _log.manage(std::move(pp_cfg));  
    _pgs.create_pg(sm, shard_id, pool_id, pg_id, std::move(osds), std::move(log)); 
}

int partition_manager::create_partition(
        uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t>&& osds, int64_t revision_id){
    auto shard_id = get_next_shard_id();
    _add_pg_shard(pool_id, pg_id, shard_id, revision_id);

    return _shard.invoke_on(
      shard_id, 
      [this, pool_id, pg_id, osds = std::move(osds), revision_id, shard_id](){
        SPDK_NOTICELOG("create pg in core %u  shard_id %u pool_id %lu pg_id %lu \n", 
            spdk_env_get_current_core(), shard_id, pool_id, pg_id);
        create_pg(pool_id, pg_id, std::move(osds), shard_id, revision_id);                
      });
}

void partition_manager::delete_pg(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id){
    auto name = pg_id_to_name(pool_id, pg_id);
    _sm_table[shard_id].erase(name);

    _log.remove();
    _pgs.delete_pg(shard_id, pool_id, pg_id);
}

int partition_manager::delete_partition(uint64_t pool_id, uint64_t pg_id){
    uint32_t shard_id;

    if(!get_pg_shard(pool_id, pg_id, shard_id)){
        return -1;
    }
    
    return _shard.invoke_on(
      shard_id, 
      [this, pool_id, pg_id, shard_id](){
        SPDK_NOTICELOG("delete pg in core %u shard_id %u pool_id %lu pg_id %lu \n", 
            spdk_env_get_current_core(), shard_id, pool_id, pg_id);
        delete_pg(pool_id, pg_id, shard_id);                   
      });
}