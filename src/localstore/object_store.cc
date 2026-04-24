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
#include "types.h"
#include "object_store.h"
#include "fastblock/base/core_sharded.h"

#include "fastblock/utils/utils.h"

#include <spdk/log.h>
#include <spdk/env.h>
#include <functional>

SPDK_LOG_REGISTER_COMPONENT(object_store)

struct blob_rw_ctx {
  bool is_read;
  struct spdk_blob* blob;
  struct spdk_io_channel* channel;

  // 读写参数列表
  std::string object_name;
  uint64_t offset;
  char*    buf;
  uint64_t len;
  uint64_t submit_tick;
  object_rw_complete cb_fn;
  void*    arg;

  // 非对齐时，会用到这些变量
  bool     is_aligned;
  uint64_t start_lba;
  uint64_t num_lba;
  char*    pin_buf;
  uint32_t blocklen;
};

struct blob_create_ctx {
  bool     is_read;
  char*    buf;
  uint64_t offset;
  uint64_t len;
  std::string  object_name;
  // create之后，然后调用open时使用
  spdk_blob_id blobid;
  struct object_store* mgr;
  object_rw_complete cb_fn;
  void*              arg;
  uint32_t shard_id;
  std::string pg;
  blob_type type;
  fb_blob blob;
  object_xattr xattr;
};

struct blob_stop_ctx {
  object_store::container table;
  object_store::iterator  it;
  spdk_blob_store*        bs;
  object_rw_complete      cb_fn;
  void*                   arg;
};

struct snap_create_ctx {
  std::string        object_name;
  std::string        snap_name;
  object_rw_complete cb_fn;
  void*              arg;

  object_store::snap       snap;
  object_store::container* hashtable;
  object_snap_xattr xattr;
};

struct snap_delete_ctx {
  std::string        object_name;
  std::string        snap_name;
  object_rw_complete cb_fn;
  void*              arg;

  spdk_blob_id             blob_id;
  object_store::container* hashtable;
};

struct recover_create_ctx {
  std::string        object_name;
  object_rw_complete cb_fn;
  void*              arg;

  spdk_blob_store*         bs;
  object_store::container* hashtable;
  object_recover_xattr xattr;
};

//读取快照时函数的上下文状态
struct recover_delete_ctx {
  std::string        object_name;
  object_rw_complete cb_fn;
  void*              arg;

  spdk_blob_store*         bs;
  fb_blob    blob;
  object_store::container* hashtable;
};

struct recover_read_ctx {
  std::string        object_name;
  object_rw_complete cb_fn;
  void*              arg;
};

struct object_delete_ctx {
  std::string object_name;
  object_rw_complete cb_fn;
  void* arg;

  spdk_blob_store* bs;
  fb_blob blob;
  object_store::container* hashtable;
};

void object_store::write(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
                      uint64_t offset, char* buf, uint64_t len,
                      object_rw_complete cb_fn, void* arg)
{
    readwrite(xattr, object_name, offset, buf, len, cb_fn, arg, 0);
}

void object_store::read(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
                     uint64_t offset, char* buf, uint64_t len,
                     object_rw_complete cb_fn, void* arg)
{
    readwrite(xattr, object_name, offset, buf, len, cb_fn, arg, 1);
}

void object_store::delete_object(std::string object_name,
                                 object_rw_complete cb_fn, void* arg)
{
  auto it = table.find(object_name);
  struct object_delete_ctx* ctx;

  if (it == table.end() || it->second.origin.blobid == 0 ||
      it->second.origin.blob == nullptr) {
    cb_fn(arg, 0);
    return;
  }

  ctx = new object_delete_ctx();
  ctx->object_name = object_name;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;
  ctx->bs = bs;
  ctx->blob = it->second.origin;
  ctx->hashtable = &table;

  spdk_blob_close(ctx->blob.blob, delete_close_complete, ctx);
}

void object_store::snap_create(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
        std::string snap_name, object_rw_complete cb_fn, void* arg) {
  struct snap_create_ctx* ctx;
  auto it = table.find(object_name);
  if (it == table.end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", object_name.c_str());
    cb_fn(arg, -EINVAL);
    return;
  }

  ctx = new snap_create_ctx();
  ctx->object_name = object_name;
  ctx->snap_name = snap_name;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;
  ctx->hashtable = &table;

  // note: 现在pg的保存在object_store中了，不需要每次都传进来了
  uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
  ctx->xattr = object_snap_xattr{.shard_id = shard_id, .pg = pg, .obj_name = object_name, .snap_name = snap_name};

  //调用spdk的spdk_bs_create_snapshot函数，创建快照，并通过snap_done返回创建的结果。
  struct spdk_blob_xattr_opts snapshot_xattrs;
  snapshot_xattrs.names = (char**)object_snap_xattr::xattr_names;
  snapshot_xattrs.count = object_snap_xattr::xattr_count;
  snapshot_xattrs.ctx = &(ctx->xattr);
  snapshot_xattrs.get_value = object_snap_xattr::get_xattr_value;

  // SPDK_NOTICELOG("object:%s snap:%s create\n", object_name.c_str(), snap_name.c_str());
  spdk_bs_create_snapshot(bs, it->second.origin.blobid, &snapshot_xattrs, snap_create_complete, ctx);
}

void object_store::snap_create_complete(void *arg, spdk_blob_id snap_id, int objerrno) {
  struct snap_create_ctx *ctx = (struct snap_create_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s snap_name:%s create failed:%s\n", ctx->object_name.c_str(), ctx->snap_name.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
    return;
  }

  auto it = ctx->hashtable->find(ctx->object_name);
  if (it == ctx->hashtable->end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", ctx->object_name.c_str());
    ctx->cb_fn(ctx->arg, -EINVAL);
    delete ctx;
    return;
  }

  ctx->snap.snap_blob.blobid = snap_id;
  ctx->snap.snap_blob.blob = nullptr;
  ctx->snap.snap_name = ctx->snap_name;
  it->second.snap_list.emplace_back(std::move(ctx->snap));
  // SPDK_NOTICELOG("object:%s snap:%s added, snap size:%lu\n",
  //     ctx->object_name.c_str(), ctx->snap_name.c_str(), it->second.snap_list.size());

  SPDK_DEBUGLOG(object_store, "object_name:%s snap_name:%s snapshot added.\n", ctx->object_name.c_str(), ctx->snap_name.c_str());
  ctx->cb_fn(ctx->arg, 0);
  delete ctx;
  return;
}

void object_store::snap_delete(std::string object_name, std::string snap_name, object_rw_complete cb_fn, void *arg) {
  struct snap_delete_ctx* ctx;
  spdk_blob_id del_blobid = 0;
  auto it = table.find(object_name);
  if (it == table.end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", object_name.c_str());
    cb_fn(arg, -EINVAL);
    return;
  }

  // 遍历snap_list，从中查找对应名字的snap
  auto& object = it->second;
  auto begin = object.snap_list.begin();
  auto end = object.snap_list.end();
  for (; begin != end ; ++begin) {
    if (begin->snap_name == snap_name) {
      del_blobid = begin->snap_blob.blobid;
      object.snap_list.erase(begin);
      break;
    }
  }
  if (!del_blobid) {
      SPDK_ERRLOG("object_name:%s snap_name:%s not found.\n", object_name.c_str(), snap_name.c_str());
      cb_fn(arg, -EINVAL);
      return;
  }

  ctx = new snap_delete_ctx();
  ctx->object_name = object_name;
  ctx->snap_name = snap_name;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;
  ctx->hashtable = &table;
  ctx->blob_id = del_blobid;
  // snap默认是关闭的，直接delete即可

  // SPDK_NOTICELOG("object:%s snap:%s delete blob_id:%lu\n", object_name.c_str(), snap_name.c_str(), del_blobid);
  spdk_bs_delete_blob(bs, ctx->blob_id, snap_delete_complete, ctx);
}

void object_store::snap_delete_complete(void *arg, int objerrno) {
  struct snap_delete_ctx* ctx = (struct snap_delete_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s snap_name:%s delete failed:%s\n", ctx->object_name.c_str(), ctx->snap_name.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
    return ;
  }

  SPDK_DEBUGLOG(object_store, "object_name:%s snap_name:%s deleted.\n", ctx->object_name.c_str(), ctx->snap_name.c_str());
  ctx->cb_fn(ctx->arg, 0);
  delete ctx;
  return;
}
/************************************************************************************************************************************/
void object_store::recovery_create(std::map<std::string, xattr_val_type>& xattr, std::string object_name, object_rw_complete cb_fn, void* arg) {
  struct recover_create_ctx* ctx;
  auto it = table.find(object_name);
  if (it == table.end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", object_name.c_str());
    cb_fn(arg, -EINVAL);
    return;
  }

  ctx = new recover_create_ctx();
  ctx->object_name = object_name;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;
  ctx->bs = bs;
  ctx->hashtable = &table;
  uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
  ctx->xattr = object_recover_xattr{.shard_id = shard_id, .pg = pg, .obj_name = object_name};

  struct spdk_blob_xattr_opts recovery_xattrs;
  recovery_xattrs.names = (char**)object_recover_xattr::xattr_names;
  recovery_xattrs.count = object_recover_xattr::xattr_count;
  recovery_xattrs.ctx = &(ctx->xattr);
  recovery_xattrs.get_value = object_recover_xattr::get_xattr_value;
  // SPDK_NOTICELOG("object:%s recovery create, origin id:%lx\n", ctx->object_name.c_str(), it->second.origin.blobid);
  spdk_bs_create_snapshot(bs, it->second.origin.blobid, &recovery_xattrs, recovery_create_complete, ctx);
}

void object_store::recovery_create_complete(void *arg, spdk_blob_id blob_id, int objerrno) {
  struct recover_create_ctx *ctx = (struct recover_create_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s recovery snapshot create failed:%s\n", ctx->object_name.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
    return;
  }

  auto it = ctx->hashtable->find(ctx->object_name);
  if (it == ctx->hashtable->end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", ctx->object_name.c_str());
    ctx->cb_fn(ctx->arg, -EINVAL);
    delete ctx;
    return;
  }

  auto &object = it->second;
  object.recover.blob = nullptr;
  object.recover.blobid = blob_id;
  // SPDK_NOTICELOG("object:%s recovery open, blob_id:%lx\n", ctx->object_name.c_str(), blob_id);
  SPDK_DEBUGLOG(object_store, "object_name:%s recovery snapshot created.\n", ctx->object_name.c_str());
  spdk_bs_open_blob(ctx->bs, object.recover.blobid, recovery_open_complete, ctx);;
}

void object_store::recovery_open_complete(void *arg, struct spdk_blob *blob, int objerrno) {
  struct recover_create_ctx *ctx = (struct recover_create_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s recovery snapshot open failed:%s\n", ctx->object_name.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
    return;
  }

  auto it = ctx->hashtable->find(ctx->object_name);
  if (it == ctx->hashtable->end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", ctx->object_name.c_str());
    ctx->cb_fn(ctx->arg, -EINVAL);
    delete ctx;
    return;
  }

  auto &object = it->second;
  object.recover.blob = blob;
  // SPDK_NOTICELOG("object_name:%s, blob_id:%lx blob:%p.\n", ctx->object_name.c_str(), object.recover.blobid, object.recover.blob);
  SPDK_DEBUGLOG(object_store, "object_name:%s recovery snapshot opened.\n", ctx->object_name.c_str());
  ctx->cb_fn(ctx->arg, 0);
  delete ctx;
}

/*******************************************************************************************************************/

void object_store::recovery_delete(std::string object_name, object_rw_complete cb_fn, void *arg) {
  struct recover_delete_ctx* ctx;
  auto it = table.find(object_name);
  if (it == table.end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", object_name.c_str());
    cb_fn(arg, -EINVAL);
    return;
  }

  ctx = new recover_delete_ctx();
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;
  ctx->object_name = object_name;
  ctx->hashtable = &table;
  ctx->bs = bs;
  ctx->blob = it->second.recover;
  spdk_blob_close(ctx->blob.blob, recovery_close_complete, ctx);
}

void object_store::recovery_close_complete(void *arg, int objerrno) {
  struct recover_delete_ctx* ctx = (struct recover_delete_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s recovery snapshot close failed:%s\n", ctx->object_name.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
    return;
  }

  auto it = ctx->hashtable->find(ctx->object_name);
  if (it == ctx->hashtable->end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", ctx->object_name.c_str());
    ctx->cb_fn(ctx->arg, -EINVAL);
    delete ctx;
    return;
  }

  it->second.recover.blob = nullptr;
  // SPDK_NOTICELOG("object:%s recovery closed.\n", ctx->object_name.c_str());
  spdk_bs_delete_blob(ctx->bs, ctx->blob.blobid, recovery_delete_complete, ctx);
}

void object_store::recovery_delete_complete(void *arg, int objerrno) {
  struct recover_delete_ctx* ctx = (struct recover_delete_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s recovery snapshot delete failed:%s\n", ctx->object_name.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
    return;
  }

  auto it = ctx->hashtable->find(ctx->object_name);
  if (it == ctx->hashtable->end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", ctx->object_name.c_str());
    ctx->cb_fn(ctx->arg, -EINVAL);
    delete ctx;
    return;
  }

  it->second.recover.blobid = 0;
  // SPDK_NOTICELOG("object:%s recovery deleted.\n", ctx->object_name.c_str());
  SPDK_DEBUGLOG(object_store, "object_name:%s recovery snapshot opened.\n", ctx->object_name.c_str());
  ctx->cb_fn(ctx->arg, 0);
  delete ctx;
}

/***************************************************************************************************/

void object_store::recovery_read(std::string object_name, char *buf, object_rw_complete cb_fn, void *arg) {
  struct recover_read_ctx* ctx;
  auto it = table.find(object_name);
  if (it == table.end()) {
    SPDK_ERRLOG("object_name:%s doesn't exist.\n", object_name.c_str());
    cb_fn(arg, -EINVAL);
    return;
  }

  auto& recovery_snap = it->second.recover;
  if (recovery_snap.blobid == 0 || recovery_snap.blob == nullptr) {
    SPDK_ERRLOG("object_name:%s invalid recovery snapshot. blob_id:%lu blob:%p.\n",
        object_name.c_str(), recovery_snap.blobid, recovery_snap.blob);
    cb_fn(arg, -EINVAL);
    return;
  }

  ctx = new recover_read_ctx();
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;
  ctx->object_name = object_name;
  // SPDK_NOTICELOG("object_name:%s blob_id:%lx blob:%p.\n", object_name.c_str(), recovery_snap.blobid, recovery_snap.blob);
  spdk_blob_io_read(recovery_snap.blob, channel, buf, 0, object_store::blob_size / object_store::unit_size, recovery_read_complete, ctx);
}

void object_store::recovery_read_complete(void *arg, int objerrno) {
  struct recover_read_ctx* ctx = (struct recover_read_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s recovery snapshot read failed:%s\n", ctx->object_name.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
    return;
  }

  ctx->cb_fn(ctx->arg, 0);
  delete ctx;
}

void object_store::delete_close_complete(void *arg, int objerrno) {
  struct object_delete_ctx* ctx = (struct object_delete_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s close failed:%s\n",
        ctx->object_name.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
    return;
  }

  auto it = ctx->hashtable->find(ctx->object_name);
  if (it == ctx->hashtable->end()) {
    ctx->cb_fn(ctx->arg, -ENOENT);
    delete ctx;
    return;
  }

  it->second.origin.blob = nullptr;
  spdk_bs_delete_blob(ctx->bs, ctx->blob.blobid, delete_complete, ctx);
}

void object_store::delete_complete(void *arg, int objerrno) {
  struct object_delete_ctx* ctx = (struct object_delete_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s delete failed:%s\n",
        ctx->object_name.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
    return;
  }

  auto it = ctx->hashtable->find(ctx->object_name);
  if (it != ctx->hashtable->end()) {
    it->second.origin.blobid = 0;
    if (it->second.recover.blobid == 0 && it->second.snap_list.empty()) {
      ctx->hashtable->erase(it);
    }
  }

  ctx->cb_fn(ctx->arg, 0);
  delete ctx;
}

void object_store::readwrite(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
                     uint64_t offset, char* buf, uint64_t len,
                     object_rw_complete cb_fn, void* arg, bool is_read)
{
  SPDK_DEBUGLOG(object_store, "object %s offset:%lu len:%lu\n", object_name.c_str(), offset, len);
  if (offset + len > blob_size)
  {
    SPDK_DEBUGLOG(object_store, "object %s offset:%lu len:%lu beyond blob size %u\n",
                        object_name.c_str(), offset, len, blob_size);
    len = blob_size - offset;
  }

    auto it = table.find(object_name);
    if (it != table.end() && it->second.origin.blob != nullptr &&
        it->second.origin.blobid != 0) {
      SPDK_DEBUGLOG(object_store, "object %s found, blob id:%" PRIu64 "\n", object_name.c_str(), it->second.origin.blobid);
      blob_readwrite(it->second.origin.blob, channel, object_name, offset, buf, len, cb_fn, arg, is_read);
    } else if (is_read) {
      SPDK_DEBUGLOG(object_store, "object %s not found for read\n", object_name.c_str());
      cb_fn(arg, -ENOENT);
    } else {
      SPDK_DEBUGLOG(object_store, "object %s not found\n", object_name.c_str());
      create_blob(xattr, object_name, offset, buf, len, cb_fn, arg, is_read);
    }
}

void object_store::create_blob(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
                     uint64_t offset, char* buf, uint64_t len,
                     object_rw_complete cb_fn, void* arg, bool is_read)
{
  struct blob_create_ctx *ctx = new blob_create_ctx();
  ctx->is_read = is_read;
  ctx->mgr = this;
  ctx->object_name = object_name;
  ctx->offset = offset;
  ctx->buf = buf;
  ctx->len = len;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;
  uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
  ctx->shard_id = shard_id;
  ctx->xattr = object_xattr{.shard_id = shard_id, .pg = pg, .obj_name = object_name};

  // Reusing precreated free blobs currently corrupts blobstore replay across
  // restart in the prototype path. Create object blobs directly until the
  // restart path is stabilized.
  if (false && global_blob_pool(shard_id).has_free_blob()) {
      SPDK_DEBUGLOG(object_store, "[test] object get blob from pool.\n");
      auto blob = global_blob_pool(shard_id).get();

      ctx->xattr.blob_set_xattr(blob.blob); // 同步操作，直接设置xattr
      ctx->blob = blob;
      spdk_blob_sync_md(blob.blob, sync_md_done, ctx);
  } else {
      SPDK_DEBUGLOG(object_store, "[test] object get blob from create. type:%s pg:%s name:%s\n",
          type_string(ctx->xattr.type).c_str(), ctx->xattr.pg.c_str(), ctx->xattr.obj_name.c_str());
      struct spdk_blob_opts opts;

      spdk_blob_opts_init(&opts, sizeof(opts));
      opts.num_clusters = object_store::blob_cluster;
      opts.thin_provision = false;
      opts.use_extent_table = false;
      opts.xattrs.count = object_xattr::xattr_count;
      opts.xattrs.names = (char**)object_xattr::xattr_names;
      opts.xattrs.ctx = &(ctx->xattr);
      opts.xattrs.get_value = object_xattr::get_xattr_value; // 异步操作，create结束后自动 set xattr
      spdk_bs_create_blob_ext(bs, &opts, create_done, ctx);
  }
}

void object_store::sync_md_done(void *arg, int bserrno) {
    struct blob_create_ctx* ctx = (struct blob_create_ctx*)arg;

    if (bserrno) {
        SPDK_ERRLOG("sync md failed:%s\n", spdk_strerror(bserrno));
        ctx->cb_fn(ctx->arg, bserrno);
        delete ctx;
        return;
    }

    SPDK_DEBUGLOG(object_store, "sync md in core %u\n", ctx->shard_id);
    // 同步完md，先把blob放进map，然后执行读写
    struct object_store::object obj;
    obj.origin = ctx->blob;
    ctx->mgr->table.emplace(std::move(ctx->object_name), std::move(obj));

    blob_readwrite(ctx->blob.blob, ctx->mgr->channel, ctx->object_name, ctx->offset, ctx->buf, ctx->len,
                  ctx->cb_fn, ctx->arg, ctx->is_read);
    delete ctx;
}

/**
 * 下面都是static函数，因为c++的成员函数隐含的第一个参数是this指针，
 * 要传递给c语言的函数指针，需要写为static成员函数，或者非成员函数
 */
void object_store::blob_readwrite(struct spdk_blob *blob, struct spdk_io_channel * channel,
                       std::string object_name, uint64_t offset, char* buf, uint64_t len,
                       object_rw_complete cb_fn, void* arg, bool is_read)
{
  struct blob_rw_ctx* ctx;
  uint64_t start_lba, num_lba, pin_buf_length;
	uint32_t lba_size;

  ctx = new blob_rw_ctx;
  ctx->pin_buf = nullptr;
  ctx->is_read = is_read;

  ctx->object_name = std::move(object_name);
  ctx->offset = offset;
  ctx->buf = buf;
  ctx->len = len;
  ctx->submit_tick = spdk_get_ticks();
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;

  get_page_parameters(offset, len, &start_lba, &lba_size, &num_lba);
  pin_buf_length = num_lba * lba_size;

  if (is_lba_aligned(offset, len)) {
    SPDK_DEBUGLOG(object_store, "aligned offset:%lu len:%lu\n", offset, len);
    ctx->is_aligned = true;
    SPDK_DEBUGLOG(object_store,
      "object_store submit %s object %s offset %lu len %lu lba %lu num_lba %lu core %u\n",
      is_read ? "read" : "write",
      ctx->object_name.c_str(),
      offset,
      len,
      start_lba,
      num_lba,
      core_sharded::get_core_sharded().this_shard_id());
    if (!is_read)
    {
      spdk_blob_io_write(blob, channel, buf, start_lba, num_lba, rw_done, ctx);
    }
    else
    {
      spdk_blob_io_read(blob, channel, buf, start_lba, num_lba, rw_done, ctx);
    }
  } else {
    SPDK_DEBUGLOG(object_store, "not aligned offset:%lu len:%lu\n", offset, len);
    ctx->is_aligned = false;
    ctx->blob = blob;
    ctx->channel = channel;

    ctx->start_lba = start_lba;
    ctx->num_lba = num_lba;
    uint32_t lcore = spdk_env_get_current_core();
    uint32_t sockid = spdk_env_get_socket_id(lcore);
    ctx->pin_buf = (char *)spdk_malloc(pin_buf_length, lba_size, NULL,
                                       sockid, SPDK_MALLOC_DMA);
    ctx->blocklen = lba_size;
    SPDK_DEBUGLOG(object_store,
      "object_store submit staged %s object %s offset %lu len %lu lba %lu num_lba %lu core %u\n",
      is_read ? "read" : "write",
      ctx->object_name.c_str(),
      offset,
      len,
      start_lba,
      num_lba,
      core_sharded::get_core_sharded().this_shard_id());

    spdk_blob_io_read(blob, channel, ctx->pin_buf, start_lba, num_lba,
                      read_done, ctx);
  }
}

// 所有读写最终都会进入这个回调。
void object_store::rw_done(void *arg, int objerrno) {
  struct blob_rw_ctx* ctx = (struct blob_rw_ctx*)arg;
  auto dur_ticks = spdk_get_ticks() - ctx->submit_tick;
  auto dur_us = spdk_get_ticks_hz() == 0 ? 0 : (dur_ticks * SPDK_SEC_TO_USEC) / spdk_get_ticks_hz();

  if (objerrno) {
    if (ctx->is_read) {
		  SPDK_ERRLOG("read offset:%lu len:%lu failed:%s\n", ctx->offset, ctx->len, spdk_strerror(objerrno));
    } else {
      SPDK_ERRLOG("write offset:%lu len:%lu failed:%s\n", ctx->offset, ctx->len, spdk_strerror(objerrno));
    }
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
		return;
	}

  if (dur_us >= slow_io_warn_us) {
    SPDK_WARNLOG(
      "slow object_store %s object %s offset %lu len %lu dur %lluus core %u\n",
      ctx->is_read ? "read" : "write",
      ctx->object_name.c_str(),
      ctx->offset,
      ctx->len,
      dur_us,
      core_sharded::get_core_sharded().this_shard_id());
  } else {
    SPDK_DEBUGLOG(object_store,
      "object_store done %s object %s offset %lu len %lu dur %lluus core %u\n",
      ctx->is_read ? "read" : "write",
      ctx->object_name.c_str(),
      ctx->offset,
      ctx->len,
      dur_us,
      core_sharded::get_core_sharded().this_shard_id());
  }

  if (ctx->pin_buf) {
    SPDK_DEBUGLOG(object_store, "free pin_buf: %p\n", ctx->pin_buf);
    spdk_free(ctx->pin_buf);
  }
  //最后执行用户的回调
  ctx->cb_fn(ctx->arg, 0);
  delete ctx;
}

// 只有非对齐的读写，会经过这个回调。
// 预先读取对齐内容，再根据 is_read 决定，写回磁盘或者返回给用户。
void object_store::read_done(void *arg, int objerrno) {
  struct blob_rw_ctx* ctx = (struct blob_rw_ctx*)arg;
  char*  pin_buf;

  if (objerrno) {
    SPDK_ERRLOG("prior read offset:%lu len:%lu start_lba:%lu num_lba:%lu failed:%s\n",
        ctx->offset, ctx->len, ctx->start_lba, ctx->num_lba, spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
		return;
	}

  pin_buf = (char *)((uintptr_t)ctx->pin_buf + (ctx->offset & (ctx->blocklen - 1)));
  if (ctx->is_read) {
    memcpy(ctx->buf, pin_buf, ctx->len);
    rw_done(ctx, 0);
  } else {
    memcpy(pin_buf, ctx->buf, ctx->len);
    spdk_blob_io_write(ctx->blob, ctx->channel,
				   ctx->pin_buf, ctx->start_lba, ctx->num_lba,
				   rw_done, ctx);
  }
}

void object_store::create_done(void *arg, spdk_blob_id blobid, int objerrno) {
  struct blob_create_ctx* ctx = (struct blob_create_ctx*)arg;
  if (objerrno) {
    SPDK_ERRLOG("name:%s blobid:%" PRIu64 " create failed:%s\n",
        ctx->object_name.c_str(), blobid, spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
		return;
	}

  ctx->blobid = blobid;

  spdk_bs_open_blob(ctx->mgr->bs, blobid, open_done, ctx);
}

void object_store::open_done(void *arg, struct spdk_blob *blob, int objerrno) {
  struct blob_create_ctx* ctx = (struct blob_create_ctx*)arg;

  if (objerrno) {
    SPDK_ERRLOG("name:%s blobid:%" PRIu64 " open failed:%s\n",
        ctx->object_name.c_str(), ctx->blobid, spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
  	return;
  }
  SPDK_DEBUGLOG(object_store, "name:%s blobid:%" PRIu64 " opened in core %u\n",
      ctx->object_name.c_str(), ctx->blobid, ctx->shard_id);

  // 成功打开
  struct object_store::object obj;
  obj.origin.blobid = spdk_blob_get_id(blob);
  obj.origin.blob = blob;
  ctx->mgr->table.emplace(std::move(ctx->object_name), std::move(obj));

  blob_readwrite(blob, ctx->mgr->channel, ctx->object_name, ctx->offset, ctx->buf, ctx->len,
                 ctx->cb_fn, ctx->arg, ctx->is_read);
  delete ctx;
}

/**
 * stop()停止运行，close掉所有blob
 *
 * TODO(sunyifang):目前stop()会close所有对象origin blob，并delete所有snapshot。有待调整。
 */
void object_store::stop(object_rw_complete cb_fn, void* arg) {
  struct blob_stop_ctx* ctx = new blob_stop_ctx();

  if (table.empty()) {
    cb_fn(arg, 0);
    delete ctx;
    return;
  }

  ctx->table = std::exchange(table, {});
  ctx->it = ctx->table.begin();
  ctx->bs = bs;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;

  // SPDK_NOTICELOG("object_name:%s origin close.\n", ctx->it->first.c_str());
  spdk_blob_close(ctx->it->second.origin.blob, close_done, ctx);
}

void object_store::close_done(void *arg, int objerrno) {
  struct blob_stop_ctx* ctx = (struct blob_stop_ctx*)arg;

  if (objerrno) {
    SPDK_ERRLOG("object_name:%s delete failed:%s\n",
        ctx->it->first.c_str(), spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
		return;
	}

  auto& object = ctx->it->second;
  // if there is snapshot
  if (!object.snap_list.empty()) {
      auto del_snap = object.snap_list.front();
      // SPDK_NOTICELOG("object_name:%s snap_name:%s delete.\n", ctx->it->first.c_str(), del_snap.snap_name.c_str());
      object.snap_list.pop_front();
      spdk_bs_delete_blob(ctx->bs, del_snap.snap_blob.blobid, close_done, ctx);
      return;
  }

  ++ctx->it;
  // if all object deleted
  if (ctx->it == ctx->table.end()) {
    ctx->cb_fn(ctx->arg, 0);
    delete ctx;
		return;
  }

  // delete next object
  // SPDK_NOTICELOG("object_name:%s origin close.\n", ctx->it->first.c_str());
  spdk_blob_close(ctx->it->second.origin.blob, close_done, ctx);
}

struct blob_info {
  fb_blob     fblob;
  std::string object_name;
};

struct blob_delete_ctx {
  object_store::container table;
  std::list<blob_info>      blobs;
  spdk_blob_store*        bs;
  object_rw_complete      cb_fn;
  void*                   arg;
};

static void delete_blob_done(void *arg, int objerrno);

static void close_blob_done(void *arg, int objerrno){
  blob_delete_ctx* ctx = (blob_delete_ctx*)arg;
  auto fblob = ctx->blobs.front();

  if (objerrno) {
    SPDK_ERRLOG("object_name:%s close blob %lu failed:%s\n",
        fblob.object_name.c_str(), fblob.fblob.blobid, spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
		return;
	}

  SPDK_INFOLOG(object_store, "object_name:%s close blob %lu done\n",
          fblob.object_name.c_str(), fblob.fblob.blobid);
  spdk_bs_delete_blob(ctx->bs, fblob.fblob.blobid, delete_blob_done, ctx);
}

static void delete_blob_done(void *arg, int objerrno){
  blob_delete_ctx* ctx = (blob_delete_ctx*)arg;
  auto &fblob = ctx->blobs.front();
  if (objerrno) {
    SPDK_ERRLOG("object_name:%s delete blob %lu failed:%s\n",
        fblob.object_name.c_str(), fblob.fblob.blobid, spdk_strerror(objerrno));
    ctx->cb_fn(ctx->arg, objerrno);
    delete ctx;
		return;
	}

  SPDK_INFOLOG(object_store, "object_name:%s delete blob %lu done\n",
          fblob.object_name.c_str(), fblob.fblob.blobid);
  ctx->blobs.pop_front();

  if(ctx->blobs.empty()){
    SPDK_INFOLOG(object_store, "delete blobs done.\n");
    ctx->cb_fn(ctx->arg, 0);
    return;
  }
  auto &nblob = ctx->blobs.front();
  spdk_blob_close(nblob.fblob.blob, close_blob_done, ctx);
}

void object_store::destroy(object_rw_complete cb_fn, void* arg){
  if (table.empty()) {
    cb_fn(arg, 0);
    return;
  }
  struct blob_delete_ctx* ctx = new blob_delete_ctx();

  ctx->table = std::exchange(table, {});
  for (auto& pr : ctx->table){
    if(pr.second.origin.blob){
      blob_info iblob{.fblob = std::move(pr.second.origin), .object_name = pr.first};
      ctx->blobs.emplace_back(std::move(iblob));
    }
    if(pr.second.recover.blob){
      blob_info iblob{.fblob = std::move(pr.second.recover), .object_name = pr.first};
      ctx->blobs.emplace_back(std::move(iblob));
    }
    while(!pr.second.snap_list.empty()){
      auto snap = pr.second.snap_list.front();
      pr.second.snap_list.pop_front();
      blob_info iblob{.fblob = std::move(snap.snap_blob), .object_name = pr.first};
      ctx->blobs.emplace_back(std::move(iblob));
    }
  }
  ctx->bs = bs;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;

  auto &fblob = ctx->blobs.front();
  spdk_blob_close(fblob.fblob.blob, close_blob_done, ctx);
}
