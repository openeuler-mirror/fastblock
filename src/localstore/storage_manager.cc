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
#include "fastblock/utils/utils.h"
#include "base/shard_service.h"

#include <memory>

SPDK_LOG_REGISTER_COMPONENT(storage_log)

//记录多个核上的storage_manager
static sharded<storage_manager> g_st_mgr;

storage_manager& global_storage(uint32_t shard_id) {
    return g_st_mgr.on_shard(shard_id);
}

static void
_storage_init(utils::context *ctx, uint32_t shard_id) {
  SPDK_INFOLOG(storage_log, "storage init, thread id %lu, shard %u\n", utils::get_spdk_thread_id(),
      shard_id);
  g_st_mgr.on_shard(shard_id).start(
    shard_id,
    [shard_id](void *arg, int serror){
      utils::context *ctx = (utils::context *)arg;
      SPDK_NOTICELOG("storage init done in shard %u.\n", core_sharded::get_core_sharded().this_shard_id());
      ctx->complete(serror);
    },
    ctx
  );
}

void
storage_init(storage_op_complete cb_fn, void* arg){
  auto &shard = core_sharded::get_core_sharded();
  auto cur_thread = spdk_get_thread();

  auto shard_num = core_sharded::get_core_sharded().count();
  auto ctx = new utils::switch_core_context{.cb_fn = std::move(cb_fn), .arg = arg, .thread = cur_thread, .serror = 0};
  utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, utils::switch_core_func, ctx);
  g_st_mgr.start();

  SPDK_INFOLOG(storage_log, "storage init in thread id: %lu, shard_num %u\n",
      utils::get_spdk_thread_id(), shard_num);

  for(uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
    shard.invoke_on(
      shard_id,
      [complete, shard_id](){
        _storage_init(complete, shard_id);
      }
    );
  }
}

static void
_storage_fini(utils::context *ctx, uint32_t shard_id) {
  SPDK_INFOLOG(storage_log, "storage fini, thread id %lu, shard %u\n",
      utils::get_spdk_thread_id(), shard_id);

  if(!g_st_mgr.shard_is_started(shard_id)){
    ctx->complete(0);
    return;
  }
  g_st_mgr.on_shard(shard_id).stop(
    [shard_id](void *arg, int error){
        if (error) {
            SPDK_ERRLOG("storage_fini in shard %u, error:%s\n", shard_id, spdk_strerror(error));
        }
        utils::context *ctx = (utils::context *)arg;
        SPDK_INFOLOG(storage_log, "storage fini done in shard %u.\n", shard_id);
        ctx->complete(error);
        return;
    },
    ctx
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

  auto shard_num = core_sharded::get_core_sharded().count();
  auto ctx = new utils::switch_core_context{.cb_fn = std::move(fini_done), .arg = arg, .thread = cur_thread, .serror = 0};
  utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, utils::switch_core_func, ctx);

  for(uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
    shard.invoke_on(
      shard_id,
      [complete, shard_id](){
        _storage_fini(complete, shard_id);
      }
    );
  }
}

static void
_storage_load(utils::context *ctx, uint32_t shard_id){
  auto &blobs = global_blob_tree(shard_id);
  spdk_blob_id kv_blob_id = blobs.kv_blob;
  spdk_blob_id checkpoint_blob_id = blobs.kv_checkpoint_blob;
  spdk_blob_id new_checkpoint_blob_id = blobs.kv_new_checkpoint_blob;

  SPDK_INFOLOG(storage_log, "storage load in shard %u, kv_blob_id %lu, checkpoint_blob_id %lu, \
      new_checkpoint_blob_id %lu, thread id %lu\n",
      shard_id, kv_blob_id, checkpoint_blob_id, new_checkpoint_blob_id,
      utils::get_spdk_thread_id());

  g_st_mgr.on_shard(shard_id).load(
    shard_id,
    kv_blob_id,
    checkpoint_blob_id,
    new_checkpoint_blob_id,
    [shard_id](void *arg, int error){
      utils::context *ctx = (utils::context *)arg;
      SPDK_INFOLOG(storage_log, "storage load done in shard %u.\n", shard_id);
      ctx->complete(error);
    },
    ctx);
}

void storage_load(storage_op_complete cb_fn, void* arg){
  auto &shard = core_sharded::get_core_sharded();
  auto cur_thread = spdk_get_thread();

  auto shard_num = core_sharded::get_core_sharded().count();
  auto ctx = new utils::switch_core_context{.cb_fn = std::move(cb_fn), .arg = arg, .thread = cur_thread, .serror = 0};
  utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, utils::switch_core_func, ctx);

  g_st_mgr.start();

  for(uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
    shard.invoke_on(
      shard_id,
      [complete, shard_id](){
        _storage_load(complete, shard_id);
      }
    );
  }
}