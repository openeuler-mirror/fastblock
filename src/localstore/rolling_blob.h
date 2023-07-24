#pragma once

#include "buffer_pool.h"
#include "spdk_buffer.h"
#include "utils/units.h"
#include "utils/varint.h"

#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/string.h>

#include <functional>
#include <map>
#include <memory>
#include <errno.h>

class rolling_blob;

struct rblob_rw_result {
    uint64_t start_pos;
    uint64_t len;
};

using rblob_rw_complete = std::function<void (void *, rblob_rw_result, int)>;
using rblob_op_complete = std::function<void (void *, int)>;

struct rblob_rw_ctx {
  bool is_read;
  struct spdk_blob *blob;
  struct spdk_io_channel *channel;

  iovecs iov; // 持有iovec数组本身的内存
  uint64_t start_pos;
  uint64_t lba;
  uint64_t len;

  rblob_rw_complete cb_fn;
  void* arg;

  struct rblob_rw_ctx* next;
  rolling_blob* rb;
};

struct rblob_sync_ctx {
  rblob_op_complete cb_fn;
  void* arg;
};

struct rblob_close_ctx {
  rblob_op_complete cb_fn;
  void* arg;
};

/** 
 *  Case 1(not rolled):  
 *   从左往右写，随着数据的增加，front往右推进，但此时还没写到blob最右侧。
 *   最前面4k是super block，保存一些基本元数据。
 *    __________________________________________________
 *    |begin       back|                  front|    end| 
 *                     |<-------- used ------->| 
 *                                    
 * 
 *  Case 2(rolled):  
 *   数据向右写到了 blob 结尾处，要从 blob 的起始位置重新开始写。
 *   但front数字不会变小，它只会单调增加。对blob size取余，即可知道它在blob中的具体位置。
 *   后边的虚线为了方便理解，假设把blob拼接在右边。
 *    __________________________________________________ _ _ _ _ _ _ _ _ _ _ _ _
 *    |begin                         back|          end|        front|      
 *                                       |<-- used1 -->|<-- used2 -->|
*/
class rolling_blob {
public:
    static constexpr uint64_t huge_blob_size = 1_GB;    // log 默认使用1G的blob，滚动写
    static constexpr uint64_t small_blob_size = 32_MB;    // kv 默认使用32M的blob，滚动写
    static constexpr uint64_t super_size = 4_KB;  // 最前面4K的super block保存这个blob的元数据
    static constexpr uint64_t unit_size = 512;

    rolling_blob(struct spdk_blob* b, struct spdk_io_channel *ch, uint64_t blob_size) 
    : blob(b)
    , channel(ch)
    , blob_size(blob_size)
    , front(0, super_size)
    , back(0, super_size)
    , super(buffer_pool_get())
    { }

    ~rolling_blob() {
      buffer_pool_put(super);
    }

    void make_test() {
        front.lba = blob_size - 316_KB;
        back.lba = front.lba - 512_MB;
    }

    void append(buffer_list bl, rblob_rw_complete cb_fn, void* arg) {
        struct rblob_rw_ctx* ctx;
        uint64_t length = bl.bytes();

        if (length > available()) {
            SPDK_ERRLOG("append no space. length:%lu available:%lu\n", length, available());
            cb_fn(arg, {front.pos, length}, -ENOSPC);
            return;
        }

        ctx = new rblob_rw_ctx();
        ctx->is_read = false;
        ctx->blob = blob;
        ctx->channel = channel;
        ctx->cb_fn = cb_fn; 
        ctx->arg = arg;
        ctx->next = nullptr;
        ctx->rb = this;

        // 如果右侧空间足够，就直接写，不需要分割
        if (is_rolled() || length < front_to_end()) {
            // SPDK_NOTICELOG("append from:%lu len:%lu\n", front.lba, length);
            uint64_t start_pos = front.pos;

            ctx->iov = std::move(bl.to_iovec());
            ctx->start_pos = start_pos;
            ctx->lba = pos_to_lba(start_pos);
            ctx->len = length;

            inflight_rw.emplace(start_pos + length, false);
            spdk_blob_io_writev(blob, channel, ctx->iov.data(), ctx->iov.size(), 
                                ctx->lba / unit_size, ctx->len / unit_size, rw_done, ctx);
            return;
        }

        // 如果右侧空间不够，就分成两次写。第一次写到end，第二次从4k开始继续往后写。
        struct rblob_rw_ctx* next = new rblob_rw_ctx();
        uint64_t first_len = front_to_end();
        uint64_t second_len = length - first_len;

        ctx->iov = std::move(bl.to_iovec(0, first_len));
        ctx->start_pos = front.pos; // 从front开始写
        ctx->lba = pos_to_lba(ctx->start_pos);
        ctx->len = first_len;
        ctx->next = next;

        next->is_read = false;
        next->blob = blob;
        next->channel = channel;
        next->cb_fn = cb_fn; 
        next->arg = arg;
        next->next = nullptr;
        next->rb = this;

        next->iov = std::move(bl.to_iovec(first_len, second_len));
        next->start_pos = front.pos + first_len;    // 从第一次写结束位置开始写
        next->lba = pos_to_lba(next->start_pos);
        next->len = second_len;

        // SPDK_NOTICELOG("append from:%lu len:%lu and from:%lu len:%lu\n", 
        //                 ctx->lba, first_len, next->lba, second_len);
        inflight_rw.emplace(next->start_pos + second_len, false); // 只等待第二次写的偏移
        spdk_blob_io_writev(blob, channel, ctx->iov.data(), ctx->iov.size(), 
                                ctx->lba / unit_size, ctx->len / unit_size, rw_done, ctx);
    }

    void read(uint64_t start, uint64_t length, buffer_list bl, 
            rblob_rw_complete cb_fn, void* arg) {
        struct rblob_rw_ctx* ctx;

        if (start < back.pos) {
            SPDK_ERRLOG("read invalid pos. pos:%lu < back_pos:%lu\n", start, back.pos);
            cb_fn(arg, {start, length}, -EINVAL);
            return;
        }

        if (length > used()) {
            SPDK_ERRLOG("read over length. length:%lu > used:%lu\n", length, used());
            cb_fn(arg, {start, length}, -ENOSPC);
            return;
        }

        ctx = new rblob_rw_ctx();
        ctx->is_read = true;
        ctx->blob = blob;
        ctx->channel = channel;
        ctx->cb_fn = cb_fn; 
        ctx->arg = arg;
        ctx->next = nullptr;
        ctx->rb = this;

        // 如果这次读需要 roll，那就分成两次读
        if (need_roll(start, length)) {
            struct rblob_rw_ctx* next = new rblob_rw_ctx();
            uint64_t first_len = pos_to_end(start);
            uint64_t second_len = length - first_len;

            ctx->iov = std::move(bl.to_iovec(0, first_len)); 
            ctx->start_pos = start;
            ctx->lba = pos_to_lba(ctx->start_pos);
            ctx->len = first_len;
            ctx->next = next;

            next->is_read = true;
            next->blob = blob;
            next->channel = channel;
            next->cb_fn = cb_fn; 
            next->arg = arg;
            next->next = nullptr;
            next->rb = this;

            next->iov = std::move(bl.to_iovec(first_len, second_len));
            next->start_pos = start + first_len;    // 从第一次写结束位置开始写
            next->lba = pos_to_lba(next->start_pos);
            next->len = second_len;

            // SPDK_NOTICELOG("read lba from:%lu len:%lu and from:%lu len:%lu\n", 
            //         ctx->lba, first_len, next->lba, second_len);
            spdk_blob_io_readv(blob, channel, ctx->iov.data(), ctx->iov.size(), 
                            ctx->lba / unit_size, ctx->len / unit_size, rw_done, ctx);
            return;
        }

        // 除了需要分两次读写的情况，到这里就是可以一次性读取
        //虽然iovec数组本身在ctx结束后就析构了，但是外部传进来的bl，其每片地址指向的内存是一直存在的
        ctx->iov = std::move(bl.to_iovec()); 
        ctx->start_pos = start;
        ctx->lba = pos_to_lba(start);
        ctx->len = length;

        // SPDK_NOTICELOG("read lba from:%lu len:%lu\n", ctx->lba, length);
        spdk_blob_io_readv(blob, channel, ctx->iov.data(), ctx->iov.size(), 
                            ctx->lba / unit_size, ctx->len / unit_size, rw_done, ctx);
        return;
    }

    // TODO(sunyifang): 暂时先不实现iomap，以后要改成调用 spdk_blob_io_unmap 的实现
    void trim_back(uint64_t length, rblob_op_complete cb_fn, void* arg) {
        if (length > used()) {
            SPDK_ERRLOG("trim back overflow. length:%lu used:%lu\n", length, used());
            cb_fn(arg, -ENOSPC);
            return;
        }
        
        // 如果右侧有足够的空间，直接trim
        if (!is_rolled() || length < back_to_end()) {
            SPDK_NOTICELOG("trim back case 1. pos from  %lu to %lu, lba from  %lu to %lu\n", 
                back.pos, back.pos + length,
                pos_to_lba(back.pos), pos_to_lba(back.pos + length));
            back.pos += length;
            back.lba = pos_to_lba(back.pos);
            cb_fn(arg, 0);
            return;
        }

        // 否则就分成两份
        SPDK_NOTICELOG("trim back case 2. pos from  %lu to %lu, lba from  %lu to %lu\n", 
            back.pos, back.pos + length,
            pos_to_lba(back.pos), pos_to_lba(back.pos + length));
        uint64_t first_len = back_to_end();
        uint64_t second_len = length - first_len;

        back.pos += length;
        back.lba = pos_to_lba(back.pos);
        cb_fn(arg, 0);
        return;
    }

    static void rw_done(void *arg, int rberrno) {
        struct rblob_rw_ctx* ctx = (struct rblob_rw_ctx*)arg;

        if (rberrno) {
            SPDK_ERRLOG("rolling_blob lba:%lu len:%lu rw failed:%s\n", ctx->lba, ctx->len, spdk_strerror(rberrno));
            ctx->cb_fn(ctx->arg, {ctx->start_pos, ctx->len}, rberrno);
            delete ctx;
            return;
        }

        // 如果有next指针，说明后面还有要执行的读写请求
        if (ctx->next) {
          struct rblob_rw_ctx* next = (struct rblob_rw_ctx*)ctx->next;
          // SPDK_NOTICELOG("rw_done to next:%lu len:%lu\n", next->lba, next->len);
          if (next->is_read) {
            spdk_blob_io_readv(next->blob, next->channel, next->iov.data(), next->iov.size(), 
                              next->lba / unit_size, next->len / unit_size, rw_done, next);
          } else {
            spdk_blob_io_writev(next->blob, next->channel, next->iov.data(), next->iov.size(), 
                              next->lba / unit_size, next->len / unit_size, rw_done, next);
          }
          delete(ctx);
          return;
        }

        // SPDK_NOTICELOG("rw_done finish:%lu len:%lu\n", ctx->lba, ctx->len);
        if (!ctx->is_read) {
            // 如果是写，可能要移动一下front
            ctx->rb->maybe_advance_front(ctx->start_pos + ctx->len);
        }

        ctx->cb_fn(ctx->arg, {ctx->start_pos, ctx->len}, rberrno);
        delete(ctx);
    }

public:
    // 同步元数据，其实就是写一次super block
    void sync_md(rblob_op_complete cb_fn, void* arg) {
        struct rblob_sync_ctx* ctx = new rblob_sync_ctx();
        ctx->cb_fn = cb_fn; 
        ctx->arg = arg;

        serilize_super();
        spdk_blob_io_write(blob, channel, super.get_buf(),
                            0, super.len() / unit_size, md_done, ctx);
    }

    static void md_done(void *arg, int rberrno) {
        struct rblob_sync_ctx* ctx = (struct rblob_sync_ctx*)arg;

        if (rberrno) {
            SPDK_ERRLOG("rolling_blob sync md failed:%s\n", spdk_strerror(rberrno));
            ctx->cb_fn(ctx->arg, rberrno);
            delete ctx;
            return;
        }

        ctx->cb_fn(ctx->arg, rberrno);
        delete ctx;
    }

    void serilize_super() {
        size_t sz;
    
        super.reset();
        sz = encode_fixed64(super.get_append(), back.lba);
        super.inc(sz);

        sz = encode_fixed64(super.get_append(), back.pos);
        super.inc(sz);

        sz = encode_fixed64(super.get_append(), front.lba);
        super.inc(sz);

        sz = encode_fixed64(super.get_append(), front.pos);
        super.inc(sz);
    }

public:
    void close(rblob_op_complete cb_fn, void* arg) {
      struct rblob_close_ctx *ctx = new rblob_close_ctx(cb_fn, arg);

      spdk_blob_close(blob, close_done, ctx);
    }

    static void close_done(void *arg, int rberrno) {
      struct rblob_close_ctx* ctx = (struct rblob_close_ctx*)arg;

      if (rberrno) {
          SPDK_ERRLOG("rolling_blob close failed:%s\n", spdk_strerror(rberrno));
          ctx->cb_fn(ctx->arg, rberrno);
          delete ctx;
          return;
      }

      ctx->cb_fn(ctx->arg, 0);
      delete ctx;
    }

public:
    uint64_t back_pos() { return back.pos; }

    uint64_t front_pos() { return front.pos; } 

private:
    bool is_rolled() { 
        return front.pos / size()  != back.pos / size(); 
    }

    bool need_roll(uint64_t start, uint64_t length) { 
        return start / size()  != (start + length) / size(); 
    }

    uint64_t available() {
        return size() - used();
    }

    uint64_t front_to_end() {
        return end() - front.pos % size();
    }

    uint64_t back_to_end() {
        return end() - back.pos % size();
    }

    uint64_t pos_to_end(uint64_t pos) {
        return end() - pos % size();
    }

    uint64_t used() { return front.pos - back.pos; }

    // 前面4k是super block，用户是不能用的
    uint64_t size() { return blob_size - super_size; }
    
    // 可用区域的 begin 和 end
    uint64_t begin() { return 0; }
    uint64_t end() { return blob_size - super_size; }

    uint64_t pos_to_lba(uint64_t pos) { return pos % size() + super_size; }

    void maybe_advance_front(uint64_t offset) {
        auto it = inflight_rw.find(offset);
        if (it != inflight_rw.end()) {
            it->second = true;
        }

        uint64_t advance = front.pos;
        while (!inflight_rw.empty()) {
            it = inflight_rw.begin();

            if (!it->second)    
                break;

            advance = it->first;
            inflight_rw.erase(it);
        }

        front.pos = advance;
        front.lba = pos_to_lba(front.pos);
        // SPDK_NOTICELOG("front.pos:%lu front.lba:%lu\n", front.pos, front.lba);
    }


private:
    struct location {
        uint64_t pos; // 在虚拟文件地址中的位置
        uint64_t lba; // 在 blob 中的地址
    };

    struct spdk_blob* blob;
    struct spdk_io_channel *channel;
    uint64_t blob_size;

    location front;
    location back;
    spdk_buffer super;

    std::map<uint64_t, bool> inflight_rw;
};



using make_rblob_complete = std::function<void (void *arg, struct rolling_blob* rblob, int rberrno)>;

struct make_rblob_ctx {
    struct spdk_blob_store *bs;
    struct spdk_io_channel *channel;

    uint64_t blob_size;
    make_rblob_complete cb_fn;
    void* arg;
};

static void
make_open_done(void *arg, struct spdk_blob *blob, int rberrno) {
  struct make_rblob_ctx *ctx = (struct make_rblob_ctx *)arg;

  /// TODO(sunyifang): open 失败还是应该 close 掉，以后需要修改
  if (rberrno) {
      SPDK_ERRLOG("make_rolling_blob failed during open. error:%s\n", spdk_strerror(rberrno));
      ctx->cb_fn(ctx->arg, nullptr, rberrno);
      delete ctx;
      return;
  }

  struct rolling_blob* rblob = new rolling_blob(blob, ctx->channel, ctx->blob_size);
  SPDK_ERRLOG("rolling_blob size:%lu\n", sizeof(rolling_blob));
  ctx->cb_fn(ctx->arg, rblob, 0);
  delete ctx;
}

static void
make_create_done(void *arg, spdk_blob_id blobid, int rberrno) {
  struct make_rblob_ctx *ctx = (struct make_rblob_ctx *)arg;

  if (rberrno) {
      SPDK_ERRLOG("make_rolling_blob failed during create. error:%s\n", spdk_strerror(rberrno));
      ctx->cb_fn(ctx->arg, nullptr, rberrno);
      return;
  }

  spdk_bs_open_blob(ctx->bs, blobid, make_open_done, ctx);
}

void make_rolling_blob(struct spdk_blob_store *bs, struct spdk_io_channel *channel, 
                       uint64_t size, make_rblob_complete cb_fn, void* arg) 
{
  struct make_rblob_ctx* ctx;
  struct spdk_blob_opts opts;
  
  ctx = new make_rblob_ctx(bs, channel, size, cb_fn, arg);
  spdk_blob_opts_init(&opts, sizeof(opts));
  opts.num_clusters = size / spdk_bs_get_cluster_size(bs);
  spdk_bs_create_blob_ext(bs, &opts, make_create_done, ctx);
}