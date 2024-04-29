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
#include "localstore/kv_store.h"
#include "utils/itos.h"
#include "utils/utils.h"


static const char *g_bdev_name = NULL;

struct hello_context_t {
  struct spdk_blob_store *bs;
  struct spdk_io_channel *channel;
  std::string bdev_name;
  fb_blob blob;
  spdk_buffer buf;
  int rc;
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

/*
 * 卸载完成
 */
static void
unload_complete(void *cb_arg, int objerrno)
{
  struct hello_context_t *hello_context = (struct hello_context_t *)cb_arg;

  SPDK_NOTICELOG("unload_complete, objerrno:%d \n", objerrno);
  if (objerrno) {
    SPDK_ERRLOG("Error %d unloading the blobstore\n", objerrno);
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

  blobstore_fini(unload_complete, hello_context);
}

void blobpool_test_close_done(void *arg, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
    SPDK_ERRLOG("Error in spdk_blob_close\n");
    unload_bs(hello_context, "Error in spdk_blob_close", rberrno);
    return;
  }

  unload_bs(hello_context, "", 0);
}

void blobpool_test_write_done(void *arg, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
    SPDK_ERRLOG("Error in spdk_blob_io_write\n");
    unload_bs(hello_context, "Error in spdk_blob_io_write", rberrno);
    return;
  }

  // blobpool_test_finish(hello_context, 0);
  SPDK_NOTICELOG("blob_pool close running on thread: %lu\n", utils::get_spdk_thread_id());
  spdk_blob_close(hello_context->blob.blob, blobpool_test_close_done, hello_context);
}

void blobpool_test_open_done(void *arg, struct spdk_blob *blob, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
    SPDK_ERRLOG("Error in open blob\n");
    unload_bs(hello_context, "Error in open blob", rberrno);
    return;
  }

  hello_context->blob.blob = blob;

  SPDK_NOTICELOG("blob_pool write running on thread: %lu\n", utils::get_spdk_thread_id());
  spdk_blob_io_write(blob, global_io_channel(), hello_context->buf.get_buf(), 0, 8, blobpool_test_write_done, hello_context);
}

void blobpool_test(void *arg, int rberrno) {
    SPDK_NOTICELOG("blob_pool_test running on thread: %lu\n", utils::get_spdk_thread_id());

    if (global_blob_pool().has_free_blob()) {
        SPDK_NOTICELOG("has free blob\n");
    } else {
        SPDK_NOTICELOG("no free blob\n");
    }

    SPDK_NOTICELOG("blob_pool size: %lu\n", global_blob_pool().size());
    auto blob = global_blob_pool().get();
    SPDK_NOTICELOG("blob_pool size: %lu\n", global_blob_pool().size());

    SPDK_NOTICELOG("blob_pool open running on thread: %lu\n", utils::get_spdk_thread_id());
    spdk_bs_open_blob(global_blobstore(), blob.blobid, blobpool_test_open_done, arg);
}

static void
init_complete(void *arg, int rberrno)
{
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  SPDK_NOTICELOG("init_complete\n");
  
  auto &shard = core_sharded::get_core_sharded();
  shard.invoke_on(
    0,
    [hello_context](){
      blobpool_test(hello_context, 0);
    }
  );   
}

static void
base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
       void *event_ctx)
{
  SPDK_WARNLOG("Unsupported bdev event: type %d\n", type);
}

static void
hello_start(void *arg1)
{
  struct hello_context_t *hello_context = (struct hello_context_t *)arg1;
  struct spdk_bs_dev *bs_dev = NULL;
  int rc;

  
  buffer_pool_init();
  hello_context->buf = buffer_pool_get();
  blobstore_init(hello_context->bdev_name, "0", init_complete, hello_context);
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
  opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;
  ::spdk_log_set_flag("blob_log");

  if ((rc = spdk_app_parse_args(argc, argv, &opts, "b:", NULL,
          demo_parse_arg, demo_usage)) !=
      SPDK_APP_PARSE_ARGS_SUCCESS) {
    exit(rc);
  }

  hello_context = new hello_context_t();
  hello_context->bdev_name = std::string(g_bdev_name);
  if (hello_context != NULL) {
    srand(time(0));

    rc = spdk_app_start(&opts, hello_start, hello_context);
    if (rc) {
      SPDK_NOTICELOG("ERROR!\n");
    } else {
      SPDK_NOTICELOG("SUCCESS!\n");
    }    
  } else {
    SPDK_ERRLOG("Could not alloc hello_context struct!!\n");
    rc = -ENOMEM;
  }

  buffer_pool_put(hello_context->buf);
  buffer_pool_fini();
  spdk_app_fini();
  delete hello_context;
  return rc;
}
