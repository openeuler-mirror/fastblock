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

#include "storage_manager.h"
#include "utils/utils.h"
#include "base/shard_service.h"

#include <memory>

SPDK_LOG_REGISTER_COMPONENT(storage_log)

//记录多个核上的storage_manager
static sharded<storage_manager> g_st_mgr;

storage_manager& global_storage(uint32_t shard_id) {
    return g_st_mgr.on_shard(shard_id);
}

struct storage_context{
  storage_op_complete cb_fn;
  void *arg;
  spdk_thread *thread;
  int serror;
};

static inline void _storage_op_done(void* arg){
  storage_context *ctx = (storage_context *)arg;
  ctx->cb_fn(ctx->arg, ctx->serror);
  delete ctx;
}

static void storage_op_done(void *arg, int rerrno){
  storage_context *ctx = (storage_context *)arg;
  auto cur_thread = spdk_get_thread();
  ctx->serror = rerrno;
  if(cur_thread != ctx->thread){
    spdk_thread_send_msg(ctx->thread, _storage_op_done, ctx);
  }else{
    _storage_op_done(ctx);
  }
}

static void
_storage_init(storage_op_complete cb_fn, void* arg) {
  struct spdk_bs_dev *bs_dev = NULL;
  auto shard_id = core_sharded::get_core_sharded().this_shard_id();
  SPDK_INFOLOG(storage_log, "storage init, thread id %lu, core %u\n", utils::get_spdk_thread_id(),
      shard_id);
  storage_context *ctx = new storage_context{.cb_fn = std::move(cb_fn), .arg = arg};
  g_st_mgr.on_shard(shard_id).start(
    [](void *arg, int serror){
      storage_context *ctx = (storage_context *)arg;
      SPDK_NOTICELOG("storage init done in core %u.\n", core_sharded::get_core_sharded().this_shard_id());
      ctx->cb_fn(ctx->arg, serror);
      delete ctx;
    },
    ctx
  );
}

void
storage_init(storage_op_complete cb_fn, void* arg){
  auto &shard = core_sharded::get_core_sharded();
  auto cur_thread = spdk_get_thread();

  storage_context *ctx = new storage_context{.cb_fn = std::move(cb_fn), .arg = arg, .thread = cur_thread};
  
  auto shard_num = core_sharded::get_core_sharded().count();
  utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, storage_op_done, ctx);
  // std::construct_at(&g_st_mgr);
  g_st_mgr.start();
  
  SPDK_INFOLOG(storage_log, "storage init in thread id: %lu, shard_num %u\n", 
      utils::get_spdk_thread_id(), shard_num);
  for(uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
    shard.invoke_on(
      shard_id,
      [complete](){
        _storage_init(utils::complete_done, complete);
      }
    );      
  }
}

static void
_storage_fini(storage_op_complete cb_fn, void* arg) {
  struct spdk_bs_dev *bs_dev = NULL;
  auto shard_id = core_sharded::get_core_sharded().this_shard_id();
  SPDK_INFOLOG(storage_log, "storage fini, thread id %lu, core %u\n", 
      utils::get_spdk_thread_id(), shard_id);
  storage_context *ctx = new storage_context{.cb_fn = std::move(cb_fn), .arg = arg};
  
  if(!g_st_mgr.shard_is_started(shard_id)){
    storage_context *ctx = (storage_context *)arg;
    ctx->cb_fn(ctx->arg, 0);
    delete ctx;
    return;
  }
  g_st_mgr.on_shard(shard_id).stop(
    [](void *arg, int error){
        if (error) {
            SPDK_ERRLOG("storage_fini. error:%s\n", spdk_strerror(error));
        }
        storage_context *ctx = (storage_context *)arg;
        SPDK_INFOLOG(storage_log, "storage fini done.\n");
        ctx->cb_fn(ctx->arg, error);
        delete ctx;
        return;
    }, ctx
  );
}


void
storage_fini(storage_op_complete cb_fn, void* arg){
  auto &shard = core_sharded::get_core_sharded();
  auto cur_thread = spdk_get_thread();
  
  auto fini_done = [cb_fn = std::move(cb_fn)](void *arg, int rerrno){
    g_st_mgr.stop();
    cb_fn(arg, rerrno);
  };

  storage_context *ctx = new storage_context{.cb_fn = std::move(fini_done), .arg = arg, .thread = cur_thread};
  auto shard_num = core_sharded::get_core_sharded().count();
  utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, storage_op_done, ctx);

  for(uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
    shard.invoke_on(
      shard_id,
      [complete](){
        _storage_fini(utils::complete_done, complete);
      }
    );    
  }   
}

static void 
_storage_load(storage_op_complete cb_fn, void* arg){
  uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();

  auto &blobs = global_blob_tree();
  spdk_blob_id kv_blob_id = blobs.on_shard(shard_id).kv_blob;
  spdk_blob_id checkpoint_blob_id = blobs.on_shard(shard_id).kv_checkpoint_blob;
  spdk_blob_id new_checkpoint_blob_id = blobs.on_shard(shard_id).kv_new_checkpoint_blob;

  SPDK_INFOLOG(storage_log, "storage load in core %u, kv_blob_id %lu, checkpoint_blob_id %lu, \
      new_checkpoint_blob_id %lu, thread id %lu\n",
      shard_id, kv_blob_id, checkpoint_blob_id, new_checkpoint_blob_id,
      utils::get_spdk_thread_id());

  storage_context *ctx = new storage_context{.cb_fn = std::move(cb_fn), .arg = arg};
  g_st_mgr.on_shard(shard_id).load(kv_blob_id, checkpoint_blob_id, new_checkpoint_blob_id, 
    [](void *arg, int error){
      storage_context *ctx = (storage_context *)arg;
      SPDK_INFOLOG(storage_log, "storage load done.\n");
      ctx->cb_fn(ctx->arg, error);
      delete ctx;
    }, ctx);  
}

void storage_load(storage_op_complete cb_fn, void* arg){
  auto &shard = core_sharded::get_core_sharded();
  auto cur_thread = spdk_get_thread();

  storage_context *ctx = new storage_context{.cb_fn = std::move(cb_fn), .arg = arg, .thread = cur_thread};

  auto shard_num = core_sharded::get_core_sharded().count();
  utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, storage_op_done, ctx);
  
  // std::construct_at(&g_st_mgr);
  g_st_mgr.start();

  for(uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
    shard.invoke_on(
      shard_id,
      [complete](){
        _storage_load(utils::complete_done, complete);
      }
    );       
  }
}