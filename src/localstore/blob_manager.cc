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

#include <spdk/log.h>
#include <spdk/string.h>
#include <functional>
#include <string>

SPDK_LOG_REGISTER_COMPONENT(blob_log)

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
  bm_complete cb_fn;
  void*       args;
};

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
    delete ctx;
    return;
  }

  // SPDK_WARNLOG("blobstore init success.\n");
  std::construct_at(&g_bs_mgr);
  g_bs_mgr.blobstore = bs;
  g_bs_mgr.channel = spdk_bs_alloc_io_channel(bs);

  free = spdk_bs_free_cluster_count(bs);
  SPDK_INFOLOG(blob_log, "blobstore has FREE clusters of %lu\n", free);

  ctx->cb_fn(ctx->args, 0);
  delete ctx;
}

void
blobstore_init(const char *bdev_name, bm_complete cb_fn, void* args) {
  struct spdk_bs_dev *bs_dev = NULL;

  SPDK_INFOLOG(blob_log, "create bs_dev\n");
  int rc = spdk_bdev_create_bs_dev_ext(bdev_name, fb_event_cb, NULL, &bs_dev);
  if (rc != 0) {
    SPDK_ERRLOG("create bs_dev failed: %s\n", spdk_strerror(-rc));
    spdk_app_stop(-1);
    return;
  }

  auto ctx = new bm_context{cb_fn, args};
  SPDK_INFOLOG(blob_log, "blobstore init...\n");
  spdk_bs_init(bs_dev, NULL, bs_init_complete, ctx);
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
    cb_fn(args, 0);
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
      if(berrno){
        return err_hand(args, berrno);
      }
      spdk_bs_iter_next(g_bs_mgr.blobstore, blob, blob_iter_complete, args);
    },
    args);
}

static void
bs_load_complete(void *args, struct spdk_blob_store *bs, int bserrno)
{ 
  struct bm_context *ctx = (struct bm_context *)args;
  uint64_t free = 0;

  if (bserrno) {
    SPDK_ERRLOG("blobstore load failed, do you have blobstore on this disk? %s\n", spdk_strerror(bserrno));
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

  // 读取每个blob的xattr，放入map中，准备还原blob运行状态
  spdk_bs_iter_first(g_bs_mgr.blobstore, blob_iter_complete, ctx);
}

void
blobstore_load(const char *bdev_name, bm_complete cb_fn, void* args) {
  struct spdk_bs_dev *bs_dev = NULL;

  SPDK_INFOLOG(blob_log, "create bs_dev...\n");
  int rc = spdk_bdev_create_bs_dev_ext(bdev_name, fb_event_cb, NULL, &bs_dev);
  if (rc != 0) {
    SPDK_ERRLOG("create bs_dev failed: %s\n", spdk_strerror(-rc));
    spdk_app_stop(-1);
    return;
  }

  SPDK_INFOLOG(blob_log, "blobstore load...\n");
  auto ctx = new bm_context{cb_fn, args};
  spdk_bs_load(bs_dev, NULL, bs_load_complete, ctx);
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

  ctx->cb_fn(ctx->args, 0);
  delete ctx;
}

void
blobstore_fini(bm_complete cb_fn, void* args)
{
	SPDK_INFOLOG(blob_log, "blobstore_fini.\n");
	if (g_bs_mgr.blobstore) {
		if (g_bs_mgr.channel) {
      SPDK_INFOLOG(blob_log, "free io_channel\n");
			spdk_bs_free_io_channel(g_bs_mgr.channel);
		}
    auto ctx = new bm_context{cb_fn, args};
    spdk_bs_unload(g_bs_mgr.blobstore, bs_fini_complete, ctx);
	}else{
    cb_fn(args, 0);
  }
}