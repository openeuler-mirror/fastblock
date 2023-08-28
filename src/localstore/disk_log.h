#pragma once

#include "rolling_blob.h"
#include "buffer_pool.h"
#include "log_entry.h"
#include "utils/units.h"
#include "utils/varint.h"
#include "utils/utils.h"

#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/string.h>

#include <absl/container/flat_hash_map.h>
#include <vector>
#include <utility>
#include <errno.h>

class disk_log;

using log_op_complete = std::function<void (void *arg, int rberrno)>;
using log_op_with_entry_complete = std::function<void (void *arg, std::vector<log_entry_t>&&, int rberrno)>;

struct log_append_ctx {
  std::vector<std::tuple<uint64_t,uint64_t,uint64_t>> idx_pos;
  std::vector<spdk_buffer> headers;
  buffer_list bl;

  log_op_complete cb_fn;
  void* arg;
  disk_log* log;
};

struct log_read_ctx {
  buffer_list bl;
  std::vector<log_entry_t> entries;

  uint64_t start_index;
  uint64_t end_index;

  log_op_with_entry_complete cb_fn;
  void* arg;
};

struct log_op_ctx {
  log_op_complete cb_fn;
  void* arg;
};

class disk_log {
    static constexpr uint64_t header_size = 4_KB;

public:
    disk_log(rolling_blob* rblob) : rblob(rblob), log_index(0) {}
    ~disk_log() { delete rblob; }

    void append(std::vector<log_entry_t>& entries, log_op_complete cb_fn, void* arg) {
        struct log_append_ctx* ctx = new log_append_ctx{ .cb_fn = cb_fn, .arg = arg, .log = this};

        uint64_t pos = rblob->front_pos();
        for (auto& entry : entries) {
            auto sbuf = buffer_pool_get();
            EncodeLogHeader(sbuf, entry);

            // 注意：append时，entry.data是从外面传进来的，而header是在本函数中申请的。
            //      所以在回调函数中回收这些，并不回收后面的buffer_list
            ctx->headers.emplace_back(sbuf);
            ctx->bl.append_buffer(sbuf);
            ctx->bl.append_buffer(std::move(entry.data));
            /// NOTE: 这个vector是写完之后要往map里保存的，从raft_index到rblob中pos和size的映射，
            ///    为了方便读取，这里保存的size，是包括了header长度(4_KB)和数据长度的。
            ctx->idx_pos.emplace_back(entry.index, pos, entry.size + 4_KB);
            pos += (entry.size + 4_KB);
        }

        rblob->append(ctx->bl, log_append_done, ctx);
    }

    void append(log_entry_t& entry, log_op_complete cb_fn, void* arg) {
        struct log_append_ctx* ctx = new log_append_ctx{ .cb_fn = cb_fn, .arg = arg, .log = this};

        uint64_t pos = rblob->front_pos();
        auto sbuf = buffer_pool_get();
        EncodeLogHeader(sbuf, entry);

        ctx->headers.emplace_back(sbuf);
        ctx->bl.append_buffer(sbuf);
        ctx->bl.append_buffer(std::move(entry.data));
        ctx->idx_pos.emplace_back(entry.index, pos, entry.size + 4_KB);

        rblob->append(ctx->bl, log_append_done, ctx);
    }

    void trim_back(uint64_t length, log_op_complete cb_fn, void* arg) {
        rblob->trim_back(length, cb_fn, arg);
    };


    void read(uint64_t index, log_op_with_entry_complete cb_fn, void* arg) {
        read(index, index, cb_fn, arg);
    }

    // 读取范围 [start_index, end_index]，包括 end_index 在内也会读到
    void read(uint64_t start_index, uint64_t end_index, log_op_with_entry_complete cb_fn, void* arg) {
        struct log_read_ctx* ctx;
        
        if (end_index < start_index) {
            SPDK_ERRLOG("end_index litter than start_index. start:%lu end:%lu\n", start_index, end_index);
            cb_fn(arg, {}, -EINVAL);
            return;
        }

        auto start_it = index_map.find(start_index);
        auto end_it = index_map.find(end_index);
        if (start_it == index_map.end() || end_it == index_map.end()) {
            SPDK_ERRLOG("can not find index. start:%lu end:%lu\n", start_index, end_index);
            cb_fn(arg, {}, -EINVAL);
            return;
        }

        auto [pos, size] = start_it->second;
        start_it++;
        end_it++;
        for (auto it = start_it; it != end_it; it++) {
            size += it->second.size;
        }

        ctx = new log_read_ctx{ .cb_fn = cb_fn, .arg = arg};
        ctx->start_index = start_index;
        ctx->end_index = end_index;
        ctx->bl = std::move(make_buffer_list(size / 4096));
        // 注意:read的时候，buffer_list整体都是在这里申请的。
        //     其中header部分在decode后马上就free了，而data部分会放在log_entry.data中返回给调用者
        rblob->read(pos, size, ctx->bl, log_read_done, ctx);
    }

    // TODO(sunyifang): 不应该这样free。
    // 但现在rblob是在外面用户new出来，然后传进来的。
    void stop(log_op_complete cb_fn, void* arg) {
        // 始终记住cb_fn是上面一层传入的函数，我们传入的lambda会在close之后调用
        // 调用时，里面会把arg传进lambda的第一个参数
        rblob->close(
          [cb_fn, this](void* arg, int rberrno){
            SPDK_NOTICELOG("disklog stop\n");
            rblob->stop();
            cb_fn(arg, rberrno);
          },
          arg);
        
    }

private:
    // 传进rolling_blob的回调函数
    static void log_append_done(void *arg, rblob_rw_result result, int rberrno) {
        struct log_append_ctx* ctx = (struct log_append_ctx*)arg;

        if (rberrno) {
            SPDK_ERRLOG("log append fail. start:%lu len:%lu error:%s\n", result.start_pos, result.len, spdk_strerror(rberrno));
            ctx->cb_fn(ctx->arg, rberrno);
            delete ctx;
            return;
        }
    
        // 回收header
        for (auto header : ctx->headers) {
            buffer_pool_put(header);
        }        

        // 保存每个index到pos和size的映射
        for (auto [idx, pos, size] : ctx->idx_pos) {
            ctx->log->maybe_index(idx, pos, size);
        }

        ctx->cb_fn(ctx->arg, 0);
        delete ctx;
    }

    static void log_read_done(void *arg, rblob_rw_result result, int rberrno) {
        struct log_read_ctx* ctx = (struct log_read_ctx*)arg;

        if (rberrno) {
            SPDK_ERRLOG("log append start_index:%lu end_index:%lu (rblob pos:%lu len:%lu) read failed:%s\n", 
                        ctx->start_index, ctx->end_index, result.start_pos, result.len, spdk_strerror(rberrno));
            ctx->cb_fn(ctx->arg, {}, rberrno);
            delete ctx;
            return;
        }

        auto index_size = ctx->end_index - ctx->start_index + 1;
        ctx->entries.reserve(index_size);
        for (uint64_t i = 0; i < index_size; i++) {
            ctx->entries.emplace_back();
            auto& entry = ctx->entries[i];

            spdk_buffer sbuf = ctx->bl.pop_front();
            sbuf.reset();
            DecodeLogHeader(sbuf, &entry);
            buffer_pool_put(sbuf);

            buffer_list&& bl = ctx->bl.pop_front_list(entry.size / 4096);
            entry.data = bl;
        }

        if (ctx->bl.bytes()) {
            SPDK_ERRLOG("bl remain byutes:%lu\n", ctx->bl.bytes());
            free_buffer_list(ctx->bl);
        }
        ctx->cb_fn(ctx->arg, std::move(ctx->entries), 0);
        delete ctx;
    }


private:
    rolling_blob* rblob;

    uint32_t log_index;
    struct log_position {
        uint64_t pos;
        uint64_t size;
    };
    std::map<uint64_t, log_position> index_map;

private:
    void maybe_index(uint64_t index, uint64_t pos, uint64_t size) {
        index_map.emplace(index, log_position{pos, size});
    }
};

using make_disklog_complete = std::function<void (void *arg, struct disk_log* dlog, int rberrno)>;

struct make_disklog_ctx {
    make_disklog_complete cb_fn;
    void* arg;
};

static void
make_disk_log_done(void *arg, struct rolling_blob* rblob, int logerrno) {
  struct make_disklog_ctx *ctx = (struct make_disklog_ctx *)arg;

  if (logerrno) {
      SPDK_ERRLOG("make_disk_log failed. error:%s\n", spdk_strerror(logerrno));
      ctx->cb_fn(ctx->arg, nullptr, logerrno);
      delete ctx;
      return;
  }

  struct disk_log* dlog = new disk_log(rblob);
  ctx->cb_fn(ctx->arg, dlog, 0);
  delete ctx;
}

inline void make_disk_log(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
                   make_disklog_complete cb_fn, void* arg) 
{
  struct make_disklog_ctx* ctx;
  
  ctx = new make_disklog_ctx(cb_fn, arg);
  make_rolling_blob(bs, channel, rolling_blob::huge_blob_size, make_disk_log_done, ctx);
}