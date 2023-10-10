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

#include "kv_store.h"

struct make_kvs_ctx {
    rolling_blob* rblob;

    make_kvs_complete cb_fn;
    void* arg;
};

static void 
make_kvstore_sync_done(void *arg, int kverrno) {
  struct make_kvs_ctx *ctx = (struct make_kvs_ctx *)arg;

  if (kverrno) {
      SPDK_ERRLOG("make_kvstore failed. error:%s\n", spdk_strerror(kverrno));
      ctx->cb_fn(ctx->arg, nullptr, kverrno);
      delete ctx;
      return;
  }

  struct kvstore* kvs = new kvstore(ctx->rblob);
  ctx->cb_fn(ctx->arg, kvs, 0);
  delete ctx;
}

void
make_kvstore_blob_done(void *arg, struct rolling_blob* rblob, int kverrno) {
  struct make_kvs_ctx *ctx = (struct make_kvs_ctx *)arg;
  blob_type type = blob_type::kv;
  uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();

  if (kverrno) {
      SPDK_ERRLOG("make_kvstore failed. error:%s\n", spdk_strerror(kverrno));
      ctx->cb_fn(ctx->arg, nullptr, kverrno);
      delete ctx;
      return;
  }

  ctx->rblob = rblob;

  spdk_blob_set_xattr(rblob->blob, "type", &type, sizeof(type));
  spdk_blob_set_xattr(rblob->blob, "shard", &shard_id, sizeof(shard_id));

  spdk_blob_sync_md(rblob->blob, make_kvstore_sync_done, ctx);
}

void make_kvstore(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
                   make_kvs_complete cb_fn, void* arg) 
{
  struct make_kvs_ctx* ctx;
  
  ctx = new make_kvs_ctx{.cb_fn = std::move(cb_fn), .arg = arg};
  make_rolling_blob(bs, channel, 4_MB, make_kvstore_blob_done, ctx);
}