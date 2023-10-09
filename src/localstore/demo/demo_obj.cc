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
#include <openssl/sha.h>

#include "localstore/object_store.h"
#include "localstore/object_recovery.h"

#define BLOB_CLUSTERS 4
#define CLUSTER_SIZE 1 << 20 // 1Mb

// 每次写8个units，就是4k
#define UNIT_SIZE 512
#define BLOCK_UNITS 8 * 1024
#define WRITE_SIZE (BLOCK_UNITS * UNIT_SIZE) // 写 4 Mb

#define WRITE_ITERATIONS 100

static const char *g_bdev_name = NULL;

/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
struct hello_context_t {
  struct spdk_blob_store *bs;
  struct spdk_io_channel *channel;

  uint64_t io_unit_size;
  int blob_size;
  int write_size;
  int block_num;

  object_store* omgr;
  int rc;

  char *write_buff;
  char *read_buff;
};


struct write_ctx_t {
  struct hello_context_t* hello_ctx;
  int idx, max;
  uint64_t start;
};

struct recover_ctx_t {
  struct hello_context_t* hello_ctx;
  object_recovery* recovery;

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
  spdk_free(hello_context->write_buff);
  spdk_free(hello_context->read_buff);
  delete hello_context->omgr;
	delete hello_context;
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
unload_bs(struct hello_context_t *hello_context, const char *msg, int objerrno)
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
recovery_delete_complete(void *arg, int objerrno) {
  struct recover_ctx_t *ctx = (struct recover_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  if (objerrno ) {
    unload_bs(hello_context, "Error in write blob iterates completion", objerrno);
    return;
  } 

  uint64_t now = spdk_get_ticks();
  double us = env_ticks_to_usecs(now - ctx->start);
  SPDK_NOTICELOG("recovery delete complete, total time: %lf us\n", us);

  hello_context->omgr->stop(obj_unload_bs, hello_context);
  delete ctx->recovery;
  delete ctx;
}

static void
recovery_delete(void *arg) {
  struct recover_ctx_t *ctx = (struct recover_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  SPDK_NOTICELOG("delete recovery.\n");
  ctx->start = spdk_get_ticks();
  ctx->recovery->recovery_delete(recovery_delete_complete, ctx);
}

static void
recovery_read_continue(void *arg, int objerrno) {
  struct recover_ctx_t *ctx = (struct recover_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  if (objerrno == -ENOENT) {
    uint64_t now = spdk_get_ticks();
    double us = env_ticks_to_usecs(now - ctx->start);
    SPDK_NOTICELOG("recovery read complete, total time: %lf us\n", us);
    recovery_delete(ctx);
    return;
  }

  if (objerrno) {
    unload_bs(hello_context, "Error in read blob iterates completion", objerrno);
    return;
  }

  // 检验sha256
  unsigned char result[32];
  SHA256((const unsigned char*)hello_context->read_buff, WRITE_SIZE, result);
  SPDK_NOTICELOG("read length:%d sha256:\n", WRITE_SIZE);
  for (int i = 0; i < 32; i++) { printf("%02x ", result[i]); } printf("\n");
  memset(hello_context->write_buff, 0x33, UNIT_SIZE);

  ctx->recovery->recovery_read_iter_next(recovery_read_continue, hello_context->read_buff, ctx);
}

static void
recovery_read_iterates(void *arg) {
  struct recover_ctx_t *ctx = (struct recover_ctx_t*)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  SPDK_NOTICELOG("recovery snapshot start...\n");
  hello_context->read_buff = (char*)spdk_malloc(WRITE_SIZE,
                  0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
                  SPDK_MALLOC_DMA);
  memset(hello_context->write_buff, 0x33, UNIT_SIZE); // 随便写几个字节，等读完再看看sha256

  ctx->start = spdk_get_ticks();
  ctx->recovery->recovery_read_iter_first(recovery_read_continue, hello_context->read_buff, ctx);
}

static void
recovery_create_complete(void *arg, int objerrno) {
  struct recover_ctx_t *ctx = (struct recover_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  if (objerrno) {
    unload_bs(hello_context, "Error in recovery_create", objerrno);
    return;
  }

  uint64_t now = spdk_get_ticks();
  double us = env_ticks_to_usecs(now - ctx->start);
  SPDK_NOTICELOG("recovery create complete, total time: %lf us\n", us);

  recovery_read_iterates(ctx);
}

static void
recovery_create(struct hello_context_t* hello_context) {
  struct recover_ctx_t* ctx = (struct recover_ctx_t*)malloc(sizeof(struct recover_ctx_t));

  ctx->hello_ctx = hello_context;
  ctx->idx = 0;
  ctx->max = WRITE_ITERATIONS;
  ctx->start = spdk_get_ticks();
  ctx->recovery = new object_recovery(hello_context->omgr);

  ctx->recovery->recovery_create(recovery_create_complete, ctx);
}

static void
object_create_snap3_done(void *arg, int objerrno) {
  struct recover_ctx_t *ctx = (struct recover_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  if (objerrno) {
    unload_bs(hello_context, "Error in object_create_snap3", objerrno);
    return;
  }

  recovery_create(hello_context);
  free(ctx);
}

static void
object_create_snap3(void *arg, int objerrno) {
  struct write_ctx_t *ctx = (struct write_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;
  std::string file_name = "file1";
  hello_context->omgr->snap_create(file_name, "snap3", object_create_snap3_done, ctx);
}

static void
object_delete_snap1(void *arg, int objerrno) {
  struct write_ctx_t *ctx = (struct write_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;
  std::string file_name = "file1";
  hello_context->omgr->snap_delete(file_name, "snap1", object_create_snap3, ctx);
}

static void
object_create_snap2(void *arg, int objerrno) {
  struct write_ctx_t *ctx = (struct write_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;
  std::string file_name = "file1";
  hello_context->omgr->snap_create(file_name, "snap2", object_delete_snap1, ctx);
}

static void
object_create_snap1(void *arg) {
  struct write_ctx_t *ctx = (struct write_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;
  std::string file_name = "file1";
  hello_context->omgr->snap_create(file_name, "snap1", object_create_snap2, ctx);
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
      // int offset = rand() % (hello_context->block_num - 1);
      // offset *= BLOCK_UNITS;
      std::string str = rand_str(10);
      SPDK_NOTICELOG("write next object:%s length:%d\n", str.c_str(), WRITE_SIZE);
      hello_context->omgr->write(rand_str(20), 0, hello_context->write_buff, WRITE_SIZE, 
                      object_write_continue, ctx);
	} else {
      uint64_t now = spdk_get_ticks();
      double us = env_ticks_to_usecs(now - ctx->start);
      SPDK_NOTICELOG("iterates write %d block, total time: %lf us\n", ctx->idx, us);

      object_create_snap1(ctx);
	}
}

static void
object_write_iterates(struct hello_context_t* hello_context) {
  struct write_ctx_t* ctx = (struct write_ctx_t*)malloc(sizeof(struct write_ctx_t));
  SPDK_NOTICELOG("arg address:%p\n", ctx);

  ctx->hello_ctx = hello_context;
  ctx->idx = 0;
  ctx->max = WRITE_ITERATIONS;
  ctx->start = spdk_get_ticks();
  hello_context->write_buff = (char*)spdk_malloc(WRITE_SIZE, // 4M
                  0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
                  SPDK_MALLOC_DMA);
  memset(hello_context->write_buff, 0x87, WRITE_SIZE);

  unsigned char result[32];
  SHA256((const unsigned char*)hello_context->write_buff, WRITE_SIZE, result);
  SPDK_NOTICELOG("write length:%d sha256:\n", WRITE_SIZE);
  for (int i = 0; i < 32; i++) { printf("%02x ", result[i]); } printf("\n");

  std::string str = "file1";
  SPDK_NOTICELOG("write first object:%s length:%d\n", str.c_str(), WRITE_SIZE);
  hello_context->omgr->write(str, 0, hello_context->write_buff, WRITE_SIZE,
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

	rc = spdk_bdev_create_bs_dev_ext(g_bdev_name, base_bdev_event_cb, NULL, &bs_dev);
	if (rc != 0) {
		SPDK_ERRLOG("Could not create blob bdev, %s!!\n",
			    spdk_strerror(-rc));
		spdk_app_stop(-1);
		return;
	}

	spdk_bs_init(bs_dev, NULL, bs_init_complete, hello_context);
}

static void
demo_usage(void)
{
  printf(" -b <bdev_name>             bdev name\n");
}

static int
demo_parse_arg(int ch, char *arg)
{
  switch (ch) {
  case 'b':
    g_bdev_name = arg;
    break;
  default:
    return -EINVAL;
  }
  return 0;
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
  if ((rc = spdk_app_parse_args(argc, argv, &opts, "b:", NULL,
          demo_parse_arg, demo_usage)) !=
      SPDK_APP_PARSE_ARGS_SUCCESS) {
    exit(rc);
  }

  hello_context = new hello_context_t();
	if (hello_context != NULL) {
		srand(time(0));
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
