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

#pragma once

#include "types.h"
#include "utils/units.h"

#include <spdk/blob.h>
#include <spdk/log.h>
#include <spdk/string.h>
#include <spdk/thread.h>
#include <functional>
#include <vector>

using pool_create_complete = std::function<void (void *arg, int objerrno)>;

class blob_pool;

struct pool_create_ctx {
  blob_pool* pool;

  pool_create_complete cb_fn;
  void* arg;

  blob_type type;
  uint64_t idx;
  uint64_t max;
  fb_blob blob;
};

struct pool_delete_ctx {
  blob_pool* pool;

  pool_create_complete cb_fn;
  void* arg;

  fb_blob blob;
};
/**
 * 加速blob的申请。
 */
class blob_pool {
  static constexpr uint32_t cluster_size = 1_MB;
  static constexpr uint32_t blob_size = cluster_size * 4;
  static constexpr uint32_t init_blob_num = 1_GB / blob_size; // 初始256个blob
  static constexpr uint32_t min_blob_num = 100;
  static constexpr uint64_t poller_period_us = 5000; // 每隔 5ms poll一次
public:
  void set_blobstore(struct spdk_blob_store* bs) { _bs = bs; }

  bool has_free_blob() { return _blobs.size(); }

  fb_blob get() {
      if (!_blobs.size()) {
        SPDK_ERRLOG("blob_pool: illegal call get. pool have no free blob!\n");
        return {};
      }
      
      fb_blob ret = _blobs.back();
      _blobs.pop_back();
    //   SPDK_NOTICELOG("blob_pool: have %lu blob!\n", _blobs.size());
      return ret;
  }

  void put(fb_blob blob) {
      _blobs.push_back(blob);
    //   SPDK_NOTICELOG("blob_pool: put blob. pool size:%lu\n", _blobs.size());
  }

  void start(pool_create_complete cb_fn, void* arg)  {
      _worker_poller = SPDK_POLLER_REGISTER(worker_poll, this, poller_period_us);
      allocate_blob(init_blob_num, std::move(cb_fn), arg);
  }

  void stop(pool_create_complete cb_fn, void* arg) {
      spdk_poller_unregister(&_worker_poller);
      deallocate_blob(std::move(cb_fn), arg);
  }

  size_t size() { return _blobs.size(); }

private:
  /**
   * deallocate和allocate的不同之处在于：allocate每次poller都会调用，而deallocate只有在pool停止时调用一次
   */
  void deallocate_blob(pool_create_complete cb_fn, void* arg) {
      if (size() == 0) {
          cb_fn(arg, 0);
          return;
      }

      pool_create_ctx* ctx = new pool_create_ctx;
      ctx->pool = this;
      ctx->cb_fn = std::move(cb_fn);
      ctx->arg = arg;
      ctx->blob = get();

      spdk_blob_close(ctx->blob.blob, blob_close_complete, ctx);
  }

  static void blob_close_complete(void *arg, int bperrno) {
      pool_create_ctx *ctx = (struct pool_create_ctx *)arg;

      if (bperrno) {
          SPDK_ERRLOG("blob_pool: close failed:%s\n", spdk_strerror(bperrno));
          ctx->cb_fn(ctx->arg, bperrno);
          delete ctx;
          return;
      }

      spdk_bs_delete_blob(ctx->pool->_bs, ctx->blob.blobid, blob_delete_complete, ctx);
  }

  static void blob_delete_complete(void *arg, int bperrno) {
      pool_create_ctx *ctx = (struct pool_create_ctx *)arg;

      if (bperrno) {
          SPDK_ERRLOG("blob_pool: delete failed:%s\n", spdk_strerror(bperrno));
          ctx->cb_fn(ctx->arg, bperrno);
          delete ctx;
          return;
      }

      if (ctx->pool->size()) {
        ctx->blob = ctx->pool->get();
        spdk_blob_close(ctx->blob.blob, blob_close_complete, ctx);
        return;
      }
      
      SPDK_NOTICELOG("blob_pool: delete all success!\n");
      ctx->cb_fn(ctx->arg, 0);
      delete ctx;
      return;
  }

private:
  /**
   * 空闲blob数量小于 min_blob_num 时，尝试申请blob
   */
  static int worker_poll(void *arg) {
      blob_pool* ctx = (blob_pool*)arg;
      return ctx->maybe_alloc_blob() ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
  }

  bool maybe_alloc_blob() {
      if (is_working()) { return false; }

      if (need_alloc_blob()) {
          SPDK_NOTICELOG("blob_pool: enter maybe_alloc_blob, pool_size:%u _blobs.size:%lu\n", min_blob_num, _blobs.size());
          uint64_t alloc_number = min_blob_num - _blobs.size() + 50;
          allocate_blob(alloc_number, [](void*, int){ }, nullptr);
          return true;
      }
      return false;
  }

  bool need_alloc_blob() { return _blobs.size() < min_blob_num; }

  void allocate_blob(uint64_t number, pool_create_complete cb_fn, void* arg) {
      if (is_working()) { 
          cb_fn(arg, -EBUSY);
          return; 
      }

      if (number == 0) {
          exit_working();
          cb_fn(arg, 0);
          return;
      }

      enter_working();
      pool_create_ctx* ctx = new pool_create_ctx;
    //   SPDK_NOTICELOG("blob_pool: allocate %lu blob!\n", number);

      ctx->pool = this;
      ctx->cb_fn = std::move(cb_fn);
      ctx->arg = arg;
      ctx->type = blob_type::free;
      ctx->idx = 0;
      ctx->max = number;

      create_blob(ctx);
  }

  void create_blob(pool_create_ctx* ctx)  {
      struct spdk_blob_opts opts;

      spdk_blob_opts_init(&opts, sizeof(opts));
      opts.num_clusters = 4;
      opts.thin_provision = false;
      opts.xattrs.count = 1;
#pragma GCC diagnostic push // TODO(sunyifang): 一个无聊的错误，暂时忽略
#pragma GCC diagnostic ignored "-Wwrite-strings"
      char *xattr_names[] = {"type"};
#pragma GCC diagnostic pop
      opts.xattrs.names = xattr_names;
      opts.xattrs.ctx = ctx;
      opts.xattrs.get_value = get_xattr_value;
      spdk_bs_create_blob_ext(_bs, &opts, create_blob_complete, ctx);
  }

  static void create_blob_complete(void *arg, spdk_blob_id blobid, int bperrno)  {
      pool_create_ctx *ctx = (struct pool_create_ctx *)arg;

      if (bperrno) {
          SPDK_ERRLOG("blob_pool: create failed:%s\n", spdk_strerror(bperrno));
          ctx->pool->exit_working();
          ctx->cb_fn(ctx->arg, 0);
          delete ctx;
          return;
      }

      ctx->blob.blobid = blobid;
      spdk_bs_open_blob(ctx->pool->_bs, blobid, open_blob_complete, ctx);
  }

  static void open_blob_complete(void *arg, struct spdk_blob *blob, int bperrno)  {
      pool_create_ctx *ctx = (struct pool_create_ctx *)arg;

      if (bperrno) {
          SPDK_ERRLOG("blob_pool: open failed:%s\n", spdk_strerror(bperrno));
          ctx->pool->exit_working();
          ctx->cb_fn(ctx->arg, 0);
          delete ctx;
          return;
      }

      ctx->blob.blob = blob;
      ctx->pool->_blobs.push_back(std::exchange(ctx->blob, {}));

      ctx->idx++;
      if (ctx->idx < ctx->max) {
          ctx->pool->create_blob(ctx);
          return;
      }
      
    //   SPDK_NOTICELOG("blob_pool: alloc all success. idx:%lu max:%lu pool size:%lu\n", 
    //                   ctx->idx, ctx->max, ctx->pool->_blobs.size());
      ctx->pool->exit_working();
      ctx->cb_fn(ctx->arg, 0);
      delete ctx;
      return;
  }

  static void get_xattr_value(void *arg, const char *name, const void **value, size_t *value_len) {
      struct pool_create_ctx* ctx = (struct pool_create_ctx*)arg;

      if (!strcmp("type", name)) {
          *value = &ctx->type;
          *value_len = sizeof(ctx->type);
          return;
      }
      *value = NULL;
      *value_len = 0;
  }


private:
  bool is_working() { return _working; }
  void enter_working() { _working = true; }
  void exit_working() { _working = false; }

private:
  struct spdk_blob_store* _bs;
  std::vector<fb_blob>    _blobs;

  struct spdk_poller *_worker_poller; 
  bool                _working = false;
};