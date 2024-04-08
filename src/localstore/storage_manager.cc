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

#include <memory>

SPDK_LOG_REGISTER_COMPONENT(storage_log)

static storage_manager g_st_mgr;

storage_manager& global_storage() {
    return g_st_mgr;
}

struct storage_context{
  storage_op_complete cb_fn;
  void *arg;
  spdk_thread *thread;
  int serror;
};

static inline void _storage_op_done(void *arg){
  storage_context *ctx = (storage_context *)arg;
  ctx->cb_fn(ctx->arg, ctx->serror);
  delete ctx;
}

static void storage_op_done(storage_context *ctx){
  auto cur_thread = spdk_get_thread();
  if(cur_thread != ctx->thread){
    spdk_thread_send_msg(ctx->thread, _storage_op_done, ctx);
  }else{
    _storage_op_done(ctx);
  }
}

static void
_storage_init(storage_op_complete cb_fn, void* arg, spdk_thread *thread) {
  struct spdk_bs_dev *bs_dev = NULL;

  SPDK_INFOLOG(storage_log, "storage init, thread id %lu\n", utils::get_spdk_thread_id());
  std::construct_at(&g_st_mgr);
  storage_context *ctx = new storage_context{.cb_fn = std::move(cb_fn), .arg = arg, .thread = thread};
  g_st_mgr.start(
    [](void *arg, int serror){
      storage_context *ctx = (storage_context *)arg;
      ctx->serror = serror;
      SPDK_INFOLOG(storage_log, "storage init done.\n");
      storage_op_done(ctx);
    },
    ctx
  );
}

void
storage_init(storage_op_complete cb_fn, void* arg){
  auto &shard = core_sharded::get_core_sharded();
  auto cur_thread = spdk_get_thread();

  shard.invoke_on(
    0,
    [cb_fn, arg, cur_thread](){
      _storage_init(cb_fn, arg, cur_thread);
    }
  );      
}

static void
_storage_fini(storage_op_complete cb_fn, void* arg, spdk_thread *thread) {
  struct spdk_bs_dev *bs_dev = NULL;

  SPDK_INFOLOG(storage_log, "storage fini, thread id %lu\n", utils::get_spdk_thread_id());
  storage_context *ctx = new storage_context{.cb_fn = std::move(cb_fn), .arg = arg, .thread = thread};
  
  g_st_mgr.stop(
    [cb_fn = std::move(cb_fn)](void *arg, int error){
        if (error) {
            SPDK_ERRLOG("storage_fini. error:%s\n", spdk_strerror(error));
        }
        storage_context *ctx = (storage_context *)arg;
        ctx->serror = error;
        SPDK_INFOLOG(storage_log, "storage fini done.\n");
        std::destroy_at(&g_st_mgr);
        storage_op_done(ctx);
        return;
    }, ctx
  );
}


void
storage_fini(storage_op_complete cb_fn, void* arg){
  auto &shard = core_sharded::get_core_sharded();
  auto cur_thread = spdk_get_thread();

  shard.invoke_on(
    0,
    [cb_fn, arg, cur_thread](){
      _storage_fini(cb_fn, arg, cur_thread);
    }
  );       
}

static void 
_storage_load(storage_op_complete cb_fn, void* arg, spdk_thread *thread){
  uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();

  auto &blobs = global_blob_tree();
  spdk_blob_id kv_blob_id = blobs.on_shard(shard_id).kv_blob;
  spdk_blob_id checkpoint_blob_id = blobs.on_shard(shard_id).kv_checkpoint_blob;
  spdk_blob_id new_checkpoint_blob_id = blobs.on_shard(shard_id).kv_new_checkpoint_blob;

  SPDK_INFOLOG(storage_log, "storage load in core %u, kv_blob_id %lu, checkpoint_blob_id %lu, \
      new_checkpoint_blob_id %lu, thread id %lu\n",
      shard_id, kv_blob_id, checkpoint_blob_id, new_checkpoint_blob_id,
      utils::get_spdk_thread_id());

  std::construct_at(&g_st_mgr);

  storage_context *ctx = new storage_context{.cb_fn = std::move(cb_fn), .arg = arg, .thread = thread};
  g_st_mgr.load(kv_blob_id, checkpoint_blob_id, new_checkpoint_blob_id, 
    [](void *arg, int error){
      storage_context *ctx = (storage_context *)arg;
      ctx->serror = error;
      SPDK_INFOLOG(storage_log, "storage load done.\n");
      storage_op_done(ctx);
    }, ctx);  
}

void storage_load(storage_op_complete cb_fn, void* arg){
  auto &shard = core_sharded::get_core_sharded();
  auto cur_thread = spdk_get_thread();

  shard.invoke_on(
    0,
    [cb_fn, arg, cur_thread](){
      _storage_load(cb_fn, arg, cur_thread);
    }
  );       
}