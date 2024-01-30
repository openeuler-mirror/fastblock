/* Copyright (c) 2024 ChinaUnicom
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
#include <algorithm>
#include <string>
#include <sstream>

class disk_log;

using log_op_complete = std::function<void (void *arg, int rberrno)>;
using log_op_with_entry_complete = std::function<void (void *arg, std::vector<log_entry_t>&&, int rberrno)>;

struct log_append_ctx {
  std::vector<std::tuple<uint64_t,uint64_t,uint64_t,uint64_t>> idx_pos;
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
    disk_log(rolling_blob* rblob) : _rblob(rblob) {
      _trim_poller = SPDK_POLLER_REGISTER(trim_poller, this, 5000);
    }
    ~disk_log() {
      delete _rblob;
      spdk_poller_unregister(&_trim_poller);
    }

    void append(std::vector<log_entry_t>& entries, log_op_complete cb_fn, void* arg) {
        struct log_append_ctx* ctx = new log_append_ctx{ .cb_fn = cb_fn, .arg = arg, .log = this};

        uint64_t pos = _rblob->front_pos();
        for (auto& entry : entries) {
            auto sbuf = buffer_pool_get();
            EncodeLogHeader(sbuf, entry);

            // 注意：append时，entry.data是从外面传进来的，而header是在本函数中申请的。
            //      所以在回调函数中回收这些，并不回收后面的buffer_list
            ctx->headers.emplace_back(sbuf);
            ctx->bl.append_buffer(sbuf);
            ctx->bl.append_buffer(entry.data);
            /// NOTE: 这个vector是写完之后要往map里保存的，从raft_index到rblob中pos和size的映射，
            ///    为了方便读取，这里保存的size，是包括了header长度(4_KB)和数据长度的。
            ctx->idx_pos.emplace_back(entry.index, pos, entry.size + 4_KB, entry.term_id);
            pos += (entry.size + 4_KB);
        }

        _rblob->append(ctx->bl, log_append_done, ctx);
    }

    void append(log_entry_t& entry, log_op_complete cb_fn, void* arg) {
        struct log_append_ctx* ctx = new log_append_ctx{ .cb_fn = cb_fn, .arg = arg, .log = this};

        uint64_t pos = _rblob->front_pos();
        auto sbuf = buffer_pool_get();
        EncodeLogHeader(sbuf, entry);

        ctx->headers.emplace_back(sbuf);
        ctx->bl.append_buffer(sbuf);
        ctx->bl.append_buffer(entry.data);
        ctx->idx_pos.emplace_back(entry.index, pos, entry.size + 4_KB, entry.term_id);

        _rblob->append(ctx->bl, log_append_done, ctx);
    }

    // TODO(sunyifang): 改成用reader类来读，因为有些状态是只属于本次读取的。
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

        auto start_it = _index_map.find(start_index);
        auto end_it = _index_map.find(end_index);
        if (start_it == _index_map.end() || end_it == _index_map.end()) {
            SPDK_ERRLOG("can not find index. start:%lu end:%lu\n", start_index, end_index);
            cb_fn(arg, {}, -EINVAL);
            return;
        }

        auto [pos, size, _] = start_it->second;
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
        _rblob->read(pos, size, ctx->bl, log_read_done, ctx);
    }

    // TODO(sunyifang): 不应该这样free。
    // 但现在rblob是在外面用户new出来，然后传进来的。
    void stop(log_op_complete cb_fn, void* arg) {
        // 始终记住cb_fn是上面一层传入的函数，我们传入的lambda会在close之后调用
        // 调用时，里面会把arg传进lambda的第一个参数
        _rblob->close(
          [cb_fn, this](void* arg, int rberrno){
            SPDK_NOTICELOG("disklog stop\n");
            _rblob->stop();
            cb_fn(arg, rberrno);
          },
          arg);

    }

    uint64_t get_term_id(uint64_t index) {
        if (auto it = _index_map.find(index); it != _index_map.end()) {
            return it->second.term_id;
        }
        return 0;
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
        for (auto [idx, pos, size, term_id] : ctx->idx_pos) {
            ctx->log->maybe_index(idx, log_position{pos, size, term_id});
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
            DecodeLogHeader(sbuf, entry);
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

public:
    static int trim_poller(void *arg) {
        disk_log* ctx = (disk_log*)arg;
        return ctx->maybe_trim() ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
    }

    // 只保留apply_index前面的1000条，超过1500条就trim一次
    bool maybe_trim() {
      _polls_count++;
      if (_trim_index - _lowest_index > 1500) {
          // trim前： lowest_index: 100  apply_index: 1700
          //   trim:  从 100 到 700
          // trim后： lowest_index: 701  apply_index: 1700 (一共保留1000条)
          trim_back(_lowest_index, _trim_index - 1000, [](void *, int){}, nullptr);
          _trims_count++;
          return true;
      }
      return false;
    }

    /**
     * trim范围 [start_index, end_index]，包括 end_index 在内也会读到
     */
    void trim_back(uint64_t start_index, uint64_t end_index, log_op_complete cb_fn, void* arg) {
        if (end_index < start_index) {
            SPDK_ERRLOG("end_index little than start_index. start:%lu end:%lu\n", start_index, end_index);
            cb_fn(arg, -EINVAL);
            return;
        }
        auto start_it = _index_map.find(start_index);
        auto end_it = _index_map.find(end_index);
        if (start_it == _index_map.end() || end_it == _index_map.end()) {
            SPDK_ERRLOG("can not find index. start:%lu end:%lu\n", start_index, end_index);
            cb_fn(arg, -EINVAL);
            return;
        }

        end_it++;
        uint64_t trim_length = 0;
        for (auto it = start_it; it != end_it; it++) {
            trim_length += it->second.size;
        }

        // 处理 disklog 中保存的数据
        _index_map.erase(start_it, end_it);
        _lowest_index = end_index + 1;

        _rblob->trim_back(trim_length, cb_fn, arg);
    }

    void advance_trim_index(uint64_t index) {
        _trim_index = std::max(_trim_index, index);
    }

    std::string dump_state() {
      std::stringstream sstream;
      sstream << "\nlowest_index:" << _lowest_index
              << " apply_index:" << _trim_index
              << " highest_index:" << _highest_index
              << "\nhold:" << _highest_index - _lowest_index + 1
              << " index_map size:" << _index_map.size()
              << "\nindex_map begin:" << _index_map.begin()->first
              << " index_map rbegin:" << _index_map.rbegin()->first
              << " _polls_count:" << _polls_count
              << " _trims_count:" << _trims_count
              << "\nrblob state:" << _rblob->dump_state()
              << std::endl;
      return sstream.str();
    }

private:
    rolling_blob* _rblob;
    struct spdk_poller *_trim_poller;
    uint64_t _polls_count = 0;
    uint64_t _trims_count = 0;

    uint64_t _lowest_index = 1; //只有trim时候会改
    uint64_t _trim_index = 1;
    uint64_t _highest_index = 1;
    struct log_position {
        uint64_t pos;
        uint64_t size; // data.size + header size(4_KB)
        uint64_t term_id;
    };
    std::map<uint64_t, log_position> _index_map;

private:
    void maybe_index(uint64_t index, log_position&& log_pos) {
        _highest_index = std::max(_highest_index, index);
        _index_map.emplace(index, std::move(log_pos));
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
