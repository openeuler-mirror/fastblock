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
    std::vector<osd_info_t> osds;
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
        uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t> osds,
        uint32_t shard_id, int64_t revision_id){
    make_log_context *ctx = new make_log_context{pool_id, pg_id, std::move(osds), shard_id, revision_id, this};
    make_disk_log(global_blobstore(), global_io_channel(), make_log_done, ctx);
}

int partition_manager::create_partition(
        uint64_t pool_id, uint64_t pg_id, std::vector<osd_info_t>&& osds, int64_t revision_id){
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
    auto name = pg_id_to_name(pool_id, pg_id);
    _sm_table[shard_id].erase(name);

    _pgs.delete_pg(shard_id, pool_id, pg_id);
}

int partition_manager::delete_partition(uint64_t pool_id, uint64_t pg_id){
    uint32_t shard_id;

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