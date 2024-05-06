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
#include "spdk_buffer.h"
#include "blob_manager.h"
#include "utils/err_num.h"
#include "utils/log.h"

#include <spdk/log.h>
#include <spdk/blob.h>
#include <spdk/string.h>
#include <functional>
#include <errno.h>
#include <utility>

using checkpoint_op_complete = std::function<void (void *, int)>;

class kv_checkpoint;

struct checkpoint_ctx {
  kv_checkpoint* kv_ckpt;
  struct spdk_blob_store *bs;
  fb_blob blob;
  blob_type type;
  uint32_t shard_id;

  iovecs iovs;

  checkpoint_op_complete cb_fn;
  void* arg;
};

/**
 * 保存全量的kv数据，永远只保留一个最新的checkpoint，最新的写完就删除老的。
 *
 * 创建新checkpoint的流程是：
 *     start_checkpoint() -> write_checkpoint() -> finish_checkpoint()
 *
 * 从磁盘恢复checkpoint的流程是：
 *     open_checkpoint() -> read_checkpoint()
 *
 * 进程退出时调用stop()
 */
class kv_checkpoint {
public:
  kv_checkpoint() : _bs(global_blobstore()), _channel(global_io_channel()) {}

public:
  static void
  kv_get_xattr_value(void *arg, const char *name, const void **value, size_t *value_len){
    struct checkpoint_ctx* ctx = (struct checkpoint_ctx*)arg;
  
    if(!strcmp("type", name)){
  		*value = &(ctx->type);
  		*value_len = sizeof(ctx->type);    
        return; 
  	} else if(!strcmp("shard", name)){
  		*value = &(ctx->shard_id);
  		*value_len = sizeof(ctx->shard_id); 
      return;   
    }
  	*value = NULL;
  	*value_len = 0;    
  }

  void set_checkpoint_blobid(spdk_blob_id checkpoint_blob_id, spdk_blob_id new_checkpoint_blob_id){
    if(checkpoint_blob_id){
      _ckpt_blob.blobid = checkpoint_blob_id;
    }
    if(new_checkpoint_blob_id){
      _new_blob.blobid = new_checkpoint_blob_id;
    }
  }

  /**
   * 开始checkpoint。创建一个新的blob，放在_new_blob中，等待写入。
   */
  void start_checkpoint(size_t size, checkpoint_op_complete cb_fn, void* arg);

  static void new_blob_create_complete(void *arg, spdk_blob_id blobid, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint new blob_id:0x%lx create failed:%s\n", blobid, spdk_strerror(rberrno));
          ctx->cb_fn(ctx->arg, rberrno);
          delete ctx;
          return;
      }
      SPDK_WARNLOG_EX("checkpoint blob create complete. blob id:%p blob:%p\n", (void *)blobid, ctx->blob.blob);
      ctx->blob.blobid = blobid;
      spdk_bs_open_blob(ctx->bs, blobid, new_blob_open_complete, ctx);
  }

  static void new_blob_open_complete(void *arg, struct spdk_blob *blob, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint new blob_id:0x%lx open failed:%s\n", ctx->blob.blobid, spdk_strerror(rberrno));
          ctx->cb_fn(ctx->arg, rberrno);
          delete ctx;
          return;
      }

      SPDK_WARNLOG_EX("checkpoint blob open complete. blob id:%p blob:%p num_cluster:%lu size:%lu\n",
                         (void *)ctx->blob.blobid, blob, spdk_blob_get_num_clusters(blob),
                         spdk_blob_get_num_clusters(blob) * spdk_bs_get_cluster_size(ctx->bs));
      ctx->blob.blob = blob;
      ctx->kv_ckpt->_new_blob = ctx->blob;
      ctx->cb_fn(ctx->arg, 0);
      delete ctx;
  }

public:
  /**
   * 写checkpoint。
   */
  void write_checkpoint(spdk_buffer& sbuf, checkpoint_op_complete cb_fn, void* arg) {
    write_checkpoint(sbuf.get_buf(), sbuf.size(), std::move(cb_fn), arg);
  }

  void write_checkpoint(buffer_list& bl, checkpoint_op_complete cb_fn, void* arg) {
    write_checkpoint(bl.to_iovec(), bl.bytes(), std::move(cb_fn), arg);
  }

  void write_checkpoint(char* buf, uint64_t len, checkpoint_op_complete cb_fn, void* arg) {
      if (_new_blob.blobid == 0) {
          SPDK_ERRLOG_EX("kv checkpoint write null blobid, call start_checkpoint first.\n");
          cb_fn(arg, -EINVAL);
          return;
      }

      struct checkpoint_ctx* ctx = new checkpoint_ctx();
      ctx->kv_ckpt = this;
      ctx->blob = _new_blob;
      ctx->cb_fn = std::move(cb_fn);
      ctx->arg = arg;
      SPDK_DEBUGLOG_EX(kvlog, "checkpoint write len:%lu lba_len:%lu\n",
                          len, len / spdk_bs_get_io_unit_size(_bs));
      spdk_blob_io_write(_new_blob.blob, _channel, buf,
                         0, len / spdk_bs_get_io_unit_size(_bs),
                         new_blob_write_complete, ctx);
  }

  void write_checkpoint(std::vector<iovec> &&iov, uint64_t len, checkpoint_op_complete cb_fn, void *arg)
  {
    if (_new_blob.blobid == 0)
    {
      SPDK_ERRLOG_EX("checkpoint write null blobid, call start_checkpoint first.\n");
      cb_fn(arg, -EINVAL);
      return;
    }

    struct checkpoint_ctx *ctx = new checkpoint_ctx();
    ctx->kv_ckpt = this;
    ctx->blob = _new_blob;
    ctx->iovs = std::move(iov);
    ctx->cb_fn = std::move(cb_fn);
    ctx->arg = arg;

    SPDK_DEBUGLOG_EX(kvlog, "checkpoint write iovs size:%lu len:%lu lba_len:%lu\n",
                        ctx->iovs.size(), len, len / spdk_bs_get_io_unit_size(_bs));
    spdk_blob_io_writev(_new_blob.blob, _channel, ctx->iovs.data(),
                        ctx->iovs.size(), 0, len / spdk_bs_get_io_unit_size(_bs),
                        new_blob_write_complete, ctx);
  }

  // 写完后先不关闭，在finish_checkpoint中关闭（因为finish_checkpoint需要修改xattr）
  static void new_blob_write_complete(void *arg, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint new blob_id:0x%lx delete failed:%s\n", ctx->blob.blobid, spdk_strerror(rberrno));
      }

      ctx->cb_fn(ctx->arg, rberrno);
      delete ctx;
  }

  static void blob_close_complete(void *arg, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint new blob_id:0x%lx close failed:%s\n", ctx->blob.blobid, spdk_strerror(rberrno));
      }

      ctx->kv_ckpt->_ckpt_blob.blob = nullptr;
      ctx->cb_fn(ctx->arg, rberrno);
      delete ctx;
  }

public:
  /**
   * 结束这次checkpoint流程。删除掉老的blob。
   */
  void finish_checkpoint(checkpoint_op_complete cb_fn, void* arg) {
      if (_new_blob.blobid == 0) {
          SPDK_ERRLOG_EX("kv checkpoint finish on null blobid.\n");
          cb_fn(arg, -EINVAL);
          return;
      }

      struct checkpoint_ctx* ctx = new checkpoint_ctx();
      ctx->kv_ckpt = this;
      ctx->bs = _bs;
      ctx->blob = _ckpt_blob;
      ctx->cb_fn = std::move(cb_fn);
      ctx->arg = arg;

      std::map<std::string, xattr_val_type> xattr;
      xattr["type"] = blob_type::kv_checkpoint;
      if (_ckpt_blob.blobid == 0) {
          // SPDK_NOTICELOG_EX("kv checkpoint first swap, old:0x%lx new:0x%lx.\n", _ckpt_blob.blobid, _new_blob.blobid);
          _ckpt_blob = std::exchange(_new_blob, {});
          // SPDK_NOTICELOG_EX("kv checkpoint first swap, old:0x%lx new:0x%lx.\n", _ckpt_blob.blobid, _new_blob.blobid);

          ::set_blob_xattr(
            _ckpt_blob.blob, 
            xattr,
            [cb_fn = std::move(cb_fn)](void *arg, int rberrno){
              struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;
              if(rberrno){
                SPDK_ERRLOG_EX("finish_checkpoint failed: %s\n", spdk_strerror(rberrno));
              }
              SPDK_WARNLOG_EX("set_blob_xattr blob:%s to %s\n",
                                 type_string(blob_type::kv_checkpoint_new).c_str(), type_string(blob_type::kv_checkpoint).c_str());
              spdk_blob_close(ctx->kv_ckpt->_ckpt_blob.blob, blob_close_complete, ctx);
            },
            ctx);
          return;
      }

      // SPDK_NOTICELOG_EX("kv checkpoint swap, old:0x%lx new:0x%lx.\n", _ckpt_blob.blobid, _new_blob.blobid);
      _ckpt_blob = std::exchange(_new_blob, {});
      // SPDK_NOTICELOG_EX("kv checkpoint swap, old:0x%lx new:0x%lx.\n", _ckpt_blob.blobid, _new_blob.blobid);
      ::set_blob_xattr(
        _ckpt_blob.blob, 
        xattr,
        [this](void *arg, int rberrno){
          struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;
          if(rberrno){
            SPDK_ERRLOG_EX("finish_checkpoint failed: %s\n", spdk_strerror(rberrno));
            ctx->cb_fn(ctx->arg, rberrno);
            return;
          }
          spdk_blob_close(ctx->kv_ckpt->_ckpt_blob.blob, new_blob_close_complete, ctx);
        },
        ctx);
  }

  static void new_blob_close_complete(void *arg, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint new blob_id:0x%lx close failed:%s\n", ctx->blob.blobid, spdk_strerror(rberrno));
      }

      ctx->kv_ckpt->_ckpt_blob.blob = nullptr;
      spdk_bs_delete_blob(ctx->bs, ctx->blob.blobid, blob_delete_complete, ctx);
  }

  static void blob_delete_complete(void *arg, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint old blob_id:0x%lx delete failed:%s\n", ctx->blob.blobid, spdk_strerror(rberrno));
      }
      
      ctx->cb_fn(ctx->arg, rberrno);
      delete ctx;
  }

public:
  /**
   * 打开_ckpt_blob，准备读取。只有从磁盘恢复kvstore的时候才会读取checkpoint。
   */
  void open_checkpoint(checkpoint_op_complete cb_fn, void* arg) {
      if (_ckpt_blob.blobid == 0) {
        SPDK_DEBUGLOG_EX(kvlog, "open_checkpoint, _ckpt_blob.blobid %lu invalid.\n", _ckpt_blob.blobid);
        cb_fn(arg, err::E_NODEV);
        return;
      }

      if (_ckpt_blob.blob != nullptr) {
          SPDK_ERRLOG_EX("open_checkpoint, _ckpt_blob.blob is not null.\n");
          cb_fn(arg, 0);
          return;
      }

      struct checkpoint_ctx* ctx = new checkpoint_ctx();
      ctx->blob = _ckpt_blob;
      ctx->kv_ckpt = this;
      ctx->cb_fn = std::move(cb_fn);
      ctx->arg = arg;
      SPDK_DEBUGLOG_EX(kvlog, "open_checkpoint, _ckpt_blob.blob:%p\n", (void *)_ckpt_blob.blobid);
      spdk_bs_open_blob(_bs, _ckpt_blob.blobid, ckpt_blob_open_complete, ctx);
  }

  static void ckpt_blob_open_complete(void *arg, struct spdk_blob *blob, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint old blob_id:0x%lx open failed:%s\n", ctx->blob.blobid, spdk_strerror(rberrno));
      }

      ctx->kv_ckpt->_ckpt_blob.blob = blob;
      ctx->cb_fn(ctx->arg, rberrno);
      delete ctx;
  }

  /**
   * 读取_ckpt_blob。
   */
  void read_checkpoint(spdk_buffer& sbuf, checkpoint_op_complete cb_fn, void* arg) {
    read_checkpoint(sbuf.get_buf(), sbuf.size(), std::move(cb_fn), arg);
  }

  void read_checkpoint(buffer_list& bl, checkpoint_op_complete cb_fn, void* arg) {
    read_checkpoint(bl.to_iovec(), bl.bytes(), std::move(cb_fn), arg);
  }

  void read_checkpoint(char* buf, uint64_t len, checkpoint_op_complete cb_fn, void* arg) {
      if (_ckpt_blob.blobid == 0) {
          SPDK_ERRLOG_EX("kv checkpoint read null blobid, save checkpoint first.\n");
          cb_fn(arg, -EINVAL);
          return;
      }

      struct checkpoint_ctx* ctx = new checkpoint_ctx();
      ctx->blob = _ckpt_blob;
      ctx->cb_fn = std::move(cb_fn);
      ctx->arg = arg;
      SPDK_DEBUGLOG_EX(kvlog, "checkpoint read blob:0x%lx len:%lu lba_len:%lu\n",
                          _ckpt_blob.blobid, len, len / spdk_bs_get_io_unit_size(_bs));
      spdk_blob_io_read(_ckpt_blob.blob, _channel, buf,
                        0, len / spdk_bs_get_io_unit_size(_bs),
                        ckpt_blob_read_complete, ctx);
  }

  void read_checkpoint(std::vector<iovec> &&iov, uint64_t len, checkpoint_op_complete cb_fn, void *arg)
  {
    if (_ckpt_blob.blobid == 0)
    {
      SPDK_ERRLOG_EX("kv checkpoint read null blobid, save checkpoint first.\n");
      cb_fn(arg, -EINVAL);
      return;
    }

    struct checkpoint_ctx *ctx = new checkpoint_ctx();
    ctx->blob = _ckpt_blob;
    ctx->iovs = std::move(iov);
    ctx->cb_fn = std::move(cb_fn);
    ctx->arg = arg;
    SPDK_DEBUGLOG_EX(kvlog, "checkpoint read blob:0x%lx iovs size:%lu len:%lu lba_len:%lu\n",
                        _ckpt_blob.blobid, ctx->iovs.size(), len, len / spdk_bs_get_io_unit_size(_bs));
    spdk_blob_io_readv(_ckpt_blob.blob, _channel, ctx->iovs.data(),
                       ctx->iovs.size(), 0, len / spdk_bs_get_io_unit_size(_bs),
                       ckpt_blob_read_complete, ctx);
  }

  // 读完直接顺便关闭
  static void ckpt_blob_read_complete(void *arg, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint old blob_id:0x%lx read failed:%s\n", ctx->blob.blobid, spdk_strerror(rberrno));
      }

      spdk_blob_close(ctx->blob.blob, ckpt_blob_close_complete, ctx);
  }

  static void ckpt_blob_close_complete(void *arg, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint old blob_id:0x%lx close failed:%s\n", ctx->blob.blobid, spdk_strerror(rberrno));
      }

      ctx->cb_fn(ctx->arg, rberrno);
      delete ctx;
  }

  // 只有_ckpt_blob打开的情况下才能调用
  size_t checkpoint_size() {
      if (_ckpt_blob.blobid == 0 || _ckpt_blob.blob == nullptr) {
          return 0;
      }
      return spdk_blob_get_num_clusters(_ckpt_blob.blob) * spdk_bs_get_cluster_size(_bs);
  }

public:
  /**
   * 关闭 _ckpt_blob.
   */
  void stop(checkpoint_op_complete cb_fn, void* arg) {
      if (_new_blob.blobid != 0) {
          SPDK_ERRLOG_EX("kv_checkpoint stop while saving checkpoint, new blob id:%p.\n", (void*)_new_blob.blobid);
      }

      // no checkpoint now
      if (_ckpt_blob.blobid == 0) {
          cb_fn(arg, 0);
          return;
      }

      struct checkpoint_ctx* ctx = new checkpoint_ctx();
      ctx->bs = _bs;
      ctx->blob = _ckpt_blob;
      ctx->cb_fn = std::move(cb_fn);
      ctx->arg = arg;

      spdk_bs_delete_blob(ctx->bs, ctx->blob.blobid, blob_delete_complete, ctx);
  }

  static void delete_new_blob_complete(void *arg, int rberrno) {
      struct checkpoint_ctx *ctx = (struct checkpoint_ctx *)arg;

      if (rberrno) {
          SPDK_ERRLOG_EX("checkpoint old blob_id:0x%lx delete failed:%s\n", ctx->blob.blobid, spdk_strerror(rberrno));
      }else{
          kv_checkpoint* kv_ckpt = ctx->kv_ckpt;
          
      }

      ctx->cb_fn(ctx->arg, rberrno);
      delete ctx;
  }

  void reset_new_blob(){
    _new_blob.blobid = 0;
    _new_blob.blob = nullptr; 
  }

  void delete_new_blob(checkpoint_op_complete cb_fn, void* arg){
    if(_new_blob.blobid == 0){
      cb_fn(arg, 0);
      return;
    }

    struct checkpoint_ctx* ctx = new checkpoint_ctx();
    ctx->kv_ckpt = this;
    ctx->cb_fn = std::move(cb_fn);
    ctx->arg = arg;    
    spdk_bs_delete_blob(_bs, _new_blob.blobid, delete_new_blob_complete, ctx);
  }
private:
    struct spdk_blob_store *_bs;
    struct spdk_io_channel *_channel;
    fb_blob _ckpt_blob; // 真正的checkpoint所在的blob
    fb_blob _new_blob;  // 新申请的，准备写的blob
};