#pragma once

#include "rolling_blob.h"
#include "coding.h"
#include "header_pool.h"
#include "log_entry.h"
#include "utils/units.h"

#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/string.h>

#include <absl/container/flat_hash_map.h>
#include <vector>
#include <utility>
#include <errno.h>

class raft_log;
using log_append_complete = std::function<void (void *arg, int rberrno)>;
using log_read_complete = std::function<void (void *arg, log_entry_t&&, int rberrno)>;
using log_op_complete = std::function<void (void *arg, int rberrno)>;

struct log_append_ctx {
  uint64_t index;
  buffer_list bl;

  log_append_complete cb_fn;
  void* arg;
  raft_log* log;
};

struct log_read_ctx {
  buffer_list bl;
  struct log_entry_t entry;

  uint64_t next_pos;

  log_read_complete cb_fn;
  void* arg;
  raft_log* log;
};

struct log_op_ctx {
  log_op_complete cb_fn;
  void* arg;
};

class raft_log {
    static constexpr uint64_t header_size = 4_KB;
public:
    raft_log(rolling_blob* rblob) : rblob(rblob), log_index(0) {}

    void append(log_entry_t* entry, log_append_complete cb_fn, void* arg) {
        struct log_append_ctx* ctx = new log_append_ctx{ .cb_fn = cb_fn, .arg = arg, .log = this};

        SPDK_NOTICELOG("log append index:%lu size:%lu name:%s\n", 
                entry->index, entry->size, entry->data.obj_name.c_str());

        auto sbuf = header_pool_get();
        EncodeLogHeader(sbuf, entry);

        ctx->bl = entry->data.buf;
        ctx->bl.prepend_buffer(sbuf);
        rblob->append(ctx->bl, log_append_done, ctx);
    }

    void trim_back(uint64_t length, log_append_complete cb_fn, void* arg) {
        rblob->trim_back(length, cb_fn, arg);
    };

    // TODO(sunyifang): 改成用reader类来读，因为有些状态是只属于本次读取的。
    void read(uint32_t index, log_read_complete cb_fn, void* arg) {
        struct log_read_ctx* ctx = new log_read_ctx{ .cb_fn = cb_fn, .arg = arg, .log = this};
        
        auto it = index_map.find(index);
        if (it == index_map.end()) {
            SPDK_ERRLOG("can not find index:%u\n", index);
            cb_fn(arg, {}, -EINVAL);
            return;
        }

        ctx->next_pos = it->second;
        ctx->bl = std::move(make_buffer_list(2));
        rblob->read(ctx->next_pos, 8_KB, ctx->bl, log_read_done, ctx);
    }

    void stop(log_op_complete cb_fn, void* arg) {
        rblob->close(cb_fn, arg);
    }

private:
    // 传进rolling_blob的回调函数
    static void log_append_done(void *arg, rblob_rw_result result, int rberrno) {
        struct log_append_ctx* ctx = (struct log_append_ctx*)arg;

        if (rberrno) {
            SPDK_ERRLOG("log append start:%lu len:%lu rw failed:%s\n", result.start_pos, result.len, spdk_strerror(rberrno));
            ctx->cb_fn(ctx->arg, rberrno);
            return;
        }

        SPDK_NOTICELOG("log write done, index:%lu start_pos:%lu len:%lu\n", ctx->index, result.start_pos, result.len);
        // 把header的buf再放回mempool。
        // TODO(sunyifang): 这里要么删掉mempool，
        //    要么把内存回收封装在spdk_buffer对象内部
        spdk_buffer header = ctx->bl.front();
        header_pool_put(header);
        ctx->bl.trim_front();
        SPDK_NOTICELOG("header_pool_put\n");

        ctx->log->maybe_index(ctx->index, result.start_pos);

        ctx->cb_fn(ctx->arg, 0);
        delete ctx;
    }

    static void log_read_done(void *arg, rblob_rw_result result, int rberrno) {
        struct log_read_ctx* ctx = (struct log_read_ctx*)arg;
        struct log_entry_t& entry = ctx->entry;

        SPDK_NOTICELOG("log read done\n");
        if (rberrno) {
            SPDK_ERRLOG("log append start:%lu len:%lu rw failed:%s\n", result.start_pos, result.len, spdk_strerror(rberrno));
            ctx->cb_fn(ctx->arg, {}, rberrno);
            return;
        }

        ctx->next_pos += result.len;

        // entry初始值默认都是0，如果index已经是非0值，说明已经解析过header
        if (!entry.index) {
          spdk_buffer sbuf = ctx->bl.front();
          DecodeLogHeader(sbuf, &entry);
          ctx->bl.trim_front();
          sbuf.free();
        }
        entry.data.buf.append_buffer(std::move(ctx->bl));

        // 如果entry的数据还没读完
        if (entry.size > entry.data.buf.bytes()) {
          auto remain = entry.size - entry.data.buf.bytes();
          ctx->bl = std::move(make_buffer_list(remain / 4096));
          ctx->log->rblob->read(ctx->next_pos, remain, ctx->bl, log_read_done, ctx);
        }

        ctx->cb_fn(ctx->arg, std::move(entry), 0);
        delete ctx;
    }

    void maybe_index(uint64_t index, uint64_t pos) {
        index_map.emplace(index, pos);
    }

private:
    rolling_blob* rblob;

    uint32_t log_index;
    absl::flat_hash_map<uint64_t, uint64_t> index_map;
};

