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

#include "kv_checkpoint.h"

SPDK_LOG_REGISTER_COMPONENT(kvlog)

void kv_checkpoint::start_checkpoint(size_t size, checkpoint_op_complete cb_fn, void* arg) {
    if (_new_blob.blobid) {
        cb_fn(arg, -EBUSY);
        return;
    }
    // SPDK_NOTICELOG("start_checkpoint blobstore:%p\n", _bs);
    struct checkpoint_ctx* ctx = new checkpoint_ctx();
    ctx->kv_ckpt = this;
    ctx->bs = _bs;
    ctx->cb_fn = std::move(cb_fn);
    ctx->arg = arg;
    ctx->type = blob_type::kv_checkpoint_new;
    ctx->shard_id = core_sharded::get_core_sharded().this_shard_id();
    struct spdk_blob_opts opts;
    spdk_blob_opts_init(&opts, sizeof(opts));
    // 申请空间时，blob的cluster个数要向上取整
    opts.num_clusters = SPDK_CEIL_DIV(size, spdk_bs_get_cluster_size(_bs));
    char *xattr_names[] = {"type", "shard"};
    opts.xattrs.count = SPDK_COUNTOF(xattr_names);
    opts.xattrs.names = xattr_names;
    opts.xattrs.ctx = ctx;
    opts.xattrs.get_value = kv_get_xattr_value;
    spdk_bs_create_blob_ext(_bs, &opts, new_blob_create_complete, ctx);
}