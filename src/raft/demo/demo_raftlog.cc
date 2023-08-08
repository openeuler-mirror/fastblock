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

#include "localstore/blob_manager.h"
#include "localstore/disk_log.h"
#include "raft/raft_log.h"
#include "raft/raft.h"

#define WRITE_ITERATIONS 100

static const char *g_bdev_name = NULL;
static struct disk_log* g_disklog;
static std::shared_ptr<raft_log> g_raftlog;
static std::string g_data;

class append_context : public context {
  virtual void finish(int r) {
    SPDK_NOTICELOG("append finish\n");
  }
};


inline std::string rand_str(int len) {
  std::string str(len, '0');
  static char t[63] = {"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"};
  for(int i = 0; i < len; i++) {
    str[i] = t[rand() % 62];
  }
  return str;
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



static void
bs_fini_done(void *arg, int rberrno) {
  SPDK_NOTICELOG("bs_fini_done\n");
  buffer_pool_fini();
  SPDK_NOTICELOG("spdk_app_stop\n");
  spdk_app_stop(0);
};

static void
demo_raftlog_fini(void *, int ){
  SPDK_NOTICELOG("demo_raftlog_fini\n");
  blobstore_fini(bs_fini_done, nullptr);
}

static void
demo_raftlog_stop(){
  SPDK_NOTICELOG("demo_raftlog_stop\n");
  g_disklog->stop(demo_raftlog_fini, nullptr);
  delete g_disklog;
}

struct disk_append_context : public context{
    disk_append_context(raft_index_t _start_idx, raft_index_t _end_idx, raft_log* rlog)
    : start_idx(_start_idx)
    , end_idx(_end_idx)
    , rlog(rlog) {}

    void finish(int r) override {
        rlog->raft_write_entry_finish(start_idx, end_idx, r);
        demo_raftlog_stop();
    }
    raft_index_t start_idx;
    raft_index_t end_idx;
    raft_log* rlog;
};


static 
std::pair<std::shared_ptr<raft_entry_t>, context*>
make_raftlog() {
  auto entry_ptr = std::make_shared<raft_entry_t>();
  entry_ptr->set_type(RAFT_LOGTYPE_WRITE);
  auto data = entry_ptr->mutable_data();
  entry_ptr->set_obj_name(rand_str(20));
  entry_ptr->set_meta(rand_str(20));
  entry_ptr->set_data(g_data);

  append_context* ctx = new append_context;

  return {entry_ptr, ctx};
}

static void 
make_log_done(void *arg, struct disk_log* dlog, int rberrno){
  SPDK_NOTICELOG("make_log_done\n");

  g_disklog = dlog;
  g_raftlog = log_new(dlog); 
  g_data = std::string(4096, ' ');

  std::vector<std::pair<std::shared_ptr<raft_entry_t>, context*>> entries;
  entries.emplace_back(make_raftlog());
  entries.emplace_back(make_raftlog());
  entries.emplace_back(make_raftlog());

  g_raftlog->log_append(entries);
  SPDK_NOTICELOG("log_append\n");

  disk_append_context* disk_ctx = new disk_append_context(0, 100, g_raftlog.get());
  g_raftlog->disk_append(0, 100, disk_ctx);
}

static void
bs_init_done(void *arg, int rberrno) {
  SPDK_NOTICELOG("blobstore:%p io_channel:%p\n", global_blobstore(), global_io_channel());

  make_disk_log(global_blobstore(), global_io_channel(), make_log_done, nullptr);
};


static void
demo_raftlog_start(void *arg)
{
  buffer_pool_init();
  blobstore_init(g_bdev_name, bs_init_done, nullptr);
};

int
main(int argc, char **argv)
{
  struct spdk_app_opts opts = {};
  int rc = 0;

  SPDK_NOTICELOG("entry\n");

  spdk_app_opts_init(&opts, sizeof(opts));
  opts.name = "hello_blob";
  if ((rc = spdk_app_parse_args(argc, argv, &opts, "b:", NULL,
          demo_parse_arg, demo_usage)) !=
      SPDK_APP_PARSE_ARGS_SUCCESS) {
    exit(rc);
  }

  srand(time(0));
  
  rc = spdk_app_start(&opts, demo_raftlog_start, nullptr);
  if (rc) {
    SPDK_NOTICELOG("ERROR!\n");
  } else {
    SPDK_NOTICELOG("SUCCESS!\n");
  }

  spdk_app_fini();
  return rc;
}