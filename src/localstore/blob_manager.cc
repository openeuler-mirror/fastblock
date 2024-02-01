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

#include "blob_manager.h"

#include <spdk/log.h>
#include <spdk/string.h>
#include <functional>

struct blobstore_manager {
	struct spdk_blob_store* blobstore;
	struct spdk_io_channel* channel;
};

static blobstore_manager g_bs_mgr;

/// TODO(sunyifang): 现在都是单核的
struct spdk_blob_store* global_blobstore() {
    return g_bs_mgr.blobstore;
}

struct spdk_io_channel* global_io_channel() {
    return g_bs_mgr.channel;
}

static void
fb_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
       void *event_ctx)
{
  SPDK_WARNLOG("Unsupported bdev event now. type: %d\n", type);
}


struct bm_context {
  bm_complete cb_fn;
  void*       args;
};

static void
bs_init_complete(void *args, struct spdk_blob_store *bs, int bserrno)
{
  struct bm_context *ctx = (struct bm_context *)args;
  uint64_t free = 0;

  SPDK_NOTICELOG("bs_init complete\n");
  if (bserrno) {
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  g_bs_mgr.blobstore = bs;
  g_bs_mgr.channel = spdk_bs_alloc_io_channel(bs);
  SPDK_NOTICELOG("blobstore:%p io_channel:%p\n", bs, g_bs_mgr.channel);

  free = spdk_bs_free_cluster_count(bs);
  SPDK_NOTICELOG("blobstore has FREE clusters of %lu\n", free);

  ctx->cb_fn(ctx->args, 0);
  delete ctx;
}

void
blobstore_init(const char *bdev_name, bm_complete cb_fn, void* args) {
  struct spdk_bs_dev *bs_dev = NULL;

  SPDK_NOTICELOG("create bs_dev\n");
  int rc = spdk_bdev_create_bs_dev_ext(bdev_name, fb_event_cb, NULL, &bs_dev);
  if (rc != 0) {
    SPDK_ERRLOG("Could not create blob bdev, %s!!\n", spdk_strerror(-rc));
    spdk_app_stop(-1);
    return;
  }

  auto ctx = new bm_context{cb_fn, args};
  SPDK_NOTICELOG("bs_init\n");
  spdk_bs_init(bs_dev, NULL, bs_init_complete, ctx);
}


void
blobstore_fini(bm_complete cb_fn, void* args)
{
	SPDK_NOTICELOG("blobstore_fini.\n");
	if (g_bs_mgr.blobstore) {
		if (g_bs_mgr.channel) {
      SPDK_NOTICELOG("free io_channel\n");
			spdk_bs_free_io_channel(g_bs_mgr.channel);
		}
		spdk_bs_unload(g_bs_mgr.blobstore, cb_fn, args);
	}
}