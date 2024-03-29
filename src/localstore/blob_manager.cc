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
#include "object_store.h"
#include "utils/units.h"
#include "utils/err_num.h"

#include <spdk/log.h>
#include <spdk/string.h>
#include <functional>
#include <string>

SPDK_LOG_REGISTER_COMPONENT(blob_log)

static std::string default_blobstore_type = "fastblock";
constexpr uint64_t blob_unit_size = 512;

struct blobstore_manager {
	struct spdk_blob_store* blobstore;
	struct spdk_io_channel* channel;

  sharded<blob_tree> blobs; // 初始化时，读取磁盘blob的xattr，找到blob的映射关系
};

static blobstore_manager g_bs_mgr;

/// TODO(sunyifang): 现在都是单核的
struct spdk_blob_store* global_blobstore() {
    return g_bs_mgr.blobstore;
}

struct spdk_io_channel* global_io_channel() {
    return g_bs_mgr.channel;
}

sharded<blob_tree>& global_blob_tree(){
    return g_bs_mgr.blobs;
}

static void
fb_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
       void *event_ctx)
{
  SPDK_WARNLOG("Unsupported bdev event now. type: %d\n", type);
}


struct bm_context {
  char *uuid_buf;
  uint32_t uuid_len;
  bm_complete cb_fn;
  void*       args;
  std::string *osd_uuid;
  struct spdk_blob *blob;
  std::string bdev_name;
};

struct create_blob_ctx {
  struct bm_context *ctx;
  blob_type type;
  struct spdk_blob_store *bs;
  spdk_blob_id blobid;
};

static void close_super_done(void *cb_arg, int bserrno){
  struct bm_context *ctx = (struct bm_context *)cb_arg;
  if (bserrno) {
    SPDK_ERRLOG("close super blob failed %s\n", spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    spdk_free(ctx->uuid_buf);
    delete ctx;
    return;
  }  

  ctx->cb_fn(ctx->args, 0);
  spdk_free(ctx->uuid_buf);
  delete ctx;
}

static void write_super_complete(void *arg, int rberrno){
  struct bm_context *ctx = (struct bm_context *)arg;
  if (rberrno) {
      SPDK_ERRLOG("write super blob failed:%s\n", spdk_strerror(rberrno));
      ctx->cb_fn(ctx->args, rberrno);
      spdk_free(ctx->uuid_buf);
      delete ctx;
      return;
  }    

  spdk_blob_close(ctx->blob, close_super_done, ctx);
}

void open_blob_complete(void *arg, struct spdk_blob *blob, int objerrno){
  struct create_blob_ctx *bc = (struct create_blob_ctx*)arg;
  struct bm_context *ctx = bc->ctx;
  if(objerrno){
    SPDK_ERRLOG("set super blob id of blob %lu failed: %s\n",
        bc->blobid, spdk_strerror(objerrno));
    bc->ctx->cb_fn(bc->ctx->args, objerrno);
    spdk_free(bc->ctx->uuid_buf);
    delete bc->ctx;
    delete bc;
    return;
  }

  ctx->blob = blob;
  SPDK_INFOLOG(blob_log, "open super blob: %lu done, write uuid %s to super blob\n",
          bc->blobid, bc->ctx->uuid_buf);
  //write uuid to super blob
  auto channel = global_io_channel();
  spdk_blob_io_write(blob, channel, bc->ctx->uuid_buf, 0,
    bc->ctx->uuid_len / blob_unit_size, write_super_complete, bc->ctx);
  delete bc;
}

static void
set_super_complete(void *cb_arg, int bserrno){
  struct create_blob_ctx *bc = (struct create_blob_ctx*)cb_arg;
  if(bserrno){
    SPDK_ERRLOG("set super blob id of blob %lu failed: %s\n",
        bc->blobid, spdk_strerror(bserrno));
    bc->ctx->cb_fn(bc->ctx->args, bserrno);
    spdk_free(bc->ctx->uuid_buf);
    delete bc->ctx;
    delete bc;
    return;
  }

  SPDK_INFOLOG(blob_log, "set super blob: %lu done\n", bc->blobid);
  spdk_bs_open_blob(bc->bs, bc->blobid, open_blob_complete, bc);
}

static void create_super_blob_done(void *arg, spdk_blob_id blobid, int objerrno) {
  struct create_blob_ctx* bc = (struct create_blob_ctx*)arg;
  struct bm_context *ctx = bc->ctx;

  if (objerrno) {
    SPDK_ERRLOG("create super blob failed:%s\n", spdk_strerror(objerrno));
    ctx->cb_fn(ctx->args, objerrno);
    spdk_free(bc->ctx->uuid_buf);
    delete ctx;
    delete bc;
    return;
  }

  SPDK_INFOLOG(blob_log, "create super blob: %lu done\n", blobid);
  bc->blobid = blobid;
  spdk_bs_set_super(bc->bs, blobid, set_super_complete, bc);  
}

static void
super_get_xattr_value(void *arg, const char *name, const void **value, size_t *value_len) {
  struct create_blob_ctx* ctx = (struct create_blob_ctx*)arg;


  if(!strcmp("type", name)){
    *value = &(ctx->type);
    *value_len = sizeof(ctx->type);    
    return; 
  }
  *value = NULL;
  *value_len = 0;
}

/**
 * spdk_bs_init：初始化blobstore
 */
static void
bs_init_complete(void *args, struct spdk_blob_store *bs, int bserrno)
{
  struct bm_context *ctx = (struct bm_context *)args;
  uint64_t free = 0;

  if (bserrno) {
    SPDK_ERRLOG("blobstore init failed: %s\n", spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    spdk_free(ctx->uuid_buf);
    delete ctx;
    return;
  }

  // SPDK_WARNLOG("blobstore init success.\n");
  std::construct_at(&g_bs_mgr);
  g_bs_mgr.blobstore = bs;
  g_bs_mgr.channel = spdk_bs_alloc_io_channel(bs);

  free = spdk_bs_free_cluster_count(bs);
  SPDK_INFOLOG(blob_log, "blobstore has FREE clusters of %lu\n", free);

  struct create_blob_ctx* bc = new create_blob_ctx();
  bc->type = blob_type::super_blob;
  bc->ctx = ctx;
  bc->bs = bs;

  struct spdk_blob_opts opts;
  spdk_blob_opts_init(&opts, sizeof(opts));
  opts.num_clusters = 4_MB / spdk_bs_get_cluster_size(bs);
  char *xattr_names[] = {"type"};
  opts.xattrs.count = SPDK_COUNTOF(xattr_names);
  opts.xattrs.names = xattr_names;
  opts.xattrs.ctx = bc;
  opts.xattrs.get_value = super_get_xattr_value; 
  spdk_bs_create_blob_ext(bs, &opts, create_super_blob_done, bc);
}

static void
bs_read_complete(void *args, struct spdk_blob_store *bs, int bserrno){
  struct bm_context *ctx = (struct bm_context *)args;

  SPDK_INFOLOG(blob_log, "blobstore read done\n");
  if (bserrno == 0) {
    SPDK_INFOLOG(blob_log, "There is data on the disk.\n"); 

    std::construct_at(&g_bs_mgr);
    g_bs_mgr.blobstore = bs;
    g_bs_mgr.channel = spdk_bs_alloc_io_channel(bs);
    ctx->cb_fn(ctx->args, err::RAFT_ERR_DISK_NOT_EMPTY);
    spdk_free(ctx->uuid_buf);
    delete ctx;
    return;
  }

  //spdk_bs_load失败后，bs会释放，bs->dev也会释放，需要重新生成bs_dev
  struct spdk_bs_opts opts = {};
  spdk_bs_opts_init(&opts, sizeof(opts));
  memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
  default_blobstore_type.copy(opts.bstype.bstype, SPDK_BLOBSTORE_TYPE_LENGTH - 1);

  struct spdk_bs_dev *bs_dev = NULL;
  int rc = spdk_bdev_create_bs_dev_ext(ctx->bdev_name.c_str(), fb_event_cb, NULL, &bs_dev);
  if (rc != 0) {
    SPDK_ERRLOG("create bs_dev failed: %s\n", spdk_strerror(-rc));
    ctx->cb_fn(ctx->args, rc);
    spdk_free(ctx->uuid_buf);
    delete ctx;
    return;
  }
  
  SPDK_INFOLOG(blob_log, "blobstore init...\n");
  spdk_bs_init(bs_dev, &opts, bs_init_complete, ctx);
}

struct blobstore_context{
  bm_complete cb_fn;
  void *arg;
  spdk_thread *thread;
  int serror;
};

static void 
_blobstore_init(bm_complete cb_fn, void* args, std::string &bdev_name, std::string &uuid){
  struct spdk_bs_dev *bs_dev = NULL;
  int rc = spdk_bdev_create_bs_dev_ext(bdev_name.c_str(), fb_event_cb, NULL, &bs_dev);
  if (rc != 0) {
    SPDK_ERRLOG("create bs_dev failed: %s\n", spdk_strerror(-rc));
    cb_fn(args, rc);
    return;
  }

  auto ctx = new bm_context{.cb_fn = std::move(cb_fn), .args = args, .osd_uuid = nullptr, .bdev_name = bdev_name};
  ctx->uuid_len = 4096;
  ctx->uuid_buf = (char*)spdk_zmalloc(ctx->uuid_len, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
  uuid.copy(ctx->uuid_buf, ctx->uuid_len - 1);
  spdk_bs_load(bs_dev, NULL, bs_read_complete, ctx);
}

static inline void _blobstore_op_done(void *arg){
  blobstore_context *ctx = (blobstore_context *)arg;
  ctx->cb_fn(ctx->arg, ctx->serror);
  delete ctx;
}

static void blobstore_op_done(blobstore_context *ctx){
  auto cur_thread = spdk_get_thread();
  if(cur_thread != ctx->thread){
    spdk_thread_send_msg(ctx->thread, _blobstore_op_done, ctx);
  }else{
    _blobstore_op_done(ctx);
  }
}

static void
_blobstore_init(std::string bdev_name, std::string uuid, bm_complete cb_fn, void* args, spdk_thread *thread) {
  SPDK_INFOLOG(blob_log, "blobstore init, thread id %lu\n", spdk_thread_get_id(spdk_get_thread()));
  
  blobstore_context *ctx = new blobstore_context{.cb_fn = std::move(cb_fn), .arg = args, .thread = thread};
  _blobstore_init([](void *arg, int serror){
    blobstore_context *ctx = (blobstore_context *)arg;
    ctx->serror = serror;
    SPDK_INFOLOG(blob_log, "blobstore init done.\n");
    blobstore_op_done(ctx);
  },
  ctx, bdev_name, uuid);
}

void
blobstore_init(std::string &bdev_name, const std::string& uuid, bm_complete cb_fn, void* args){
  auto &shard = core_sharded::get_core_sharded();
  /*
    在spdk的函数bs_open_blob中有一个行“assert(spdk_get_thread() == bs->md_thread)”会检测当前spdk线程与blobstore的spdk线程是否相等，
    raft在读写log核对象数据之前会先使用bs_open_blob打开blob，为了保证在debug模式下正常运行，应该让raft的spdk线程与blobstore的spdk线程
    相同。
  */
  //这里先支持单核    
  auto cur_thread = spdk_get_thread();
  shard.invoke_on(
    0,
    [bdev_name = bdev_name, uuid, cb_fn, args, cur_thread](){
      _blobstore_init(bdev_name, std::move(uuid), cb_fn, args, cur_thread);
    }
  );  
}

/**
 * spdk_bs_init：加载已经存在的blobstore
 */
void parse_kv_xattr(struct spdk_blob *blob) {
  uint32_t *shard_id;
  size_t len;
  int rc;

  rc = spdk_blob_get_xattr_value(blob, "shard", (const void **)&shard_id, &len);
  if (rc < 0) return;

  if (*shard_id > g_bs_mgr.blobs.size())  return;
  SPDK_INFOLOG(blob_log, "kv xattr, blob id %lu has shard_id: %u\n", spdk_blob_get_id(blob), (*shard_id));
  g_bs_mgr.blobs.on_shard(*shard_id).kv_blob = spdk_blob_get_id(blob);
}

void parse_log_xattr(struct spdk_blob *blob) {
  uint32_t *shard_id;
  const char *value;
  std::string pg;
  size_t len;
  int rc;

  rc = spdk_blob_get_xattr_value(blob, "shard", (const void **)&shard_id, &len);
  if (rc < 0) 
    return;
  // SPDK_NOTICELOG("blob id %lu has shard_id: %u\n", spdk_blob_get_id(blob), (*shard_id));
  

  rc = spdk_blob_get_xattr_value(blob, "pg", (const void **)&value, &len);
  if (rc < 0) 
    return;
  pg = std::string(value, len);
  SPDK_INFOLOG(blob_log, "log xattr, blob id %lu shard_id %u  pg  %s \n", spdk_blob_get_id(blob), *shard_id, pg.c_str());

  if (*shard_id > g_bs_mgr.blobs.size())  return;
  SPDK_INFOLOG(blob_log, "log blob, blob id %ld blob size %lu\n", spdk_blob_get_id(blob), 
          spdk_blob_get_num_clusters(blob) * spdk_bs_get_cluster_size(global_blobstore()));
  g_bs_mgr.blobs.on_shard(*shard_id).log_blobs[pg] = blob;
}

void parse_object_xattr(struct spdk_blob *blob) {
  uint32_t *shard_id;
  const char *value;
  std::string pg, obj_name, snap_name, recover_name;
  size_t len;
  int rc;

  rc = spdk_blob_get_xattr_value(blob, "shard", (const void **)&shard_id, &len);
  if (rc < 0) return;

  rc = spdk_blob_get_xattr_value(blob, "pg", (const void **)&value, &len);
  if (rc < 0) return;
  pg = std::string(value, len);
  
  if (*shard_id > g_bs_mgr.blobs.size())  return;
  auto [pg_it, _] = g_bs_mgr.blobs.on_shard(*shard_id).object_blobs.try_emplace(pg);

  rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&value, &len);
  if (rc < 0) return;
  obj_name = std::string(value, len);
  auto [it, __] = pg_it->second.try_emplace(obj_name);
  auto& object = it->second;

  if(spdk_blob_get_xattr_value(blob, "recover", (const void **)&value, &len) == 0){
      recover_name = std::string(value, len);
      object.recover.blob = blob;
      object.recover.blobid = spdk_blob_get_id(blob);
  }else if(spdk_blob_get_xattr_value(blob, "snap", (const void **)&value, &len) == 0){
      snap_name = std::string(value, len);
      object_store::snap snapshot;
      snapshot.snap_blob.blobid = spdk_blob_get_id(blob); 
      snapshot.snap_blob.blob = blob;
      snapshot.snap_name = snap_name;
      object.snap_list.emplace_back(std::move(snapshot));   
  }else{
      object.origin.blob = blob;
      object.origin.blobid = spdk_blob_get_id(blob);    
  }

  SPDK_INFOLOG(blob_log, "blob id %lu shard_id %u pg %s obj_name %s  snap_name %s recover_name %s\n", 
          spdk_blob_get_id(blob), *shard_id, pg.c_str(), obj_name.c_str(), snap_name.c_str(), 
          recover_name.c_str());
}

void parse_kv_checkpoint_xattr(struct spdk_blob *blob) {
  uint32_t *shard_id;
  size_t len;
  int rc;

  rc = spdk_blob_get_xattr_value(blob, "shard", (const void **)&shard_id, &len);
  if (rc < 0) return;

  if (*shard_id > g_bs_mgr.blobs.size())  return;
  SPDK_INFOLOG(blob_log, "kv_checkpoint xattr, blob id %lu has shard_id: %u\n", spdk_blob_get_id(blob), (*shard_id));
  g_bs_mgr.blobs.on_shard(*shard_id).kv_checkpoint_blob = spdk_blob_get_id(blob);
}

void parse_kv_new_checkpoint_xattr(struct spdk_blob *blob) {
  uint32_t *shard_id;
  size_t len;
  int rc;

  rc = spdk_blob_get_xattr_value(blob, "shard", (const void **)&shard_id, &len);
  if (rc < 0) return;

  if (*shard_id > g_bs_mgr.blobs.size())  return;
  SPDK_INFOLOG(blob_log, "kv_new_checkpoint xattr, blob id %lu has shard_id: %u\n", spdk_blob_get_id(blob), (*shard_id));
  g_bs_mgr.blobs.on_shard(*shard_id).kv_new_checkpoint_blob = spdk_blob_get_id(blob);
}

struct parse_blob_ctx{
  bm_complete cb_fn;
  void* args;
  blob_type type;
};

static void parse_blob_xattr(void *arg, struct spdk_blob *blob, int rberrno){
  parse_blob_ctx* ctx = (parse_blob_ctx*)arg;
  if(rberrno){
    SPDK_ERRLOG("open blob failed: %s\n", spdk_strerror(rberrno));
    ctx->cb_fn(ctx->args, rberrno);
    delete ctx;
    return;
  }

  auto type_str = type_string(ctx->type);
  SPDK_INFOLOG(blob_log, "blob id %lu type: %s\n", spdk_blob_get_id(blob), type_str.c_str());
  switch (ctx->type) {
    case blob_type::kv:
      parse_kv_xattr(blob);
      break;
    case blob_type::log:
      parse_log_xattr(blob);
      break;
    case blob_type::object:
      parse_object_xattr(blob);
      break;
    case blob_type::kv_checkpoint:
      parse_kv_checkpoint_xattr(blob);
      break;
    case blob_type::kv_checkpoint_new:
      parse_kv_new_checkpoint_xattr(blob);
      break;
    default:
      break;
  }  
  ctx->cb_fn(ctx->args, 0);
  delete ctx;  
}

void parse_open_blob_xattr(struct spdk_blob *blob, bm_complete cb_fn, void* args){
  //参数blob随后会在外部关闭
  blob_type *type;
  size_t len;
  int rc;

  rc = spdk_blob_get_xattr_value(blob, "type", (const void**)&type, &len);
  if (rc < 0){
    cb_fn(args, 0);
    return;
  }

  auto type_str = type_string(*type);
  SPDK_INFOLOG(blob_log, "blob id %lu type: %s\n", spdk_blob_get_id(blob), type_str.c_str());
  parse_blob_ctx *ctx = new parse_blob_ctx{.cb_fn = std::move(cb_fn), .args = args, .type = *type};
  switch (*type){
  case blob_type::log:
  case blob_type::object:
    spdk_bs_open_blob(g_bs_mgr.blobstore, spdk_blob_get_id(blob),
		       parse_blob_xattr, ctx);
    break;
  case blob_type::kv:
  case blob_type::kv_checkpoint:
  case blob_type::kv_checkpoint_new:
    parse_blob_xattr(ctx, blob, 0);
    break;
  default:
    ctx->cb_fn(ctx->args, 0);
    delete ctx;
    break;
  }
}

static void
blob_iter_complete(void *args, struct spdk_blob *blob, int bserrno)
{ 
  auto err_hand = [](void *arg, int berrno){
    if (berrno) {
      struct bm_context *ctx = (struct bm_context *)arg;
      if (berrno == -ENOENT) {
        // ENOENT 表示迭代顺利完成.
        SPDK_INFOLOG(blob_log, "blob iteration complete.\n");
        ctx->cb_fn(ctx->args, 0);
        delete ctx;
        return;
      }
      SPDK_ERRLOG("blob iteration failed: %s\n", spdk_strerror(berrno));
      ctx->cb_fn(ctx->args, berrno);
      delete ctx;
      return;
    }
  };

  if (bserrno) {
    return err_hand(args, bserrno);
  }

  parse_open_blob_xattr(
    blob,
    [blob, err_hand = std::move(err_hand)](void *args, int berrno){
      SPDK_INFOLOG(blob_log, "parse_open_blob_xattr, berrno %d\n", berrno);
      if(berrno){
        return err_hand(args, berrno);
      }
      spdk_bs_iter_next(g_bs_mgr.blobstore, blob, blob_iter_complete, args);
    },
    args);
}

static void close_super_complete(void *cb_arg, int bserrno){
  struct bm_context *ctx = (struct bm_context *)cb_arg;
  if (bserrno) {
    SPDK_ERRLOG("close super blob failed %s\n", spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }  

  SPDK_INFOLOG(blob_log, "close super blob done\n");
  // 读取每个blob的xattr，放入map中，准备还原blob运行状态
  spdk_bs_iter_first(g_bs_mgr.blobstore, blob_iter_complete, ctx);
}

static void read_super_blob_complete(void *arg, int bserrno) {
  struct bm_context *ctx = (struct bm_context *)arg;

  if (bserrno) {
    SPDK_ERRLOG("open super blob failed %s\n", spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    if(ctx->uuid_buf)
        spdk_free(ctx->uuid_buf);
    delete ctx;
    return;
  }

  SPDK_INFOLOG(blob_log, "read super blob done, uuid %s\n", ctx->uuid_buf);
  if(ctx->osd_uuid)
      *(ctx->osd_uuid) = ctx->uuid_buf;
  spdk_free(ctx->uuid_buf);

  spdk_blob_close(ctx->blob, close_super_complete, ctx);
}

static void open_super_blob_complete(void *arg, struct spdk_blob *blob, int objerrno){
  struct bm_context *ctx = (struct bm_context *)arg;
  if (objerrno) {
    SPDK_ERRLOG("open super blob failed %s\n", spdk_strerror(objerrno));
    ctx->cb_fn(ctx->args, objerrno);
    delete ctx;
    return;
  }

  ctx->blob = blob;
  auto channel = global_io_channel();
  ctx->uuid_len = 4096;
  ctx->uuid_buf = (char*)spdk_zmalloc(ctx->uuid_len, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
  spdk_blob_io_read(blob, channel, ctx->uuid_buf, 0,
		      ctx->uuid_len / blob_unit_size,
		      read_super_blob_complete, ctx);
}

static void get_super_complete(void *cb_arg, spdk_blob_id blobid, int bserrno){
  struct bm_context *ctx = (struct bm_context *)cb_arg;
  if (bserrno) {
    SPDK_ERRLOG("get super failed %s\n", spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  SPDK_INFOLOG(blob_log, "get super blob: %lu done\n", blobid);
  spdk_bs_open_blob(g_bs_mgr.blobstore, blobid, open_super_blob_complete, ctx);
}

static void
bs_load_complete(void *args, struct spdk_blob_store *bs, int bserrno)
{ 
  struct bm_context *ctx = (struct bm_context *)args;
  uint64_t free = 0;

  if (bserrno) {
    SPDK_ERRLOG("blobstore load failed, do you have blobstore on this disk? %s\n", err::string_status(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  SPDK_INFOLOG(blob_log, "blobstore load success.\n");
  std::construct_at(&g_bs_mgr);
  g_bs_mgr.blobstore = bs;
  g_bs_mgr.channel = spdk_bs_alloc_io_channel(bs);
  g_bs_mgr.blobs.start();

  free = spdk_bs_free_cluster_count(bs);
  SPDK_INFOLOG(blob_log, "blobstore has FREE clusters of %lu\n", free);

  spdk_bs_get_super(g_bs_mgr.blobstore, get_super_complete, ctx);
}

static void
_blobstore_load(std::string &bdev_name, bm_complete cb_fn, void* args, std::string *osd_uuid) {
  struct spdk_bs_dev *bs_dev = NULL;

  int rc = spdk_bdev_create_bs_dev_ext(bdev_name.c_str(), fb_event_cb, NULL, &bs_dev);
  if (rc != 0) {
    SPDK_ERRLOG("create bs_dev failed: %s\n", spdk_strerror(-rc));
    cb_fn(args, rc);
    return;
  }

  auto ctx = new bm_context{.cb_fn = cb_fn, .args = args, .osd_uuid = osd_uuid};
  spdk_bs_load(bs_dev, NULL, bs_load_complete, ctx);
}

static void
_blobstore_load(std::string bdev_name, bm_complete cb_fn, void* args, std::string *osd_uuid, spdk_thread *thread){
  SPDK_INFOLOG(blob_log, "blobstore load, thread id %lu\n", spdk_thread_get_id(spdk_get_thread()));

  blobstore_context *ctx = new blobstore_context{.cb_fn = std::move(cb_fn), .arg = args, .thread = thread};
  _blobstore_load(
    bdev_name,
    [](void *arg, int serror){
      blobstore_context *ctx = (blobstore_context *)arg;
      ctx->serror = serror;
      SPDK_INFOLOG(blob_log, "blobstore load done.\n");
      blobstore_op_done(ctx);
    },
    ctx,
    osd_uuid
  );
}

void
blobstore_load(std::string &bdev_name, bm_complete cb_fn, void* args, std::string *osd_uuid){
  auto &shard = core_sharded::get_core_sharded();
  
  auto cur_thread = spdk_get_thread();
  shard.invoke_on(
    0,
    [bdev_name = bdev_name, osd_uuid, cb_fn, args, cur_thread](){
      _blobstore_load(bdev_name, cb_fn, args, osd_uuid, cur_thread);
    }
  );    
}

/**
 * spdk_bs_unload 卸载blobstore
 */
static void
bs_fini_complete(void *args, int bserrno)
{ 
  struct bm_context *ctx = (struct bm_context *)args;

  if (bserrno) {
    SPDK_ERRLOG("blobstore unload failed: %s\n", spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  SPDK_INFOLOG(blob_log, "blobstore unload success.\n");
  g_bs_mgr.blobstore = nullptr;
  g_bs_mgr.channel = nullptr;
  g_bs_mgr.blobs.stop();

  ctx->cb_fn(ctx->args, 0);
  delete ctx;
}

static void
_blobstore_fini(bm_complete cb_fn, void* args)
{
	if (g_bs_mgr.blobstore) {
		if (g_bs_mgr.channel) {
      SPDK_INFOLOG(blob_log, "free io_channel\n");
			spdk_bs_free_io_channel(g_bs_mgr.channel);
		}
    auto ctx = new bm_context{.cb_fn = cb_fn, .args = args};
    spdk_bs_unload(g_bs_mgr.blobstore, bs_fini_complete, ctx);
	}else{
    cb_fn(args, 0);
  }
}

static void
_blobstore_fini(bm_complete cb_fn, void* args, spdk_thread *thread){
  SPDK_INFOLOG(blob_log, "blobstore fini, thread id %lu\n", spdk_thread_get_id(spdk_get_thread()));
  auto ctx = new blobstore_context{.cb_fn = std::move(cb_fn), .arg = args, .thread = thread};
  _blobstore_fini([](void *arg, int serror){
    blobstore_context *ctx = (blobstore_context *)arg;
    ctx->serror = serror;
    SPDK_INFOLOG(blob_log, "blobstore fini done.\n");
    blobstore_op_done(ctx);
  },
  ctx);
}

void
blobstore_fini(bm_complete cb_fn, void* args){
  auto &shard = core_sharded::get_core_sharded();

  auto cur_thread = spdk_get_thread();
  shard.invoke_on(
    0,
    [cb_fn, args, cur_thread](){
      _blobstore_fini(cb_fn, args, cur_thread);
    }
  );    
}