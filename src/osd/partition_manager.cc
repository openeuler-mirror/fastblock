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

#include "partition_manager.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "localstore/blob_manager.h"
#include "localstore/disk_log.h"
#include "raft/pg_group.h"

SPDK_LOG_REGISTER_COMPONENT(osd)

bool partition_manager::get_pg_shard(uint64_t pool_id, uint64_t pg_id, uint32_t &shard_id){
    std::string name = pg_id_to_name(pool_id, pg_id);
    if(_shard_table.count(name) == 0)
        return false;

    shard_id = _shard_table[name]._shard;
    return true;
}

struct make_log_context{
    uint64_t pool_id;
    uint64_t pg_id;
    std::vector<utils::osd_info_t> osds;
    uint32_t shard_id; 
    int64_t revision_id;
    partition_manager* pm;
};

static void make_log_done(void *arg, struct disk_log* dlog, int rberrno){
    make_log_context* mlc = (make_log_context*)arg;
    partition_manager* pm = mlc->pm;

    SPDK_INFOLOG(osd, "make_log_done, rberrno %d\n", rberrno);
    if(rberrno){
        return;
    }
    auto sm = std::make_shared<osd_stm>();
    pm->add_osd_stm(mlc->pool_id, mlc->pg_id, mlc->shard_id, sm);
    pm->get_pg_group().create_pg(sm, mlc->shard_id, mlc->pool_id, mlc->pg_id, std::move(mlc->osds), dlog);
    delete mlc;
}

void partition_manager::create_pg(
        uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t> osds, 
        uint32_t shard_id, int64_t revision_id){
    make_log_context *ctx = new make_log_context{pool_id, pg_id, std::move(osds), shard_id, revision_id, this};
    make_disk_log(global_blobstore(), global_io_channel(), make_log_done, ctx);
}

int partition_manager::osd_state_is_not_active(){
    switch (_state) {
    case osd_state::OSD_STARTING:
        SPDK_WARNLOG("%s.\n", err::string_status(err::OSD_STARTING));
        return err::OSD_STARTING;
    case osd_state::OSD_ACTIVE:
        return 0;
    case osd_state::OSD_DOWN:
        SPDK_WARNLOG("%s.\n", err::string_status(err::OSD_DOWN));
        return err::OSD_DOWN;
    default:
        SPDK_WARNLOG("unknown osd state.\n");
        return -1;
    }
    return  0;
}

void partition_manager::start(utils::context *complete){
    _pgs.start(
      [this, complete](void *, int res){
        set_osd_state(osd_state::OSD_ACTIVE);
        complete->complete(res);
      },
      nullptr
    );
}

int partition_manager::create_partition(
        uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t>&& osds, int64_t revision_id){
    int state = osd_state_is_not_active();
    if(state != 0)
        return state;
    auto shard_id = get_next_shard_id();
    _add_pg_shard(pool_id, pg_id, shard_id, revision_id);

    return _shard.invoke_on(
      shard_id, 
      [this, pool_id, pg_id, osds = std::move(osds), revision_id, shard_id](){
        SPDK_INFOLOG(osd, "create pg in core %u  shard_id %u pool_id %lu pg_id %lu \n", 
            spdk_env_get_current_core(), shard_id, pool_id, pg_id);
        create_pg(pool_id, pg_id, std::move(osds), shard_id, revision_id);                
      });
}

void partition_manager::delete_pg(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id){
    _pgs.delete_pg(shard_id, pool_id, pg_id);
    del_osd_stm(pool_id, pg_id, shard_id);
}

int partition_manager::delete_partition(uint64_t pool_id, uint64_t pg_id){
    uint32_t shard_id;
    int state = osd_state_is_not_active();
    if(state != 0)
        return state;

    if(!get_pg_shard(pool_id, pg_id, shard_id)){
        return -1;
    }
    
    _remove_pg_shard(pool_id, pg_id);
    return _shard.invoke_on(
      shard_id, 
      [this, pool_id, pg_id, shard_id](){
        SPDK_INFOLOG(osd, "delete pg in core %u shard_id %u pool_id %lu pg_id %lu \n", 
            spdk_env_get_current_core(), shard_id, pool_id, pg_id);
        delete_pg(pool_id, pg_id, shard_id);                   
      });
}

int partition_manager::change_pg_membership(uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t> new_osds, utils::context* complete){
    uint32_t shard_id;
    int state = osd_state_is_not_active();
    if(state != 0){
        if(complete)
            complete->complete(state);
        return state;
    }

    if(!get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_WARNLOG("not found pg %lu.%lu\n", pool_id, pg_id);
        if(complete)
            complete->complete(err::RAFT_ERR_NOT_FOUND_PG);
        return err::RAFT_ERR_NOT_FOUND_PG;
    }

    return _shard.invoke_on(
      shard_id, 
      [this, pool_id, pg_id, shard_id, new_osds = std::move(new_osds), complete]() mutable{
        SPDK_INFOLOG(osd, "change pg membership in core %u shard_id %u pool_id %lu pg_id %lu \n", 
            spdk_env_get_current_core(), shard_id, pool_id, pg_id);
        std::vector<raft_node_info> osd_infos;
        for(auto& new_osd : new_osds){
            raft_node_info osd_info;
            osd_info.set_node_id(new_osd.node_id);
            osd_info.set_addr(new_osd.address);
            osd_info.set_port(new_osd.port);
            osd_infos.emplace_back(std::move(osd_info));
        }
        get_pg_group().change_pg_membership(shard_id, pool_id, pg_id, std::move(osd_infos), complete);                  
      });    

}