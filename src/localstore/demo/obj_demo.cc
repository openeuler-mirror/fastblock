#include <spdk/stdinc.h>
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/blob_bdev.h>
#include <spdk/blob.h>
#include <spdk/log.h>
#include <spdk/string.h>

#include <time.h>
#include <stdlib.h>
#include <string>

#include "localstore/object_store.h"

#define BLOB_CLUSTERS 4
#define CLUSTER_SIZE 1 << 20 // 1Mb

// 每次写8个units，就是4k
#define BLOCK_UNITS 8

#define WRITE_ITERATIONS 100

/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
struct hello_context_t {
  struct spdk_blob_store *bs;
  struct spdk_io_channel *channel;
  char* bdev_name;

  uint64_t io_unit_size;
  int blob_size;
  int write_size;
  int block_num;

  object_store* omgr;
  int rc;
};


struct write_ctx_t {
  struct hello_context_t* hello_ctx;
  object_store* omgr;
  char *write_buff;

  int idx, max;
  uint64_t start;
};

inline std::string rand_str(int len) {
  std::string str(len, '0');
  static char t[63] = {"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"};
  for(int i = 0; i < len; i++) {
    str[i] = t[rand() % 62];
  }
  return str;
}

static inline double env_ticks_to_secs(uint64_t j)
{
	return (double)j / spdk_get_ticks_hz();
}

static inline double env_ticks_to_msecs(uint64_t j)
{
	return env_ticks_to_secs(j) * 1000;
}

static inline double env_ticks_to_usecs(uint64_t j)
{
	return env_ticks_to_secs(j) * 1000 * 1000;
}

static void
write_blob_iterates(struct hello_context_t *hello_context);


uint64_t create_blob_tsc, create_snap_tsc;

uint64_t write_blob_tsc, write_snap_tsc;

/*
 * Free up memory that we allocated.
 */
static void
hello_cleanup(struct hello_context_t *hello_context)
{
  delete hello_context->omgr;
	free(hello_context);
}

/*
 * Callback routine for the blobstore unload.
 */
static void
unload_complete(void *cb_arg, int objerrno)
{
	struct hello_context_t *hello_context = (struct hello_context_t *)cb_arg;

	SPDK_NOTICELOG("unload_complete, objerrno:%d \n", objerrno);
	if (objerrno) {
		SPDK_ERRLOG("Error %d unloading the bobstore\n", objerrno);
		hello_context->rc = objerrno;
	}

	spdk_app_stop(hello_context->rc);
}

/*
 * Unload the blobstore, cleaning up as needed.
 */
static void
unload_bs(struct hello_context_t *hello_context, char *msg, int objerrno)
{
	SPDK_NOTICELOG("unload_bs, objerrno:%d \n", objerrno);
	if (objerrno) {
		SPDK_ERRLOG("%s (err %d)\n", msg, objerrno);
		hello_context->rc = objerrno;
	}
	if (hello_context->bs) {
		if (hello_context->channel) {
      SPDK_NOTICELOG("unload_bs, free io_channel\n");
			spdk_bs_free_io_channel(hello_context->channel);
		}
		spdk_bs_unload(hello_context->bs, unload_complete, hello_context);
	} else {
		spdk_app_stop(objerrno);
	}
}

static void
obj_unload_bs(void *cb_arg, int objerrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)cb_arg;

  SPDK_NOTICELOG("obj_unload_bs, objerrno:%d \n", objerrno);
  unload_bs(hello_context, "", 0);
}

static void
object_write_continue(void *arg, int objerrno) {
  struct write_ctx_t *ctx = (struct write_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  if (objerrno) {
		unload_bs(hello_context, "Error in write blob iterates completion", objerrno);
		return;
	}

	ctx->idx++;
	if (ctx->idx < ctx->max) {
      int offset = rand() % (hello_context->block_num - 1);
      offset *= BLOCK_UNITS;
      std::string str = rand_str(20);
      SPDK_NOTICELOG("write next object:%s offset:%d length:%d\n", str.c_str(), offset, 512 * BLOCK_UNITS);
      ctx->omgr->write(rand_str(20), offset, ctx->write_buff, 512 * BLOCK_UNITS, 
                      object_write_continue, ctx);
	} else {
      uint64_t now = spdk_get_ticks();
      double us = env_ticks_to_usecs(now - ctx->start);
      SPDK_NOTICELOG("iterates write %d block, total time: %lf us\n", ctx->idx, us);

      ctx->omgr->stop(obj_unload_bs, hello_context);
      free(ctx);
      // spdk_app_stop(-1);
	}
}

static void
object_write_iterates(struct hello_context_t* hello_context) {
  struct write_ctx_t* ctx = (struct write_ctx_t*)malloc(sizeof(struct write_ctx_t));
  SPDK_NOTICELOG("arg address:%p\n", ctx);

  ctx->hello_ctx = hello_context;
  ctx->omgr = hello_context->omgr;
  ctx->idx = 0;
  ctx->max = WRITE_ITERATIONS;
  ctx->start = spdk_get_ticks();
  ctx->write_buff = (char*)spdk_malloc(512 * BLOCK_UNITS,
                  0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
                  SPDK_MALLOC_DMA);
  memset(ctx->write_buff, 0x87, 512 * BLOCK_UNITS);

  uint64_t offset = rand() % (hello_context->block_num - 1);
  offset *= hello_context->write_size + 12;
  std::string str = rand_str(20);
  SPDK_NOTICELOG("write first object:%s offset:%d length:%d\n", str.c_str(), offset, 512 * BLOCK_UNITS);
  hello_context->omgr->write(str, offset, ctx->write_buff, 512 * BLOCK_UNITS, 
                             object_write_continue, ctx);
}



/*
 * Callback function for initializing the blobstore.
 */
static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs,
		 int bserrno)
{ 
	struct hello_context_t *hello_context = (struct hello_context_t *)cb_arg;
  uint64_t free = 0;

	SPDK_NOTICELOG("init entry\n");
	if (bserrno) {
		unload_bs(hello_context, "Error initing the blobstore",
			  bserrno);
		return;
	}

	hello_context->bs = bs;
	SPDK_NOTICELOG("blobstore: %p\n", hello_context->bs);
  hello_context->channel = spdk_bs_alloc_io_channel(hello_context->bs);

  // 此处有坑，channel 必须从外部传进 object_store 才可以。
  // 不可以在 object_store 内部调用 spdk_bs_alloc_io_channel，否则bs无法正常unload
  hello_context->omgr = new object_store(bs, hello_context->channel);
  SPDK_NOTICELOG("object_store at %p created.\n", hello_context->omgr);

  hello_context->io_unit_size = spdk_bs_get_io_unit_size(hello_context->bs);
  hello_context->blob_size = BLOB_CLUSTERS * CLUSTER_SIZE;
  hello_context->write_size = hello_context->io_unit_size * BLOCK_UNITS;
  hello_context->block_num = hello_context->blob_size / hello_context->write_size;

  free = spdk_bs_free_cluster_count(hello_context->bs);
  SPDK_NOTICELOG("blobstore has FREE clusters of %" PRIu64 "\n", free);
  
	object_write_iterates(hello_context);
  // obj_unload_bs(hello_context, 0);
}

static void
base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	SPDK_WARNLOG("Unsupported bdev event: type %d\n", type);
}

/*
 * Our initial event that kicks off everything from main().
 */
static void
hello_start(void *arg1)
{
	struct hello_context_t *hello_context = (struct hello_context_t *)arg1;
	struct spdk_bs_dev *bs_dev = NULL;
	int rc;

	SPDK_NOTICELOG("entry\n");

	rc = spdk_bdev_create_bs_dev_ext(hello_context->bdev_name, base_bdev_event_cb, NULL, &bs_dev);
	if (rc != 0) {
		SPDK_ERRLOG("Could not create blob bdev, %s!!\n",
			    spdk_strerror(-rc));
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_init(bs_dev, NULL, bs_init_complete, hello_context);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;
	struct hello_context_t *hello_context = NULL;

	SPDK_NOTICELOG("entry\n");

	spdk_app_opts_init(&opts, sizeof(opts));

	opts.name = "hello_blob";
	opts.json_config_file = argv[1];

  // hello_context = new hello_context_t();
	hello_context = (struct hello_context_t*)calloc(1, sizeof(struct hello_context_t));
	if (hello_context != NULL) {
		srand(time(0));
		hello_context->bdev_name = argv[2];
		SPDK_WARNLOG("bdev name:%s\n", hello_context->bdev_name);
		rc = spdk_app_start(&opts, hello_start, hello_context);
		if (rc) {
			SPDK_NOTICELOG("ERROR!\n");
		} else {
			SPDK_NOTICELOG("SUCCESS!\n");
		}
		/* Free up memory that we allocated */
		
	} else {
		SPDK_ERRLOG("Could not alloc hello_context struct!!\n");
		rc = -ENOMEM;
	}
	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	hello_cleanup(hello_context);
	return rc;
}
