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
    std::vector<utils::osd_info_t> osds;
    uint32_t shard_id; 
    int64_t revision_id;
    partition_manager* pm;
    pm_complete cb_fn;
    void *arg;
};

static void make_log_done(void *arg, struct disk_log* dlog, int rberrno){
    make_log_context* mlc = (make_log_context*)arg;

    if(rberrno){
        SPDK_ERRLOG_EX("make_disk_log failed: %s\n", spdk_strerror(rberrno));
        mlc->cb_fn(mlc->arg, rberrno);
        delete mlc;
        return;
    }

    partition_manager* pm = mlc->pm;
    auto sm = std::make_shared<osd_stm>();
    auto pg = pg_id_to_name(mlc->pool_id, mlc->pg_id);
    sm->set_pg(pg);
    pm->add_osd_stm(mlc->pool_id, mlc->pg_id, mlc->shard_id, sm);
    pm->get_pg_group().create_pg(sm, mlc->shard_id, mlc->pool_id, mlc->pg_id, std::move(mlc->osds), 
            dlog, pm->get_mon_client());
    mlc->cb_fn(mlc->arg, rberrno);
    delete mlc;
}

void partition_manager::create_pg(
        uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t> osds, 
        uint32_t shard_id, int64_t revision_id, pm_complete cb_fn, void *arg){
    make_log_context *ctx = new make_log_context{.pool_id = pool_id, .pg_id = pg_id, .osds = std::move(osds), 
                    .shard_id = shard_id, .revision_id = revision_id, .pm = this, .cb_fn = std::move(cb_fn), .arg = arg};
    std::string pg = pg_id_to_name(pool_id, pg_id);   
    make_disk_log(global_blobstore(), global_io_channel(), pg, make_log_done, ctx);
}

int partition_manager::osd_state_is_not_active(){
    switch (_state) {
    case osd_state::OSD_STARTING:
        SPDK_WARNLOG_EX("%s.\n", err::string_status(err::OSD_STARTING));
        return err::OSD_STARTING;
    case osd_state::OSD_ACTIVE:
        return 0;
    case osd_state::OSD_DOWN:
        SPDK_WARNLOG_EX("%s.\n", err::string_status(err::OSD_DOWN));
        return err::OSD_DOWN;
    default:
        SPDK_WARNLOG_EX("unknown osd state.\n");
        return err::RAFT_ERR_UNKNOWN;
    }
    return  0;
}

struct pm_arg{
    utils::context *complete;
    int res;
};

void pm_start_done(void *arg){
    pm_arg *pm = (pm_arg *)arg;
    pm->complete->complete(pm->res);
    delete pm;
}

void partition_manager::start(utils::context *complete, std::shared_ptr<monitor::client> mon_client){
    _mon_client = mon_client;
    auto cur_thread = spdk_get_thread();
    _pgs.start(
      [this, complete, cur_thread](void *, int res){
        set_osd_state(osd_state::OSD_ACTIVE);
        auto c_thread = spdk_get_thread();
        if(cur_thread != c_thread){
            pm_arg *arg = new pm_arg{.complete = complete, .res = res};
            spdk_thread_send_msg(cur_thread, pm_start_done, arg);
        }else{
            complete->complete(res);
        }
      },
      nullptr
    );
}

struct partition_op_ctx{
    pm_complete cb_fn;
    void *arg;
    int perrno;
    uint64_t pool_id;
    uint64_t pg_id;
    uint32_t shard_id;
    int64_t revision_id;
    partition_manager* pm;
    /* 
     1 表示要创建pg
     0 表示要删除pg
    */
    int op;
};

static void partition_op_done(void* arg){
    partition_op_ctx* ctx = (partition_op_ctx *)arg;
    SPDK_INFOLOG_EX(osd, "partition_op_done, ctx->perrno %d pg %lu.%lu shard_id %u op %d\n", ctx->perrno, 
            ctx->pool_id, ctx->pg_id, ctx->shard_id, ctx->op);
    if(ctx->perrno == 0){
        if(ctx->op == 0)
            ctx->pm->remove_pg_shard(ctx->pool_id, ctx->pg_id);
        else 
            ctx->pm->add_pg_shard(ctx->pool_id, ctx->pg_id, ctx->shard_id, ctx->revision_id);
    }
    ctx->cb_fn(ctx->arg, ctx->perrno);
    delete ctx;
}

int partition_manager::create_partition(
        uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t>&& osds, 
        int64_t revision_id, pm_complete&& cb_fn, void *arg){
    int state = osd_state_is_not_active();
    if(state != 0){
        cb_fn(arg, state);
        return state;
    }

    auto shard_id = get_next_shard_id();

    auto cur_thread = spdk_get_thread();
    partition_op_ctx* ctx = new partition_op_ctx{.cb_fn = std::move(cb_fn), .arg = arg, .pool_id = pool_id,
                    .pg_id = pg_id, .shard_id = shard_id, .revision_id = revision_id, .pm = this, .op = 1};

    auto create_pg_done = [cur_thread](void *arg, int perrno){
        partition_op_ctx* ctx = (partition_op_ctx *)arg;
        ctx->perrno = perrno;
        spdk_thread_send_msg(cur_thread, partition_op_done, arg);
    };

    return _shard.invoke_on(
      shard_id,
      [this, pool_id, pg_id, osds = std::move(osds), revision_id, shard_id, ctx, create_pg_done = std::move(create_pg_done)](){
        SPDK_INFOLOG_EX(osd, "create pg in core %u  shard_id %u pool_id %lu pg_id %lu \n",
            spdk_env_get_current_core(), shard_id, pool_id, pg_id);
        create_pg(pool_id, pg_id, std::move(osds), shard_id, revision_id, std::move(create_pg_done), ctx);
      });
}

void partition_manager::load_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id, struct spdk_blob* blob,
                            object_store::container objects, pm_complete cb_fn, void *arg){
    auto dlog = make_disk_log(global_blobstore(), global_io_channel(), blob);
    // TODO:为什么要先创建osd_stm，然后再load呢？直接创建的时候构造object_store不可以吗？
    auto sm = std::make_shared<osd_stm>();

    get_pg_group().load_pg(sm, shard_id, pool_id, pg_id, dlog, 
      [this, cb_fn = std::move(cb_fn), objects = std::move(objects), sm, pool_id, pg_id, shard_id](void *arg, int lerrno){
        if(lerrno != 0){
            cb_fn(arg, lerrno);
            return;
        }
        // note: 直接把pg string放进object里，后面不用再传了
        std::string pg = pg_id_to_name(pool_id, pg_id);
        SPDK_INFOLOG_EX(osd, "[test] create pg:%s!\n", pg.c_str());
        sm->set_pg(pg);
        sm->load_object(std::move(objects));
        add_osd_stm(pool_id, pg_id, shard_id, sm);
        SPDK_INFOLOG_EX(osd, "load pg done\n");
        cb_fn(arg, lerrno);
      }, 
      arg, _mon_client);
}

int partition_manager::load_partition(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id, struct spdk_blob* blob, 
                            object_store::container objects, pm_complete&& cb_fn, void *arg){
    auto cur_thread = spdk_get_thread();
    partition_op_ctx* ctx = new partition_op_ctx{.cb_fn = std::move(cb_fn), .arg = arg, .pool_id = pool_id, 
                    .pg_id = pg_id, .shard_id = shard_id, .revision_id = 0, .pm = this, .op = 1};

    auto load_pg_done = [cur_thread](void *arg, int perrno){
        partition_op_ctx* ctx = (partition_op_ctx *)arg;
        ctx->perrno = perrno;
        spdk_thread_send_msg(cur_thread, partition_op_done, arg);
    };

    return _shard.invoke_on(
      shard_id,
      [this, pool_id, pg_id, blob, shard_id, ctx, load_pg_done = std::move(load_pg_done), objects = std::move(objects)](){
        SPDK_INFOLOG_EX(osd, "load pg in core %u  shard_id %u pg %lu.%lu \n", 
            spdk_env_get_current_core(), shard_id, pool_id, pg_id);
        load_pg(shard_id, pool_id, pg_id, blob, std::move(objects), std::move(load_pg_done), ctx);        
    });
}

void partition_manager::delete_pg(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id, pm_complete cb_fn, void *arg){
    _pgs.delete_pg(
      shard_id, 
      pool_id, 
      pg_id, 
      [this, cb_fn = std::move(cb_fn), pool_id, pg_id, shard_id](void *arg, int lerrno){
        SPDK_INFOLOG_EX(osd, "delete pg %lu.%lu done, rberrno %d\n", pool_id, pg_id, lerrno);  
        if(lerrno != 0){
            cb_fn(arg, lerrno);
            return;
        }       

        auto stm = get_osd_stm(shard_id, pool_id, pg_id);
        if(!stm){
            cb_fn(arg, 0);
            return;
        }
        auto destroy_objects = [this, pool_id, pg_id, shard_id, cb_fn = std::move(cb_fn)](void* arg, int rberrno){
            SPDK_INFOLOG_EX(osd, "delete pg %lu.%lu object done, rberrno %d\n", pool_id, pg_id, rberrno);   
            if(rberrno == 0){
                del_osd_stm(pool_id, pg_id, shard_id);
            }
            
            cb_fn(arg, rberrno);
        };
        stm->destroy_objects(std::move(destroy_objects), arg);
      },
      arg);
}

int partition_manager::delete_partition(uint64_t pool_id, uint64_t pg_id, pm_complete&& cb_fn, void *arg){
    uint32_t shard_id;
    int state = osd_state_is_not_active();
    if(state != 0){
        cb_fn(arg, state);
        return state;
    }

    if(!get_pg_shard(pool_id, pg_id, shard_id)){
        cb_fn(arg, 0);
        return 0;
    }

    auto cur_thread = spdk_get_thread();
    partition_op_ctx* ctx = new partition_op_ctx{.cb_fn = std::move(cb_fn), .arg = arg, .pool_id = pool_id,
                    .pg_id = pg_id, .shard_id = shard_id, .revision_id = 0, .pm = this, .op = 0};

    auto delete_pg_done = [cur_thread](void *arg, int perrno){
        partition_op_ctx* ctx = (partition_op_ctx *)arg;
        ctx->perrno = perrno;
        spdk_thread_send_msg(cur_thread, partition_op_done, arg);
    };

    return _shard.invoke_on(
      shard_id,
      [this, pool_id, pg_id, shard_id, ctx, delete_pg_done = std::move(delete_pg_done)](){
        SPDK_INFOLOG_EX(osd, "delete pg in core %u shard_id %u pool_id %lu pg_id %lu \n",
            spdk_env_get_current_core(), shard_id, pool_id, pg_id);
        delete_pg(pool_id, pg_id, shard_id, std::move(delete_pg_done), ctx);
      });
}

void partition_manager::active_pg(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id, pm_complete cb_fn, void *arg){
    _pgs.active_pg(shard_id, pool_id, pg_id);
}

int partition_manager::active_partition(uint64_t pool_id, uint64_t pg_id, pm_complete&& cb_fn, void *arg) {
    uint32_t shard_id;
    int state = osd_state_is_not_active();
    if(state != 0){
        cb_fn(arg, state);
        return state;
    }

    if(!get_pg_shard(pool_id, pg_id, shard_id)){
        cb_fn(arg, 0);
        return 0;
    }    

    auto cur_thread = spdk_get_thread();
    partition_op_ctx* ctx = new partition_op_ctx{.cb_fn = std::move(cb_fn), .arg = arg, .pool_id = pool_id,
                    .pg_id = pg_id, .shard_id = shard_id, .revision_id = 0, .pm = this, .op = 1};

    auto activate_pg_done = [cur_thread](void *arg, int perrno){
        partition_op_ctx* ctx = (partition_op_ctx *)arg;
        ctx->perrno = perrno;
        spdk_thread_send_msg(cur_thread, partition_op_done, arg);
    };

    return _shard.invoke_on(
      shard_id,
      [this, pool_id, pg_id, shard_id, ctx, activate_pg_done = std::move(activate_pg_done)](){
        SPDK_INFOLOG_EX(osd, "activate pg in core %u shard_id %u pool_id %lu pg_id %lu \n",
            spdk_env_get_current_core(), shard_id, pool_id, pg_id);
        active_pg(pool_id, pg_id, shard_id, std::move(activate_pg_done), ctx);
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
        SPDK_INFOLOG_EX(osd, "not found pg %lu.%lu\n", pool_id, pg_id);
        if(complete)
            complete->complete(err::RAFT_ERR_NOT_FOUND_PG);
        return err::RAFT_ERR_NOT_FOUND_PG;
    }

    return _shard.invoke_on(
      shard_id, 
      [this, pool_id, pg_id, shard_id, new_osds = std::move(new_osds), complete]() mutable{
        SPDK_INFOLOG_EX(osd, "change pg membership in core %u shard_id %u pool_id %lu pg_id %lu \n", 
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
