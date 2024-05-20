#include "disk_log.h"
#include "utils/log.h"

struct make_disklog_ctx {
    std::string pg;
    rolling_blob* rblob;

    make_disklog_complete cb_fn;
    void* arg;
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

  SPDK_NOTICELOG_EX("make_disklog pg:%s success.\n", ctx->pg.c_str());
  struct disk_log* dlog = new disk_log(ctx->rblob);
  ctx->cb_fn(ctx->arg, dlog, 0);
  delete ctx;
}

void
make_disklog_blob_done(void *arg, struct rolling_blob* rblob, int logerrno) {
  struct make_disklog_ctx *ctx = (struct make_disklog_ctx *)arg;
  uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
  log_xattr xattr{.shard_id = shard_id, .pg = ctx->pg};

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
    std::string pg, make_disklog_complete cb_fn, void* arg)
{
  struct make_disklog_ctx* ctx = new make_disklog_ctx{.pg = pg, .cb_fn = cb_fn, .arg = arg};
  make_rolling_blob(bs, channel, rolling_blob::huge_blob_size, make_disklog_blob_done, ctx);
}

disk_log* make_disk_log(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
                    struct spdk_blob* blob){
    
    struct rolling_blob* rblob = make_rolling_blob(bs, channel, blob);
    struct disk_log* dlog = new disk_log(rblob);
    return dlog;
}