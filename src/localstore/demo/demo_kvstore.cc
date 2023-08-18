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

#define append_ITERATIONS 100
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
  kvstore* kvs;
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

uint64_t create_blob_tsc, create_snap_tsc;

uint64_t write_blob_tsc, write_snap_tsc;

/*
 * Free up memory that we allocated.
 */
static void
hello_cleanup(struct hello_context_t *hello_context)
{
    delete hello_context->kvs;
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

  unload_bs(hello_context, "", 0);
}

static void kvstore_replay_done(void *arg, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
    unload_bs(hello_context, "Error in rolling append continue completion", rberrno);
    return;
  }

  SPDK_NOTICELOG("kvstore_replay_done. kvstore size: %lu \n", hello_context->kvs->size());

  for (int i = 0; i < 150000; i += 5000) {
      auto key = std::string("key") + itos(i);
      auto val = hello_context->kvs->get(key);
      if (val) {
          SPDK_NOTICELOG("key:%s get value:%s\n", key.c_str(), val->c_str());
      } else {
          SPDK_NOTICELOG("key:%s no value\n", key.c_str());
      }
  }

  hello_context->kvs->stop(close_complete, hello_context);
}

static void
kvstore_replay(void *arg, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
    unload_bs(hello_context, "Error in rolling append continue completion", rberrno);
    return;
  }

  SPDK_NOTICELOG("kvstore_persist_second done. kvstore size: %lu \n", hello_context->kvs->size());

  for (int i = 0; i < 150000; i += 5000) {
      auto key = std::string("key") + itos(i);
      auto val = hello_context->kvs->get(key);
      if (val) {
          SPDK_NOTICELOG("key:%s get value:%s\n", key.c_str(), val->c_str());
      } else {
          SPDK_NOTICELOG("key:%s no value\n", key.c_str());
      }
  }

  hello_context->kvs->clear();
  SPDK_NOTICELOG("kvstore clear. kvstore size: %lu \n", hello_context->kvs->size());

  hello_context->kvs->replay(kvstore_replay_done, hello_context);
}

static void
kvstore_persist_third(void *arg, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
    unload_bs(hello_context, "Error in rolling append continue completion", rberrno);
    return;
  }

  SPDK_NOTICELOG("kvstore_persist_second done. kvstore size: %lu \n", hello_context->kvs->size());

  for (int i = 0; i < 150000; i += 5000) {
      auto key = std::string("key") + itos(i);
      auto val = hello_context->kvs->get(key);
      if (val) {
          SPDK_NOTICELOG("key:%s get value:%s\n", key.c_str(), val->c_str());
      } else {
          SPDK_NOTICELOG("key:%s no value\n", key.c_str());
      }
  }

  for (int i = 100000; i < 150000; i++) {
      hello_context->kvs->put(std::string("key") + itos(i), rand_str(4) + itos(i));
  }
  hello_context->kvs->persist(kvstore_replay, hello_context);
}

static void
kvstore_persist_second(void *arg, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
    unload_bs(hello_context, "Error in rolling append continue completion", rberrno);
    return;
  }

  SPDK_NOTICELOG("kvstore_persist_first done. kvstore size: %lu \n", hello_context->kvs->size());

  for (int i = 0; i < 150000; i += 5000) {
      auto key = std::string("key") + itos(i);
      auto val = hello_context->kvs->get(key);
      if (val) {
          SPDK_NOTICELOG("key:%s get value:%s\n", key.c_str(), val->c_str());
      } else {
          SPDK_NOTICELOG("key:%s no value\n", key.c_str());
      }
  }

  for (int i = 0; i < 50000; i++) {
      hello_context->kvs->remove(std::string("key") + itos(i));
  }
  hello_context->kvs->persist(kvstore_persist_third, hello_context);
}

static void
kvstore_persist_first(hello_context_t *hello_context) {
  SPDK_NOTICELOG("kvstore size: %lu \n", hello_context->kvs->size());

  for (int i = 0; i < 100000; i++) {
      hello_context->kvs->put(std::string("key") + itos(i), rand_str(6) + itos(i));
  }

  hello_context->kvs->persist(kvstore_persist_second, hello_context);
}

static void
make_rblob_done(void *arg, struct rolling_blob* rblob, int rberrno) {
  struct hello_context_t *hello_context = (struct hello_context_t *)arg;

  if (rberrno) {
      unload_bs(hello_context, "Error in blob create callback", rberrno);
      return;
  }

  SPDK_NOTICELOG("make_rblob_done\n");
  hello_context->kvs = new kvstore(rblob);

  kvstore_persist_first(hello_context);
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
                    make_rblob_done, hello_context);
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

  
  buffer_pool_init();
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
  buffer_pool_fini();
  spdk_app_fini();
  hello_cleanup(hello_context);
  return rc;
}
