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

#include "localstore/rolling_blob.h"
#include "localstore/spdk_buffer.h"

#define BLOB_CLUSTERS 4
#define CLUSTER_SIZE 1 << 20 // 1Mb

// 每次写8个units，就是4k
#define BLOCK_UNITS 8

#define WRITE_ITERATIONS 100000

static const char *g_bdev_name = NULL;

/*
 * We'll use this struct to gather housekeeping hello_context to pass between
 * our events and callbacks.
 */
struct hello_context_t {
  struct spdk_blob_store *bs;
  struct spdk_blob *blob;
  spdk_blob_id blobid;

  struct spdk_io_channel *channel;
  char* bdev_name;

  uint64_t io_unit_size;
  uint64_t cluster_size;
  int blob_size;
  int write_size;
  int block_num;


  rolling_blob* rblob;

  int rc;
};


struct write_ctx_t {
  struct hello_context_t* hello_ctx;

  rolling_blob* rblob;
  buffer_list bl;

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
    delete hello_context->rblob;
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
    SPDK_ERRLOG("%s (err %d %s)\n", msg, objerrno, spdk_strerror(objerrno));
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

/*
 * Callback routine for the deletion of a blob.
 */
static void
delete_complete(void *arg, int bserrno)
{
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  SPDK_NOTICELOG("delete complete\n");
  if (bserrno) {
    unload_bs(hello_context, "Error in delete completion",
        bserrno);
    return;
  }

  unload_bs(hello_context, "", 0);
}

static void
delete_blob(void *arg, int bserrno)
{
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  SPDK_NOTICELOG("delete blob\n");
  if (bserrno) {
    unload_bs(hello_context, "Error in delete blob", bserrno);
    return;
  }

  spdk_bs_delete_blob(hello_context->bs, hello_context->blobid,
          delete_complete, hello_context);
}

static void
close_blob(struct hello_context_t* hello_context)
{
  spdk_blob_close(hello_context->blob, delete_blob, hello_context);
}




/********************************************************************/
static void
rolling_append_continue(void *arg, rblob_rw_result result, int objerrno) {
  struct write_ctx_t *ctx = (struct write_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  if (objerrno) {
    unload_bs(hello_context, "Error in rolling append continue completion", objerrno);
    return;
  }

  ctx->idx++;
  if (ctx->idx < ctx->max) {
      if (ctx->idx % 1000 == 0) {
          ctx->rblob->trim_back(65536000, [](void*, int){}, nullptr);
      }

      //   SPDK_NOTICELOG("append length:%d\n", ctx->bl.bytes());
      ctx->rblob->append(ctx->bl, rolling_append_continue, ctx);
  } else {
      uint64_t now = spdk_get_ticks();
      double us = env_ticks_to_usecs(now - ctx->start);
      SPDK_NOTICELOG("iterates write %d block, total time: %lf us\n", ctx->idx, us);

      free_buffer_list(ctx->bl);
      delete ctx;
      close_blob(hello_context);
  }
}

static void
rolling_append_iterates(struct hello_context_t* hello_context) {
  struct write_ctx_t* ctx = new write_ctx_t();

  ctx->hello_ctx = hello_context;
  ctx->rblob = hello_context->rblob;
  ctx->idx = 0;
  ctx->max = WRITE_ITERATIONS;
  ctx->bl = make_buffer_list(16);
  ctx->start = spdk_get_ticks();

//   SPDK_NOTICELOG("append length:%d\n", ctx->bl.bytes());
//   ctx->rblob->make_test();
  ctx->rblob->append(ctx->bl, rolling_append_continue, ctx);
}
/********************************************************************/



static void
open_blob_done(void *arg, struct spdk_blob *blob, int bserrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (bserrno) {
    unload_bs(hello_context, "Error in open completion", bserrno);
    return;
  }

  hello_context->blob = blob;
  hello_context->rblob = new rolling_blob(blob, hello_context->channel, rolling_blob::huge_blob_size);

  rolling_append_iterates(hello_context);
}

static void
create_blob_done(void *arg, spdk_blob_id blobid, int bserrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (bserrno) {
    unload_bs(hello_context, "Error in blob create callback", bserrno);
    return;
  }

  hello_context->blobid = blobid;

  spdk_bs_open_blob(hello_context->bs, blobid,
        open_blob_done, hello_context);
}

static void
create_blob(struct hello_context_t *hello_context) {
  struct spdk_blob_opts opts;
  spdk_blob_opts_init(&opts, sizeof(opts));
  opts.num_clusters = rolling_blob::huge_blob_size / hello_context->cluster_size;
  spdk_bs_create_blob_ext(hello_context->bs, &opts, create_blob_done, hello_context);
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
    unload_bs(hello_context, "Error initing the blobstore", bserrno);
    return;
  }

  hello_context->bs = bs;
  SPDK_NOTICELOG("blobstore: %p\n", hello_context->bs);
  hello_context->channel = spdk_bs_alloc_io_channel(hello_context->bs);

  hello_context->io_unit_size = spdk_bs_get_io_unit_size(hello_context->bs);
  hello_context->cluster_size = spdk_bs_get_cluster_size(hello_context->bs);
  hello_context->blob_size = BLOB_CLUSTERS * CLUSTER_SIZE;
  hello_context->write_size = hello_context->io_unit_size * BLOCK_UNITS;
  hello_context->block_num = hello_context->blob_size / hello_context->write_size;

  free = spdk_bs_free_cluster_count(hello_context->bs);
  SPDK_NOTICELOG("blobstore has FREE clusters of %" PRIu64 "\n", free);
  
  create_blob(hello_context);
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

  SPDK_NOTICELOG("start create bs_dev %s\n", g_bdev_name);

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

  // hello_context = new hello_context_t();
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
