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

#include "kv_store.h"
#include "utils/utils.h"

struct make_kvs_ctx {
    rolling_blob* rblob;

    make_kvs_complete cb_fn;
    void* arg;
    spdk_blob_id checkpoint_blob_id;
    spdk_blob_id new_checkpoint_blob_id;
    struct kvstore* kvs;

    uint32_t shard_id;
};

static void
make_kvstore_sync_done(void *arg, int kverrno) {
  struct make_kvs_ctx *ctx = (struct make_kvs_ctx *)arg;

  if (kverrno) {
      SPDK_ERRLOG_EX("make_kvstore failed. error:%s\n", spdk_strerror(kverrno));
      ctx->cb_fn(ctx->arg, nullptr, kverrno);
      delete ctx;
      return;
  }

  struct kvstore* kvs = new kvstore(ctx->rblob, ctx->shard_id);
  ctx->cb_fn(ctx->arg, kvs, 0);
  delete ctx;
}

void
make_kvstore_blob_done(void *arg, struct rolling_blob* rblob, int kverrno) {
  struct make_kvs_ctx *ctx = (struct make_kvs_ctx *)arg;
  // uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
  kv_xattr xattr{.shard_id = ctx->shard_id};

  if (kverrno) {
      SPDK_ERRLOG_EX("make_kvstore failed. error:%s\n", spdk_strerror(kverrno));
      ctx->cb_fn(ctx->arg, nullptr, kverrno);
      delete ctx;
      return;
  }

  ctx->rblob = rblob;
  xattr.blob_set_xattr(rblob->blob);
  spdk_blob_sync_md(rblob->blob, make_kvstore_sync_done, ctx);
}

void make_kvstore(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
                   make_kvs_complete cb_fn, void* arg)
{
  uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
  auto make_done = [shard_id, cb_fn = std::move(cb_fn)](void *arg, struct kvstore* kvs, int kverrno){
      core_sharded::get_core_sharded().invoke_on(
        shard_id,
        [cb_fn = std::move(cb_fn), arg, kvs, kverrno](){
          SPDK_INFOLOG_EX(kvlog, "make kvstore done in core %u, thread %lu\n", 
              core_sharded::get_core_sharded().this_shard_id(), utils::get_spdk_thread_id());
          if(kverrno == 0 && kvs){
            //注意：kvstore的start方法里回注册一个poller，这个poller需要在这个kv所属的core里注册
            kvs->start();
          }
          cb_fn(arg, kvs, kverrno);
        });
  };  

  struct make_kvs_ctx* ctx = new make_kvs_ctx{.cb_fn = std::move(make_done), .arg = arg, .shard_id = shard_id};
  core_sharded::get_core_sharded().invoke_on(
    utils::default_blobstore_core,
    [bs, channel, ctx](){
      make_rolling_blob(bs, channel, 4_MB, make_kvstore_blob_done, ctx);
    });
}

static void kv_replay_done(void *arg, int kverrno){
  struct make_kvs_ctx *ctx = (struct make_kvs_ctx *)arg;

  if (kverrno) {
      SPDK_ERRLOG_EX("load_kvstore failed. error:%s\n", spdk_strerror(kverrno));
      ctx->cb_fn(ctx->arg, nullptr, kverrno);
      delete ctx->kvs;
      delete ctx;
      return;
  }   

  ctx->cb_fn(ctx->arg, ctx->kvs, 0);
  delete ctx;
}

static void load_kv_md_done(void *arg, int kverrno){
  struct make_kvs_ctx *ctx = (struct make_kvs_ctx *)arg;

  if (kverrno) {
      SPDK_ERRLOG_EX("load_kvstore failed. error:%s\n", spdk_strerror(kverrno));
      ctx->cb_fn(ctx->arg, nullptr, kverrno);
      delete ctx;
      return;
  }

  // SPDK_WARNLOG_EX("load_md done.\n");
  struct kvstore* kvs = new kvstore(ctx->rblob, ctx->shard_id);
  ctx->kvs = kvs;
  kvs->set_checkpoint_blobid(ctx->checkpoint_blob_id, ctx->new_checkpoint_blob_id);
  kvs->replay(kv_replay_done, ctx);
}

static void open_rolling_blob_done(void *arg, struct rolling_blob* rblob, int kverrno){
  struct make_kvs_ctx *ctx = (struct make_kvs_ctx *)arg;

  if (kverrno) {
      SPDK_ERRLOG_EX("load_kvstore failed. error:%s\n", spdk_strerror(kverrno));
      ctx->cb_fn(ctx->arg, nullptr, kverrno);
      delete ctx;
      return;
  } 
  ctx->rblob = rblob;
  
  rblob->load_md(load_kv_md_done, ctx);
}

void load_kvstore(spdk_blob_id blob_id, spdk_blob_id checkpoint_blob_id, 
                  spdk_blob_id new_checkpoint_blob_id, struct spdk_blob_store *bs, 
                  struct spdk_io_channel *channel,
                  make_kvs_complete cb_fn, void* arg){
  uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
  auto make_done = [shard_id, cb_fn = std::move(cb_fn)](void *arg, struct kvstore* kvs, int kverrno){
      core_sharded::get_core_sharded().invoke_on(
        shard_id,
        [cb_fn = std::move(cb_fn), arg, kvs, kverrno](){
          SPDK_INFOLOG_EX(kvlog, "load kvstore done in core %u\n", core_sharded::get_core_sharded().this_shard_id());
          if(kverrno == 0 && kvs){
            //注意：kvstore的start方法里回注册一个poller，这个poller需要在这个kv所属的core里注册
            kvs->start();
          }
          cb_fn(arg, kvs, kverrno);
        });
  };  

  struct make_kvs_ctx* ctx = new make_kvs_ctx{
                                  .cb_fn = std::move(make_done), 
                                  .arg = arg,
                                  .checkpoint_blob_id = checkpoint_blob_id,
                                  .new_checkpoint_blob_id = new_checkpoint_blob_id,
                                  .shard_id = shard_id};
  core_sharded::get_core_sharded().invoke_on(
    utils::default_blobstore_core,
    [blob_id, bs, channel, ctx](){
      open_rolling_blob(blob_id, bs, channel, open_rolling_blob_done, ctx);
    });
}