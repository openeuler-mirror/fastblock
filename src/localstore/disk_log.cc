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

#include "disk_log.h"
#include "utils/log.h"

SPDK_LOG_REGISTER_COMPONENT(disk_log)

struct make_disklog_ctx {
    std::string pg;
    rolling_blob* rblob;

    make_disklog_complete cb_fn;
    void* arg;

    uint32_t shard_id;
};

static void
make_disklog_sync_done(void *arg, int logerrno) {
  struct make_disklog_ctx *ctx = (struct make_disklog_ctx *)arg;

  if (logerrno) {
      SPDK_ERRLOG_EX("make_disklog failed. error:%s\n", spdk_strerror(logerrno));
      ctx->cb_fn(ctx->arg, nullptr, logerrno);
      delete ctx;
      return;
  }

  SPDK_INFOLOG_EX(disk_log, "make_disklog pg:%s trim_back, in core %u thread %lu .\n", ctx->pg.c_str(),
      core_sharded::get_core_sharded().this_shard_id(), utils::get_spdk_thread_id());
  struct disk_log* dlog = new disk_log(ctx->rblob);
  ctx->cb_fn(ctx->arg, dlog, 0);
  delete ctx;
}

void
make_disklog_blob_done(void *arg, struct rolling_blob* rblob, int logerrno) {
  struct make_disklog_ctx *ctx = (struct make_disklog_ctx *)arg;
  // uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
  log_xattr xattr{.shard_id = ctx->shard_id, .pg = ctx->pg};

  if (logerrno) {
      SPDK_ERRLOG_EX("make_disk_log failed. error:%s\n", spdk_strerror(logerrno));
      ctx->cb_fn(ctx->arg, nullptr, logerrno);
      delete ctx;
      return;
  }

  ctx->rblob = rblob;
  xattr.blob_set_xattr(rblob->blob);
  spdk_blob_sync_md(rblob->blob, make_disklog_sync_done, ctx);
}

void make_disk_log(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
    std::string pg, make_disklog_complete cb_fn, void* arg, uint32_t shard_id)
{
  auto make_done = [shard_id, cb_fn = std::move(cb_fn), pg](void *arg, struct disk_log* dlog, int rerrno){
      core_sharded::get_core_sharded().invoke_on(
        shard_id,
        [cb_fn = std::move(cb_fn), arg, rerrno, dlog, pg = std::move(pg)](){
          if(rerrno == 0 && dlog) {
            SPDK_INFOLOG_EX(disk_log, "start disklog pg:%s , in core %u thread %lu .\n", pg.c_str(),
                core_sharded::get_core_sharded().this_shard_id(), utils::get_spdk_thread_id());  
            dlog->start();          
          }
          cb_fn(arg, dlog, rerrno);
        });
  };  

  struct make_disklog_ctx* ctx = new make_disklog_ctx{.pg = pg, .cb_fn = std::move(make_done), .arg = arg, .shard_id = shard_id};
  core_sharded::get_core_sharded().invoke_on(
    utils::default_blobstore_core,
    [bs, channel, ctx](){
      make_rolling_blob(bs, channel, rolling_blob::huge_blob_size, make_disklog_blob_done, ctx);
    });
}

disk_log* make_disk_log(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
                    struct spdk_blob* blob){
    
    struct rolling_blob* rblob = make_rolling_blob(bs, channel, blob);
    struct disk_log* dlog = new disk_log(rblob);
    dlog->start();
    return dlog;
}