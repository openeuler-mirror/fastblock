#include "partition_manager.h"
#include "storage/pp_config.h"
#include "spdk/env.h"
#include "spdk/log.h"

struct partition_args{
    partition_args(
        partition_manager* _pm, uint64_t _pool_id, uint64_t _pg_id, 
        std::vector<osd_info_t>&& _osds, int64_t _revision_id)
    : pm(_pm)
    , pool_id(_pool_id)
    , pg_id(_pg_id)
    , osds(std::move(_osds))
    , revision_id(_revision_id){}

    partition_args(partition_manager* _pm, uint64_t _pool_id, uint64_t _pg_id)
    : pm(_pm)
    , pool_id(_pool_id)
    , pg_id(_pg_id){}

    partition_manager* pm;
    uint64_t pool_id;
    uint64_t pg_id;
    std::vector<osd_info_t> osds;
    int64_t revision_id;
};

bool partition_manager::get_pg_core(uint64_t pool_id, uint64_t pg_id, uint32_t &core_id){
    std::string name = pg_id_to_name(pool_id, pg_id);
    if(_core_table.count(name) == 0)
        return false;

    core_id = _core_table[name]._shard;
    return true;
}

void partition_manager::create_pg(
        uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t> osds, 
        uint32_t core_id, int64_t revision_id){
    auto sm = std::make_shared<osd_sm>(_datadir);
    auto name = pg_id_to_name(pool_id, pg_id);
    _sm_table[core_id][std::move(name)] = sm; 

    storage::pp_config pp_cfg(pool_id, pg_id, _logdir, _datadir, revision_id);
    auto log =  _log.manage(std::move(pp_cfg));  
    _pgs.create_pg(sm, core_id, pool_id, pg_id, std::move(osds), std::move(log)); 
}

void create_partition_func(void *arg){
    partition_args* partition = (partition_args*)arg;
    partition_manager* pm = partition->pm;
    SPDK_NOTICELOG("create_partition in core %u pool_id %lu pg_id %lu _next_core %u _core_num %u\n", 
        spdk_env_get_current_core(), partition->pool_id, partition->pg_id,  pm->_next_core, pm->_core_num);
    pm->create_pg(partition->pool_id, partition->pg_id, 
                std::move(partition->osds), spdk_env_get_current_core(), partition->revision_id);
    delete partition;
}

int partition_manager::create_partition(
        uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t>&& osds, int64_t revision_id){
    //lock
    auto  core_id = get_next_core_id();
    _add_pg_core(pool_id, pg_id, core_id, revision_id);

    if(spdk_env_get_current_core() == core_id){
        SPDK_NOTICELOG("create_partition in core %u pool_id %lu pg_id %lu _next_core %u _core_num %u\n", 
            spdk_env_get_current_core(), pool_id, pg_id,  _next_core, _core_num);
        create_pg(pool_id, pg_id, std::move(osds), core_id, revision_id);
    }else{
        auto partition = new partition_args(
                this, pool_id, pg_id, std::move(osds), revision_id);
        spdk_thread_send_msg(_threads[core_id], create_partition_func, (void *)partition);
    }
    return 0;
}

void partition_manager::delete_pg(uint64_t pool_id, uint64_t pg_id, uint32_t core_id){
    auto name = pg_id_to_name(pool_id, pg_id);
    _sm_table[core_id].erase(name);

    _log.remove();
    _pgs.delete_pg(core_id, pool_id, pg_id);
}

void delete_partition_func(void *arg){
    partition_args* partition = (partition_args*)arg;
    SPDK_NOTICELOG("delete_partition in core %u\n", spdk_env_get_current_core());
    partition_manager* pm = partition->pm;
    pm->delete_pg(partition->pool_id, partition->pg_id, spdk_env_get_current_core());
    delete partition;
}

int partition_manager::delete_partition(uint64_t pool_id, uint64_t pg_id){
    uint32_t core_id;

    if(!get_pg_core(pool_id, pg_id, core_id)){
        return -1;
    }
    if(spdk_env_get_current_core() == core_id){
        delete_pg(pool_id, pg_id, core_id);
    }else{
        auto partition = new partition_args(this, pool_id, pg_id);
        spdk_thread_send_msg(_threads[core_id], delete_partition_func, (void *)partition);
    }
    return 0;
}

void partition_manager::create_spdk_threads(){
    for(auto i = 0; i < _core_num; i++){
        struct spdk_cpuset	tmp_cpumask = {};
	    spdk_cpuset_set_cpu(&tmp_cpumask, i, true);
        std::string thread_name = "block_thread" + i;
        struct spdk_thread *thread = spdk_thread_create(thread_name.c_str(), &tmp_cpumask);   
        _threads.push_back(thread); 
    }
}