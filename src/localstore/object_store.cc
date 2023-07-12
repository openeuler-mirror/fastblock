/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2. 
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2 
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, 
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, 
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.  
 * See the Mulan PSL v2 for more details.  
 */

#include <spdk/log.h>

#include "object_store.h"

struct blob_rw_ctx {
  bool is_read;
  struct spdk_blob *blob;
  struct spdk_io_channel *channel;

  // 读写参数列表
  std::string object_name;
  uint64_t offset;
  char*    buf;
  uint64_t len;
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
  bool is_read;
  struct object_store *mgr;

  // 读写参数列表
  std::string object_name;
  uint64_t offset;
  char*    buf;
  uint64_t len;
  object_rw_complete cb_fn;
  void*    arg;

  // create之后，然后调用open时使用
  spdk_blob_id blobid;
};

struct blob_close_ctx {
  struct object_store *mgr;
  // 用来保存 close 到哪一个元素了
  object_store::iterator it;

  object_rw_complete cb_fn;
  void*    arg;
};

void object_store::write(std::string object_name, 
                      uint64_t offset, char* buf, uint64_t len, 
                      object_rw_complete cb_fn, void* arg)
{
    readwrite(object_name, offset, buf, len, cb_fn, arg, 0);
}

void object_store::read(std::string object_name, 
                     uint64_t offset, char* buf, uint64_t len, 
                     object_rw_complete cb_fn, void* arg) 
{
    readwrite(object_name, offset, buf, len, cb_fn, arg, 1);
}

void object_store::readwrite(std::string object_name, 
                     uint64_t offset, char* buf, uint64_t len, 
                     object_rw_complete cb_fn, void* arg, bool is_read) 
{
    SPDK_DEBUGLOG("object %s offset:%lu len:%lu\n", object_name.c_str(), offset, len);
    if (offset + len >= blob_size) {
      SPDK_WARNLOG("object %s offset:%lu len:%lu beyond blob size %u\n",
          object_name.c_str(), offset, len, blob_size);
      len = blob_size - offset;
    }

    auto it = table.find(object_name);
    if (it != table.end()) {
        SPDK_DEBUGLOG("object %s found, blob id:%" PRIu64 "\n", object_name.c_str(), it->second->blobid);
        blob_readwrite(it->second->blob, channel, offset, buf, len, cb_fn, arg, is_read);
    } else {
        SPDK_DEBUGLOG("object %s not found\n", object_name.c_str());
        // 没找到，就先创建blob对象，之后再调用write_blob
        struct blob_create_ctx* ctx = new blob_create_ctx();
        struct spdk_blob_opts opts;

        ctx->is_read = is_read;
        ctx->mgr = this;
        ctx->object_name = object_name;
        ctx->offset = offset;
        ctx->buf = buf;
        ctx->len = len;
        ctx->cb_fn = cb_fn;
        ctx->arg = arg;

        spdk_blob_opts_init(&opts, sizeof(opts));
        opts.num_clusters = object_store::blob_cluster;
        spdk_bs_create_blob_ext(bs, &opts, create_done, ctx);
    }
}

/**
 * 下面都是static函数，因为c++的成员函数隐含的第一个参数是this指针，
 * 要传递给c语言的函数指针，需要写为static成员函数，或者非成员函数
 */
void object_store::blob_readwrite(struct spdk_blob *blob, struct spdk_io_channel * channel, 
                       uint64_t offset, char* buf, uint64_t len, 
                       object_rw_complete cb_fn, void* arg, bool is_read) 
{
  struct blob_rw_ctx* ctx;
  uint64_t start_lba, num_lba, pin_buf_length;
	uint32_t lba_size;

  ctx = new blob_rw_ctx;
  ctx->pin_buf = nullptr;
  ctx->is_read = is_read;

  ctx->offset = offset;
  ctx->buf = buf;
  ctx->len = len;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;

  get_page_parameters(offset, len, &start_lba, &lba_size, &num_lba);
  pin_buf_length = num_lba * lba_size;

  if (is_lba_aligned(offset, len)) {
      SPDK_DEBUGLOG("aligned offset:%lu len:%lu\n", offset, len);
      ctx->is_aligned = true;
      if (is_read) {
        spdk_blob_io_write(blob, channel, buf, start_lba, num_lba, rw_done, ctx);
      } else {
        spdk_blob_io_read(blob, channel, buf, start_lba, num_lba, rw_done, ctx);
      }
  } else {
      SPDK_DEBUGLOG("not aligned offset:%lu len:%lu\n", offset, len);
      ctx->is_aligned = false;
      ctx->blob = blob;
      ctx->channel = channel;

      ctx->start_lba = start_lba;
      ctx->num_lba = num_lba;
      ctx->pin_buf = (char*)spdk_malloc(pin_buf_length, lba_size, NULL,
                                        SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
      ctx->blocklen = lba_size;
      
      spdk_blob_io_read(blob, channel, ctx->pin_buf, start_lba, num_lba,
          read_done, ctx);
  }
}

// 所有读写最终都会进入这个回调。
void object_store::rw_done(void *arg, int objerrno) {
  struct blob_rw_ctx* ctx = (struct blob_rw_ctx*)arg;

  if (objerrno) {
    if (ctx->is_read) {
		  SPDK_ERRLOG("read offset:%lu len:%lu failed:%s\n", ctx->offset, ctx->len, spdk_strerror(objerrno));
    } else {
      SPDK_ERRLOG("write offset:%lu len:%lu failed:%s\n", ctx->offset, ctx->len, spdk_strerror(objerrno));
    }
		return;
	}

  // object层的处理代码，可以写在这里 

  if (ctx->pin_buf) {
    SPDK_DEBUGLOG("free pin_buf: %p\n", ctx->pin_buf);
    spdk_free(ctx->pin_buf);
  }
  //最后执行用户的回调
  ctx->cb_fn(ctx->arg, objerrno);
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
		return;
	}
  SPDK_DEBUGLOG("name:%s blobid:%" PRIu64 " opened\n", ctx->object_name.c_str(), ctx->blobid);

  // 成功打开
  struct object_store::fb_blob* fblob = new fb_blob;
  fblob->blobid = ctx->blobid;
  fblob->blob = blob;
  // 此处打开 blob 后要对 object_store 中的 table 进行修改，
  // 所以 blob_create_ctx 需要保存 object_store 指针
  ctx->mgr->table.emplace(std::move(ctx->object_name), fblob);

  blob_readwrite(blob, ctx->mgr->channel, ctx->offset, ctx->buf, ctx->len, 
                 ctx->cb_fn, ctx->arg, ctx->is_read);
  delete ctx;
}

/**
 * stop()停止运行，close掉所有blob
 */
void object_store::stop(object_rw_complete cb_fn, void* arg) {
  struct blob_close_ctx* ctx = new blob_close_ctx();

  if (table.empty()) {
    cb_fn(arg, 0);
    delete ctx;
    return;
  }

  auto it = table.begin();
  ctx->it  = it;
  ctx->mgr = this;
  ctx->cb_fn = cb_fn;
  ctx->arg = arg;

  spdk_blob_close(it->second->blob, close_done, ctx);
}

void object_store::close_done(void *arg, int objerrno) {
  struct blob_close_ctx* ctx = (struct blob_close_ctx*)arg;

  if (objerrno) {
    SPDK_ERRLOG("blobid:%" PRIu64 " close failed:%s\n", 
        ctx->it->second->blobid, spdk_strerror(objerrno));
		return;
	}

  SPDK_DEBUGLOG("close %u blobid:" PRIu64 " closed\n", ctx->count, ctx->it->second->blobid);
  ctx->it++;
  auto& table = ctx->mgr->table;
  if (ctx->it == table.end()) {
    SPDK_DEBUGLOG("close %u blobids finish\n", table.size());
    ctx->cb_fn(ctx->arg, 0);
    delete ctx;
    return;
  }

  spdk_blob_close(ctx->it->second->blob, close_done, ctx);
}