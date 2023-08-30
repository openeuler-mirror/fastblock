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
#include <algorithm>

#include "localstore/rolling_blob.h"
#include "localstore/spdk_buffer.h"
#include "localstore/disk_log.h"

#define append_ITERATIONS 600000
#define read_ITERATIONS   100

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

  rolling_blob* rblob;
  disk_log* log;
  int rc;
};

struct test_ctx_t {
  struct hello_context_t* hello_ctx;

  disk_log* log;
  buffer_list bl;

  int read_idx, read_max;
  int append_idx, append_max;

  uint64_t append_raft_index{0};
  uint64_t read_raft_index{0};
  uint64_t apply_raft_index{0};

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

uint64_t create_blob_tsc, create_snap_tsc;

uint64_t write_blob_tsc, write_snap_tsc;

/*
 * Free up memory that we allocated.
 */
static void
hello_cleanup(struct hello_context_t *hello_context)
{
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


static void
close_complete(void *arg, int rberrno)
{
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  SPDK_NOTICELOG("delete blob\n");
  if (rberrno) {
    unload_bs(hello_context, "Error in delete blob", rberrno);
    return;
  }

  delete hello_context->log;
  unload_bs(hello_context, "", 0);
}

static void
log_stop(struct hello_context_t* hello_context)
{
  SPDK_NOTICELOG("disklog stop\n");
  hello_context->log->stop(close_complete, hello_context);
}

/********************************************************************/
static void
log_read_continue(void *arg, std::vector<log_entry_t>&& entries, int rberrno) {
  struct test_ctx_t *ctx = (struct test_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  if (rberrno) {
      SPDK_ERRLOG("Error in log read continue completion %s\n", spdk_strerror(rberrno));
      free_buffer_list(ctx->bl);
      delete ctx;
      log_stop(hello_context);
      return;
  }

  if (entries.size() == 0) {
      SPDK_ERRLOG("read entries size zero\n");
      free_buffer_list(ctx->bl);
      delete ctx;
      log_stop(hello_context);
      return;
  }

  for (auto& entry : entries) {
      SPDK_NOTICELOG("log read, index:%lu size:%lu term:%lu meta:%s, data len:%lu\n", 
                     entry.index, entry.size, entry.term_id, entry.meta.c_str(), 
                     entry.data.bytes());
      free_buffer_list(entry.data);
  }
  
  ctx->read_idx++;
  if (ctx->read_idx < ctx->read_max) {
      uint64_t start = ctx->read_raft_index++;
      uint64_t end = std::min(start + 2, ctx->apply_raft_index);
      SPDK_NOTICELOG("iterates read start:%lu end:%lu\n", start, end);
      ctx->log->read(start, end, log_read_continue, ctx);
      // ctx->log->read(0, 2, log_read_continue, ctx);
  } else {
      uint64_t now = spdk_get_ticks();
      double us = env_ticks_to_usecs(now - ctx->start);
      SPDK_NOTICELOG("iterates read %d entry, total time: %lf us\n", ctx->read_idx, us);

      free_buffer_list(ctx->bl);
      delete ctx;

      log_stop(hello_context);
  }
}

static void
log_read_iterates(struct test_ctx_t* ctx) {
  ctx->read_raft_index = append_ITERATIONS - read_ITERATIONS;
  uint64_t start = ctx->read_raft_index++;
  uint64_t end = std::min(start + 2, ctx->apply_raft_index);
  SPDK_NOTICELOG("iterates read start:%lu end:%lu\n", start, end);
  ctx->log->read(start, end, log_read_continue, ctx);
}

/********************************************************************/

static void
log_append_continue(void *arg, int rberrno) {
  struct test_ctx_t *ctx = (struct test_ctx_t *)arg;
  struct hello_context_t *hello_context = ctx->hello_ctx;

  if (rberrno) {
    unload_bs(hello_context, "Error in rolling append continue completion", rberrno);
    return;
  }

  ctx->append_idx++;
  ctx->log->advance_apply(ctx->apply_raft_index);
  if (ctx->append_idx % 5000 == 0) {
    auto dump = ctx->log->dump_state();
    SPDK_NOTICELOG("%d-th log state:%s", ctx->append_idx, dump.c_str());
  }

  if (ctx->append_idx < ctx->append_max) {
      log_entry_t entry{ .term_id = ctx->append_raft_index, 
                         .index = ctx->append_raft_index, 
                         .size = ctx->bl.bytes(), 
                         .meta = "test",
                         .data = ctx->bl};
      ctx->apply_raft_index = ctx->append_raft_index;
      ctx->append_raft_index++;
      // SPDK_NOTICELOG("log append, index:%lu size:%lu term:%lu meta:%s, data len:%lu\n", 
      //                entry.index, entry.size, entry.term_id, entry.meta.c_str(), 
      //                entry.data.bytes());
      ctx->log->append(entry, log_append_continue, ctx);
  } else {
      uint64_t now = spdk_get_ticks();
      double us = env_ticks_to_usecs(now - ctx->start);
      SPDK_NOTICELOG("iterates append %d, apply:%lu entries, total time: %lf us\n", ctx->append_idx, ctx->apply_raft_index, us);

      log_read_iterates(ctx);
  }
}

static void
log_append_iterates(struct hello_context_t* hello_context) {
  struct test_ctx_t* ctx = new test_ctx_t();

  ctx->hello_ctx = hello_context;
  ctx->log = hello_context->log;
  ctx->append_idx = 0;
  ctx->append_max = append_ITERATIONS;
  ctx->read_idx = 0;
  ctx->read_max = read_ITERATIONS;

  ctx->bl = make_buffer_list(8);
  ctx->start = spdk_get_ticks();


  std::vector<log_entry_t> entry_vec;
  for (int i = 0; i < 3; i++) {
      auto entry = entry_vec.emplace_back(log_entry_t{ .term_id = ctx->append_raft_index, 
                                    .index = ctx->append_raft_index, 
                                    .size = ctx->bl.bytes(),
                                    .meta = "meta",
                                    .data = ctx->bl});
      // SPDK_NOTICELOG("log append vector %d-th, index:%lu size:%lu term:%lu meta:%s, data len:%lu\n", 
      //             i, entry.index, entry.size, entry.term_id, entry.meta.c_str(), entry.data.bytes());
      ctx->apply_raft_index = ctx->append_raft_index;
      ctx->append_raft_index++;
  }
  ctx->log->append(entry_vec, log_append_continue, ctx);
}

/********************************************************************/

static void
make_2th_log_done(void *arg, struct rolling_blob* rblob, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
    unload_bs(hello_context, "Error in blob create callback", rberrno);
    return;
  }

  auto log = new disk_log(rblob);
  log->stop([hello_context, log] (void *, int) {
      SPDK_NOTICELOG("close second disk_log\n");
      log_append_iterates(hello_context);
      delete log;
    },
    nullptr); 
}

static void
make_log_done(void *arg, struct rolling_blob* rblob, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
    unload_bs(hello_context, "Error in blob create callback", rberrno);
    return;
  }

  hello_context->rblob = rblob;
  hello_context->log = new disk_log(rblob);

  // log_append_iterates(hello_context);
  // 打开第二个log
  make_rolling_blob(hello_context->bs, hello_context->channel, rolling_blob::huge_blob_size, 
                    make_2th_log_done, hello_context);
}

static void
bs_init_complete(void *cb_arg, struct spdk_blob_store *bs,
     int rberrno)
{ 
  struct hello_context_t *hello_context = (struct hello_context_t *)cb_arg;
  uint64_t free = 0;

  SPDK_NOTICELOG("init entry\n");
  if (rberrno) {
    unload_bs(hello_context, "Error initing the blobstore", rberrno);
    return;
  }

  hello_context->bs = bs;
  SPDK_NOTICELOG("blobstore: %p\n", hello_context->bs);
  hello_context->channel = spdk_bs_alloc_io_channel(hello_context->bs);

  free = spdk_bs_free_cluster_count(hello_context->bs);
  SPDK_NOTICELOG("blobstore has FREE clusters of %" PRIu64 "\n", free);
  
  make_rolling_blob(hello_context->bs, hello_context->channel, rolling_blob::huge_blob_size, 
                    make_log_done, hello_context);
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

  buffer_pool_init();
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
  buffer_pool_fini();
  hello_cleanup(hello_context);
  return rc;
}
