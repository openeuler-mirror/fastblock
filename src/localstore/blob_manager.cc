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
#include "fastblock/utils/err_num.h"
#include "fastblock/utils/utils.h"

#include <spdk/log.h>
#include <spdk/string.h>
#include <functional>
#include <string>
#include <assert.h>

SPDK_LOG_REGISTER_COMPONENT(blob_log)

static std::string default_blobstore_type = "fastblock";
constexpr uint64_t blob_unit_size = 512;

struct blobstore_manager {
	struct spdk_blob_store* blobstore;
  struct spdk_io_channel* channel;
  blob_pool               pool;

  blob_tree blobs; // 初始化时，读取磁盘blob的xattr，找到blob的映射关系
};

class blobstore_managers {
public:
  blobstore_managers(uint32_t size){
    _bs_mgrs.resize(size);
  }

  uint32_t size(){
    return _bs_mgrs.size();
  }

  void set_blobstore(uint32_t index, struct spdk_blob_store* blobstore){
    _bs_mgrs[index].blobstore = blobstore;
  }

  void create_channel(uint32_t index, struct spdk_blob_store *blobstore){
    struct spdk_io_channel* channel = spdk_bs_alloc_io_channel(blobstore);
    _bs_mgrs[index].channel = channel;
  }

  void free_channel(uint32_t index){
    auto channel = _bs_mgrs[index].channel;
    spdk_bs_free_io_channel(channel);
    _bs_mgrs[index].channel = nullptr;
  }

  void start_blob_pool(uint32_t index, pool_create_complete cb_fn, void* arg){
    auto &pool = _bs_mgrs[index].pool;
    auto bs = _bs_mgrs[index].blobstore;
    if(!bs){
      cb_fn(arg, err::OSD_BLOBSTORE_NO_INIT);
      return;
    }
    pool.set_blobstore(bs);
    pool.start(cb_fn, arg);
  }

  void stop_blob_pool(uint32_t index, pool_create_complete cb_fn, void* arg){
    auto &pool = _bs_mgrs[index].pool;
    pool.stop(cb_fn, arg);
  }

  struct spdk_blob_store* get_blobstore(uint32_t index){
    return _bs_mgrs[index].blobstore;
  }

  struct spdk_io_channel* get_channel(uint32_t index){
    return _bs_mgrs[index].channel;
  }

  blob_pool& get_blob_pool(uint32_t index){
    return _bs_mgrs[index].pool;
  }

  blob_tree& get_blob_tree(uint32_t index){
    return _bs_mgrs[index].blobs;
  }

private:
  std::vector<struct blobstore_manager> _bs_mgrs;
};

static std::shared_ptr<blobstore_managers> g_bs_mgr;

//记录blobstore在bdev中的范围，blobstore_load时需要用到
std::vector<std::pair<uint64_t, uint64_t>> g_bs_space;

/// TODO(sunyifang): 现在都是单核的
struct spdk_blob_store* global_blobstore(uint32_t shard_id) {
    assert(shard_id < g_bs_mgr->size());
    return g_bs_mgr->get_blobstore(shard_id);
}

struct spdk_io_channel* global_io_channel(uint32_t shard_id) {
    assert(shard_id < g_bs_mgr->size());
    return g_bs_mgr->get_channel(shard_id);
}

blob_tree& global_blob_tree(uint32_t shard_id){
    assert(shard_id < g_bs_mgr->size());
    return g_bs_mgr->get_blob_tree(shard_id);
}

blob_pool& global_blob_pool(uint32_t shard_id) {
    assert(shard_id < g_bs_mgr->size());
    return g_bs_mgr->get_blob_pool(shard_id);
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
  bool force;
  uint64_t bs_start_offset;
  uint64_t bs_length;
  uint32_t shard_id;
};

struct create_blob_ctx {
  struct bm_context *ctx;
  blob_type type;
  struct spdk_blob_store *bs;
  spdk_blob_id blobid;
};

/*************** Star of blobstore_init ****************/

static void blob_pool_complete(void *cb_arg, int bserrno) {
  struct bm_context *ctx = (struct bm_context *)cb_arg;
  if (bserrno) {
    SPDK_ERRLOG("close super blob in shard %u failed %s\n", ctx->shard_id, spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    spdk_free(ctx->uuid_buf);
    delete ctx;
    return;
  }

  ctx->cb_fn(ctx->args, err::E_SUCCESS);
  spdk_free(ctx->uuid_buf);
  delete ctx;
}

static void close_super_done(void *cb_arg, int bserrno){
  struct bm_context *ctx = (struct bm_context *)cb_arg;
  if (bserrno) {
    SPDK_ERRLOG("close super blob in shard %u failed %s\n", ctx->shard_id, spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    spdk_free(ctx->uuid_buf);
    delete ctx;
    return;
  }

  SPDK_NOTICELOG("close super blob done, in shard: %u\n", ctx->shard_id);

  g_bs_mgr->start_blob_pool(ctx->shard_id, blob_pool_complete, ctx);
}

static void write_super_complete(void *arg, int rberrno){
  struct bm_context *ctx = (struct bm_context *)arg;
  if (rberrno) {
      SPDK_ERRLOG("write super blob in shard %u failed:%s\n", ctx->shard_id, spdk_strerror(rberrno));
      ctx->cb_fn(ctx->args, rberrno);
      spdk_free(ctx->uuid_buf);
      delete ctx;
      return;
  }

  SPDK_INFOLOG(blob_log, "write uuid to super blob done, in shard: %u\n", ctx->shard_id);
  spdk_blob_close(ctx->blob, close_super_done, ctx);
}

void open_blob_complete(void *arg, struct spdk_blob *blob, int objerrno){
  struct create_blob_ctx *bc = (struct create_blob_ctx*)arg;
  struct bm_context *ctx = bc->ctx;
  if(objerrno){
    SPDK_ERRLOG("open super blob id of blob %lu in shard: %u failed: %s\n",
        bc->blobid, ctx->shard_id, spdk_strerror(objerrno));
    bc->ctx->cb_fn(bc->ctx->args, objerrno);
    spdk_free(bc->ctx->uuid_buf);
    delete bc->ctx;
    delete bc;
    return;
  }

  ctx->blob = blob;
  SPDK_INFOLOG(blob_log, "open super blob: %lu done, write uuid %s to super blob in shard: %u\n",
                     bc->blobid, bc->ctx->uuid_buf, ctx->shard_id);
  // write uuid to super blob
  auto channel = global_io_channel(ctx->shard_id);
  spdk_blob_io_write(blob, channel, bc->ctx->uuid_buf, 0,
                     bc->ctx->uuid_len / blob_unit_size, write_super_complete, bc->ctx);
  delete bc;
}

static void
set_super_complete(void *cb_arg, int bserrno)
{
  struct create_blob_ctx *bc = (struct create_blob_ctx *)cb_arg;
  if (bserrno)
  {
    SPDK_ERRLOG("set super blob id of blob %lu in shard: %u failed: %s\n",
                bc->blobid, bc->ctx->shard_id, spdk_strerror(bserrno));
    bc->ctx->cb_fn(bc->ctx->args, bserrno);
    spdk_free(bc->ctx->uuid_buf);
    delete bc->ctx;
    delete bc;
    return;
  }

  SPDK_INFOLOG(blob_log, "set super blob: %lu in shard: %u done\n", bc->blobid, bc->ctx->shard_id);
  spdk_bs_open_blob(bc->bs, bc->blobid, open_blob_complete, bc);
}

static void create_super_blob_done(void *arg, spdk_blob_id blobid, int objerrno)
{
  struct create_blob_ctx *bc = (struct create_blob_ctx *)arg;
  struct bm_context *ctx = bc->ctx;

  if (objerrno)
  {
    SPDK_ERRLOG("create super blob in shard %u failed:%s\n", ctx->shard_id, spdk_strerror(objerrno));
    ctx->cb_fn(ctx->args, objerrno);
    spdk_free(bc->ctx->uuid_buf);
    delete ctx;
    delete bc;
    return;
  }

  SPDK_INFOLOG(blob_log, "create super blob: %lu in shard: %u done\n", blobid, ctx->shard_id);
  bc->blobid = blobid;
  spdk_bs_set_super(bc->bs, blobid, set_super_complete, bc);
}

static void
super_get_xattr_value(void *arg, const char *name, const void **value, size_t *value_len)
{
  struct create_blob_ctx *ctx = (struct create_blob_ctx *)arg;

  if (!strcmp("type", name))
  {
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

  if (bserrno)
  {
    SPDK_ERRLOG("blobstore init in shard %u failed: %s\n", ctx->shard_id, spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    spdk_free(ctx->uuid_buf);
    delete ctx;
    return;
  }

  g_bs_mgr->set_blobstore(ctx->shard_id, bs);
  g_bs_mgr->create_channel(ctx->shard_id, bs);

  uint64_t free = spdk_bs_free_cluster_count(bs);
  SPDK_INFOLOG(blob_log, "blobstore has FREE clusters of %lu, in thread %lu, in shard %u\n",
      free, utils::get_spdk_thread_id(), core_sharded::get_core_sharded().this_shard_id());

  struct create_blob_ctx *bc = new create_blob_ctx();
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

/**
 * spdk_bs_init, 初始化blobstore
 */
static void
bs_init(void *arg, int bserrno) {
    struct bm_context *ctx = (struct bm_context *)arg;

    //spdk_bs_load失败后，bs会释放，bs->dev也会释放，需要重新生成bs_dev
    struct spdk_bs_opts opts = {};
    spdk_bs_opts_init(&opts, sizeof(opts));
    memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
    default_blobstore_type.copy(opts.bstype.bstype, SPDK_BLOBSTORE_TYPE_LENGTH - 1);
    opts.cluster_sz = utils::default_blobstore_cluster_size;
    opts.start_offset = ctx->bs_start_offset;
	  opts.length = ctx->bs_length;

    struct spdk_bs_dev *bs_dev = NULL;
    int rc = spdk_bdev_create_bs_dev_ext(ctx->bdev_name.c_str(), fb_event_cb, NULL, &bs_dev);
    if (rc != 0) {
      SPDK_ERRLOG("create bs_dev in shard %u failed: %s\n", ctx->shard_id, spdk_strerror(-rc));
      ctx->cb_fn(ctx->args, rc);
      spdk_free(ctx->uuid_buf);
      delete ctx;
      return;
    }

    SPDK_INFOLOG(blob_log, "blobstore init in shard %u ...\n", ctx->shard_id);
    spdk_bs_init(bs_dev, &opts, bs_init_complete, ctx);
}

static void
bs_read_complete(void *args, struct spdk_blob_store *bs, int bserrno)
{
  struct bm_context *ctx = (struct bm_context *)args;

  SPDK_INFOLOG(blob_log, "blobstore read done, in shard %u\n", ctx->shard_id);
  if (bserrno == 0) {
    SPDK_WARNLOG("There is data on the disk!\n");

    if (ctx->force) {
        SPDK_WARNLOG("\'--force\' option configured, force to mkfs!\n");
        // 要先把已经load的 blobstore destroy掉
        spdk_bs_destroy(bs, bs_init, ctx);
        return;
    } else {
        SPDK_ERRLOG("If you intend to mkfs arbitrarily, please use \'--force\' option.\n");
        g_bs_mgr->set_blobstore(ctx->shard_id, bs);
        g_bs_mgr->create_channel(ctx->shard_id, bs);
        ctx->cb_fn(ctx->args, err::RAFT_ERR_DISK_NOT_EMPTY);
        spdk_free(ctx->uuid_buf);
        delete ctx;
        return;
    }
  }

  bs_init(ctx, 0);
}

static void
_blobstore_init(bm_complete cb_fn, void* args, std::string &bdev_name, std::string &uuid, bool force, uint32_t shard_id){
  struct spdk_bs_dev *bs_dev = NULL;
  int rc = spdk_bdev_create_bs_dev_ext(bdev_name.c_str(), fb_event_cb, NULL, &bs_dev);
  if (rc != 0) {
    SPDK_ERRLOG("create bs_dev in shard %u failed: %s\n", shard_id, spdk_strerror(-rc));
    cb_fn(args, rc);
    return;
  }

  uint64_t cluster_count = bs_dev->blockcnt / (utils::default_blobstore_cluster_size / bs_dev->blocklen);
  auto core_count = core_sharded::get_core_sharded().count();
  uint64_t cluster_count_per_core = cluster_count / core_count;
  uint64_t start_cluster = shard_id * cluster_count_per_core;
  uint64_t bs_start_offset = start_cluster * utils::default_blobstore_cluster_size;
  uint64_t bs_length = cluster_count_per_core * utils::default_blobstore_cluster_size;

  g_bs_space[shard_id] = std::make_pair(bs_start_offset, bs_length);
  SPDK_INFOLOG(blob_log, "shard_id %u, cluster_count %lu, core_count %u, cluster_count_per_core %lu, start_cluster %lu, bs_start_offset %lu, bs_length %lu\n",
      shard_id, cluster_count, core_count, cluster_count_per_core, start_cluster, bs_start_offset, bs_length);


  SPDK_INFOLOG(blob_log, "blobstore init in shard %u ...\n", shard_id);
  auto ctx = new bm_context{.cb_fn = std::move(cb_fn), .args = args, .osd_uuid = nullptr, .bdev_name = bdev_name,
          .force = force, .bs_start_offset = bs_start_offset, .bs_length = bs_length, .shard_id = shard_id};
  ctx->uuid_len = 4096;
  ctx->uuid_buf = (char*)spdk_zmalloc(ctx->uuid_len, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
  uuid.copy(ctx->uuid_buf, ctx->uuid_len - 1);

  struct spdk_bs_opts opts = {};
  spdk_bs_opts_init(&opts, sizeof(opts));
  opts.cluster_sz = utils::default_blobstore_cluster_size;
  opts.start_offset = bs_start_offset;
	opts.length = bs_length;
  spdk_bs_load(bs_dev, &opts, bs_read_complete, ctx);
}

static void
_blobstore_init(std::string bdev_name, std::string uuid, bool force, utils::context *ctx, uint32_t shard_id){
  SPDK_INFOLOG(blob_log, "blobstore init, thread id %lu, shard %u\n", utils::get_spdk_thread_id(),
      core_sharded::get_core_sharded().this_shard_id());

  _blobstore_init([shard_id](void *arg, int serror){
      utils::context *ctx = (utils::context *)arg;
      SPDK_INFOLOG(blob_log, "blobstore init done, in shard %u\n", shard_id);
      ctx->complete(serror);
    },
  ctx, bdev_name, uuid, force, shard_id);
}

void
blobstore_init(std::string &bdev_name, const std::string& uuid, bool force, bm_complete cb_fn, void* args){
  auto &shard = core_sharded::get_core_sharded();

  auto cur_thread = spdk_get_thread();
  SPDK_NOTICELOG("blobstore init, from thread id: %lu\n", utils::get_spdk_thread_id());

  auto shard_num = core_sharded::get_core_sharded().count();
  g_bs_mgr = std::make_shared<blobstore_managers>(shard_num);
  g_bs_space.resize(shard_num);

  auto ctx = new utils::switch_core_context{.cb_fn = std::move(cb_fn), .arg = args, .thread = cur_thread, .serror = 0};
  utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, utils::switch_core_func, ctx);

  for (uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
    shard.invoke_on(
      shard_id,
      [bdev_name = bdev_name, uuid, force, complete, shard_id](){
        _blobstore_init(bdev_name, std::move(uuid), force, complete, shard_id);
      }
    );
  }
}
/*************** End of blobstore_init ****************/

/*************** Start of blobstore_load ****************/
/**
 * spdk_bs_load：加载已经存在的blobstore
 */
static void
load_done(void *args, int bserrno) {
  struct bm_context *ctx = (struct bm_context *)args;

  if (bserrno) {
    SPDK_ERRLOG("blob_manager load_done in shard %u failed %s\n", ctx->shard_id, spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  SPDK_INFOLOG(blob_log, "blob_manager load_done, in shard %u, call cb_fn(args, 0)\n", ctx->shard_id);
  ctx->cb_fn(ctx->args, err::E_SUCCESS);
  delete ctx;
}

void parse_kv_xattr(struct spdk_blob *blob) {
  auto xattr = kv_xattr::parse_xattr(blob);

  SPDK_INFOLOG(blob_log, "kv xattr, blob id: %lu shard_id: %u\n", spdk_blob_get_id(blob), xattr.shard_id);
  if (xattr.shard_id > g_bs_mgr->size()) {
      SPDK_ERRLOG("kv xattr, shard_id: %u bigger than shard num: %u", xattr.shard_id, g_bs_mgr->size());
      return;
  }

  g_bs_mgr->get_blob_tree(xattr.shard_id).kv_blob = spdk_blob_get_id(blob);
}

void parse_kv_checkpoint_xattr(struct spdk_blob *blob) {
  auto xattr = kv_checkpoint_xattr::parse_xattr(blob);

  SPDK_INFOLOG(blob_log, "kv checkpoint xattr, blob id: %lu shard_id: %u\n", spdk_blob_get_id(blob), xattr.shard_id);
  if (xattr.shard_id > g_bs_mgr->size()) {
      SPDK_ERRLOG("kv checkpoint xattr, shard_id: %u bigger than shard num: %u", xattr.shard_id, g_bs_mgr->size());
      return;
  }

  auto &blobs = g_bs_mgr->get_blob_tree(xattr.shard_id);
  blobs.kv_checkpoint_blob = spdk_blob_get_id(blob);
}

void parse_kv_new_checkpoint_xattr(struct spdk_blob *blob) {
  auto xattr = kv_checkpoint_new_xattr::parse_xattr(blob);

  SPDK_INFOLOG(blob_log, "kv checkpoint new xattr, blob id: %lu shard_id: %u\n", spdk_blob_get_id(blob), xattr.shard_id);
  if (xattr.shard_id > g_bs_mgr->size()) {
      SPDK_ERRLOG("kv checkpoint new xattr, shard_id: %u bigger than shard num: %u", xattr.shard_id, g_bs_mgr->size());
      return;
  }

  auto &blobs = g_bs_mgr->get_blob_tree(xattr.shard_id);
  blobs.kv_checkpoint_blob = spdk_blob_get_id(blob);
}

void parse_log_xattr(struct spdk_blob *blob) {
  auto xattr = log_xattr::parse_xattr(blob);

  if (xattr.shard_id > g_bs_mgr->size()) {
      SPDK_ERRLOG("log xattr, shard_id: %u bigger than shard num: %u", xattr.shard_id, g_bs_mgr->size());
      return;
  }

  SPDK_INFOLOG(blob_log, "log xattr, blob id: %lu shard_id: %u pg: %s blob size:%lu\n",
      spdk_blob_get_id(blob), xattr.shard_id, xattr.pg.c_str(),
      spdk_blob_get_num_clusters(blob) * spdk_bs_get_cluster_size(g_bs_mgr->get_blobstore(xattr.shard_id)));

  g_bs_mgr->get_blob_tree(xattr.shard_id).log_blobs[xattr.pg] = blob;
}

void parse_object_xattr(struct spdk_blob *blob) {
  auto xattr = object_xattr::parse_xattr(blob);

  SPDK_INFOLOG(blob_log, "object xattr, blob id: %lu shard_id: %u pg: %s obj_name: %s\n",
      spdk_blob_get_id(blob), xattr.shard_id, xattr.pg.c_str(), xattr.obj_name.c_str());

  if (xattr.shard_id > g_bs_mgr->size()) {
      SPDK_ERRLOG("object xattr, shard_id: %u bigger than shard num: %u", xattr.shard_id, g_bs_mgr->size());
      return;
  }

  auto &blobs = g_bs_mgr->get_blob_tree(xattr.shard_id);
  auto [pg_it, _] = blobs.object_blobs.try_emplace(xattr.pg);
  auto [it, __] = pg_it->second.try_emplace(xattr.obj_name);
  auto& object = it->second;

  object.origin.blob = blob;
  object.origin.blobid = spdk_blob_get_id(blob);
}

void parse_object_snap_xattr(struct spdk_blob *blob) {
  auto xattr = object_snap_xattr::parse_xattr(blob);

  SPDK_INFOLOG(blob_log, "object snap xattr, blob id: %lu shard_id: %u pg: %s obj_name: %s snap_name: %s\n",
      spdk_blob_get_id(blob), xattr.shard_id, xattr.pg.c_str(), xattr.obj_name.c_str(), xattr.snap_name.c_str());

  if (xattr.shard_id > g_bs_mgr->size()) {
      SPDK_ERRLOG("object snap xattr, shard_id: %u bigger than shard num: %u", xattr.shard_id, g_bs_mgr->size());
      return;
  }

  auto &blobs = g_bs_mgr->get_blob_tree(xattr.shard_id);
  auto [pg_it, _] = blobs.object_blobs.try_emplace(xattr.pg);
  auto [it, __] = pg_it->second.try_emplace(xattr.obj_name);
  auto& object = it->second;

  object_store::snap snapshot;
  snapshot.snap_blob.blobid = spdk_blob_get_id(blob);
  snapshot.snap_blob.blob = blob;
  snapshot.snap_name = xattr.snap_name;
  object.snap_list.emplace_back(std::move(snapshot));
}

void parse_object_recover_xattr(struct spdk_blob *blob) {
  auto xattr = object_recover_xattr::parse_xattr(blob);

  SPDK_INFOLOG(blob_log, "object recover xattr, blob id: %lu shard_id: %u pg: %s obj_name: %s\n",
      spdk_blob_get_id(blob), xattr.shard_id, xattr.pg.c_str(), xattr.obj_name.c_str());

  if (xattr.shard_id > g_bs_mgr->size()) {
      SPDK_ERRLOG("object recover xattr, shard_id: %u bigger than shard num: %u", xattr.shard_id, g_bs_mgr->size());
      return;
  }

  auto &blobs = g_bs_mgr->get_blob_tree(xattr.shard_id);
  auto [pg_it, _] = blobs.object_blobs.try_emplace(xattr.pg);
  auto [it, __] = pg_it->second.try_emplace(xattr.obj_name);
  auto& object = it->second;

  object.recover.blob = blob;
  object.recover.blobid = spdk_blob_get_id(blob);
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
  // SPDK_INFOLOG(blob_log, "blob id %lu type: %s\n", spdk_blob_get_id(blob), type_str.c_str());
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
    case blob_type::object_snap:
      parse_object_snap_xattr(blob);
      break;
    case blob_type::object_recover:
      parse_object_recover_xattr(blob);
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


void parse_open_blob_xattr(struct spdk_blob *blob, uint32_t shard_id, bm_complete cb_fn, void* args){
  //参数blob随后会在外部关闭
  blob_type *type;
  size_t len;
  int rc;

  rc = spdk_blob_get_xattr_value(blob, "type", (const void**)&type, &len);
  if (rc < 0){
    cb_fn(args, 0);
    return;
  }

  auto bs = g_bs_mgr->get_blobstore(shard_id);
  auto type_str = type_string(*type);
  SPDK_INFOLOG(blob_log, "blob id: %lu type: %s, shard: %u\n", spdk_blob_get_id(blob), type_str.c_str(), shard_id);
  parse_blob_ctx *ctx = new parse_blob_ctx{.cb_fn = std::move(cb_fn), .args = args, .type = *type};
  switch (*type){
  case blob_type::log:
  case blob_type::object:
  case blob_type::object_snap:
  case blob_type::object_recover:
    spdk_bs_open_blob(bs, spdk_blob_get_id(blob),
		       parse_blob_xattr, ctx);
    break;
  case blob_type::kv:
  case blob_type::kv_checkpoint:
  case blob_type::kv_checkpoint_new:
    parse_blob_xattr(ctx, blob, 0);
    break;
  case blob_type::super_blob:
  case blob_type::free:
    // SPDK_WARNLOG("blob id: %lu type: %s skip!\n", spdk_blob_get_id(blob), type_str.c_str());
  default:
    SPDK_INFOLOG(blob_log, "blob id: %lu type: %s shard: %u falls into default!\n", spdk_blob_get_id(blob), type_str.c_str(), shard_id);
    ctx->cb_fn(ctx->args, 0);
    delete ctx;
    break;
  }
}

static void
blob_iter_complete(void *args, struct spdk_blob *blob, int bserrno)
{
  struct bm_context *ctx = (struct bm_context *)args;
  auto err_hand = [](void *arg, int berrno){
    if (berrno) {
      struct bm_context *ctx = (struct bm_context *)arg;
      if (berrno == -ENOENT) {
        // ENOENT 表示迭代顺利完成.
        auto bs = g_bs_mgr->get_blobstore(ctx->shard_id);
        SPDK_INFOLOG(blob_log, "blob iteration complete in shard %u\n", ctx->shard_id);
        g_bs_mgr->start_blob_pool(ctx->shard_id, load_done, ctx);
        return;
      }
      SPDK_ERRLOG("blob iteration in shard %u failed: %s\n", ctx->shard_id, spdk_strerror(berrno));
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
      ctx->shard_id,
      [blob, err_hand = std::move(err_hand)](void *args, int berrno)
      {
        struct bm_context *ctx = (struct bm_context *)args;
        SPDK_INFOLOG(blob_log, "in shard %u, parse_open_blob_xattr, berrno %d\n", ctx->shard_id, berrno);
        if (berrno)
        {
          return err_hand(args, berrno);
        }
        auto bs = g_bs_mgr->get_blobstore(ctx->shard_id);
        spdk_bs_iter_next(bs, blob, blob_iter_complete, args);
      },
      args);
}

static void close_super_complete(void *cb_arg, int bserrno){
  struct bm_context *ctx = (struct bm_context *)cb_arg;
  if (bserrno) {
    SPDK_ERRLOG("close super blob in shard %u failed %s\n", ctx->shard_id, spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  auto bs = g_bs_mgr->get_blobstore(ctx->shard_id);
  SPDK_INFOLOG(blob_log, "close super blob done, in shard %u\n", ctx->shard_id);
  // 读取每个blob的xattr，放入map中，准备还原blob运行状态
  spdk_bs_iter_first(bs, blob_iter_complete, ctx);
}

static void read_super_blob_complete(void *arg, int bserrno) {
  struct bm_context *ctx = (struct bm_context *)arg;

  if (bserrno) {
    SPDK_ERRLOG("open super blob in shard %u failed %s\n", ctx->shard_id, spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    if(ctx->uuid_buf)
        spdk_free(ctx->uuid_buf);
    delete ctx;
    return;
  }

  SPDK_INFOLOG(blob_log, "read super blob done, in shard %u, uuid %s\n", ctx->shard_id, ctx->uuid_buf);
  if(ctx->osd_uuid)
      *(ctx->osd_uuid) = ctx->uuid_buf;
  spdk_free(ctx->uuid_buf);

  spdk_blob_close(ctx->blob, close_super_complete, ctx);
}

static void open_super_blob_complete(void *arg, struct spdk_blob *blob, int objerrno){
  struct bm_context *ctx = (struct bm_context *)arg;
  if (objerrno) {
    SPDK_ERRLOG("open super blob in shard %u failed %s\n", ctx->shard_id, spdk_strerror(objerrno));
    ctx->cb_fn(ctx->args, objerrno);
    delete ctx;
    return;
  }

  ctx->blob = blob;
  auto channel = global_io_channel(ctx->shard_id);
  ctx->uuid_len = 4096;
  ctx->uuid_buf = (char*)spdk_zmalloc(ctx->uuid_len, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
  spdk_blob_io_read(blob, channel, ctx->uuid_buf, 0,
		      ctx->uuid_len / blob_unit_size,
		      read_super_blob_complete, ctx);
}

static void get_super_complete(void *cb_arg, spdk_blob_id blobid, int bserrno){
  struct bm_context *ctx = (struct bm_context *)cb_arg;
  if (bserrno) {
    SPDK_ERRLOG("get super in shard %u failed %s\n", ctx->shard_id, spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  auto bs = g_bs_mgr->get_blobstore(ctx->shard_id);
  SPDK_INFOLOG(blob_log, "get super blob: %lu in shard %u done\n", blobid, ctx->shard_id);
  spdk_bs_open_blob(bs, blobid, open_super_blob_complete, ctx);
}

static void
bs_load_complete(void *args, struct spdk_blob_store *bs, int bserrno)
{
  struct bm_context *ctx = (struct bm_context *)args;

  if (bserrno) {
    SPDK_ERRLOG("blobstore load failed, do you have blobstore on this disk? %s\n", err::string_status(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  SPDK_INFOLOG(blob_log, "blobstore load success, in shard %u\n", ctx->shard_id);
  g_bs_mgr->set_blobstore(ctx->shard_id, bs);
  g_bs_mgr->create_channel(ctx->shard_id, bs);

  uint64_t free = spdk_bs_free_cluster_count(bs);
  SPDK_INFOLOG(blob_log, "blobstore has FREE clusters of %lu, in thread %lu, shard %u\n",
      free, utils::get_spdk_thread_id(), core_sharded::get_core_sharded().this_shard_id());
  spdk_bs_get_super(bs, get_super_complete, ctx);
}

static void
_blobstore_load(std::string &bdev_name, bm_complete cb_fn, void* args, std::string *osd_uuid, uint32_t shard_id) {
  struct spdk_bs_dev *bs_dev = NULL;

  int rc = spdk_bdev_create_bs_dev_ext(bdev_name.c_str(), fb_event_cb, NULL, &bs_dev);
  if (rc != 0) {
    SPDK_ERRLOG("create bs_dev in shard %u failed: %s\n", shard_id, spdk_strerror(-rc));
    cb_fn(args, rc);
    return;
  }

  if(shard_id >= g_bs_space.size()){
    SPDK_ERRLOG("The blobstore space scope has not been configured, in shard %u\n", shard_id);
    cb_fn(args, err::E_INVAL);
    return;
  }
  uint64_t bs_start_offset = g_bs_space[shard_id].first;
  uint64_t bs_length = g_bs_space[shard_id].second;

  auto ctx = new bm_context{.cb_fn = cb_fn, .args = args, .osd_uuid = osd_uuid, .shard_id = shard_id};

  struct spdk_bs_opts opts = {};
  spdk_bs_opts_init(&opts, sizeof(opts));
  opts.cluster_sz = utils::default_blobstore_cluster_size;
  opts.start_offset = bs_start_offset;
	opts.length = bs_length;
  spdk_bs_load(bs_dev, &opts, bs_load_complete, ctx);
}

static void
_blobstore_load(std::string bdev_name, std::string *osd_uuid, utils::context *ctx, uint32_t shard_id){
  SPDK_INFOLOG(blob_log, "blobstore load, thread id %lu, shard %u\n", utils::get_spdk_thread_id(),
      core_sharded::get_core_sharded().this_shard_id());

  _blobstore_load(
    bdev_name,
    [shard_id](void *arg, int serror){
      utils::context *ctx = (utils::context *)arg;
      SPDK_INFOLOG(blob_log, "blobstore load done, in shard %u\n", shard_id);
      ctx->complete(serror);
    },
    ctx,
    osd_uuid,
    shard_id
  );
}

void
blobstore_load(std::string &bdev_name, bm_complete cb_fn, void* args, std::string *osd_uuid){
  auto &shard = core_sharded::get_core_sharded();

  auto shard_num = core_sharded::get_core_sharded().count();
  if(g_bs_space.size() != shard_num){
    cb_fn(args, err::E_INVAL);
    return;
  }
  g_bs_mgr = std::make_shared<blobstore_managers>(shard_num);

  auto cur_thread = spdk_get_thread();
  auto ctx = new utils::switch_core_context{.cb_fn = std::move(cb_fn), .arg = args, .thread = cur_thread, .serror = 0};
  utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, utils::switch_core_func, ctx);

  for (uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
    shard.invoke_on(
      shard_id,
      [bdev_name = bdev_name, osd_uuid, complete, shard_id](){
        //只需要第一个核获取uuid
        if(shard_id == 0)
          _blobstore_load(bdev_name, osd_uuid, complete, shard_id);
        else
          _blobstore_load(bdev_name, nullptr, complete, shard_id);
      }
    );
  }
}
/*************** End of blobstore_load ****************/

/*************** Start of blobstore_fini ****************/
/**
 * spdk_bs_unload 卸载blobstore
 */
static void
bs_fini_complete(void *args, int bserrno)
{
  struct bm_context *ctx = (struct bm_context *)args;

  if (bserrno) {
    SPDK_ERRLOG("blobstore unload in shard %u  failed: %s\n", ctx->shard_id, spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  SPDK_INFOLOG(blob_log, "blobstore unload success in shard %u.\n", ctx->shard_id);
  g_bs_mgr->set_blobstore(ctx->shard_id, nullptr);
  ctx->cb_fn(ctx->args, 0);
  delete ctx;
}

static void pool_stop_complete(void *args, int bserrno) {
  struct bm_context *ctx = (struct bm_context *)args;

  if (bserrno) {
    SPDK_ERRLOG("blobpool stop in shard %u failed: %s\n", ctx->shard_id, spdk_strerror(bserrno));
    ctx->cb_fn(ctx->args, bserrno);
    delete ctx;
    return;
  }

  auto bs = g_bs_mgr->get_blobstore(ctx->shard_id);
  SPDK_INFOLOG(blob_log, "blobstore unload in shard %u ...\n", ctx->shard_id);
  spdk_bs_unload(bs, bs_fini_complete, ctx);
}

static void
_blobstore_fini(bm_complete cb_fn, void* args, uint32_t shard_id)
{
  if (g_bs_mgr && shard_id < g_bs_mgr->size()) {
    auto ctx = new bm_context{.cb_fn = cb_fn, .args = args, .shard_id = shard_id};
    g_bs_mgr->free_channel(shard_id);
    SPDK_INFOLOG(blob_log, "blobpool stop in thread %lu, shard %u\n",
        utils::get_spdk_thread_id(), shard_id);
    g_bs_mgr->stop_blob_pool(shard_id, pool_stop_complete, ctx);
	}else{
    cb_fn(args, 0);
  }
}

static void
_blobstore_fini(utils::context *ctx, uint32_t shard_id){
  SPDK_INFOLOG(blob_log, "blobstore fini, thread id %lu, shard %u\n",
      utils::get_spdk_thread_id(), core_sharded::get_core_sharded().this_shard_id());

  _blobstore_fini([shard_id](void *arg, int serror){
    utils::context *ctx = (utils::context *)arg;
    SPDK_INFOLOG(blob_log, "blobstore fini done in shard %u\n", shard_id);
    ctx->complete(serror);
  },
  ctx, shard_id);
}

void
blobstore_fini(bm_complete cb_fn, void* args){
  auto &shard = core_sharded::get_core_sharded();

  auto cur_thread = spdk_get_thread();
  auto shard_num = core_sharded::get_core_sharded().count();

  auto ctx = new utils::switch_core_context{.cb_fn = std::move(cb_fn), .arg = args, .thread = cur_thread, .serror = 0};
  utils::multi_complete *complete = new utils::multi_complete(shard_num, shard_num, utils::switch_core_func, ctx);

  for (uint32_t shard_id = 0; shard_id < shard_num; shard_id++){
    shard.invoke_on(
      shard_id,
      [complete, shard_id](){
        _blobstore_fini(complete, shard_id);
      }
    );
  }
}
/*************** End of blobstore_fini ****************/
