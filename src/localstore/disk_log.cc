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

#include "disk_log.h"
#include "localstore/aos/log_store.h"

#include <cstring>

SPDK_LOG_REGISTER_COMPONENT(disk_log)

namespace {

fastblock::aos::log_entry to_aos_entry(log_entry_t& e) {
  fastblock::aos::log_entry out;
  out.term = e.term_id;
  out.index = e.index;
  out.type = e.type;
  out.meta = e.meta;
  out.data.reserve(e.data.bytes());
  for (auto sbuf : e.data) {
    out.data.append(sbuf.get_buf(), sbuf.size());
  }
  return out;
}

log_entry_t from_aos_entry(const fastblock::aos::log_entry& e) {
  log_entry_t out;
  out.term_id = e.term;
  out.index = e.index;
  out.type = e.type;
  out.meta = e.meta;
  out.size = e.data.size();
  // raft path expects 4K-aligned buffer_list chunks.
  const uint64_t nblocks =
      out.size == 0 ? 0 : (out.size + 4095) / 4096;
  if (nblocks == 0) {
    return out;
  }
  out.data = make_buffer_list(nblocks);
  size_t off = 0;
  for (auto sbuf : out.data) {
    const size_t n =
        std::min(static_cast<size_t>(4096), e.data.size() - off);
    if (n > 0) {
      sbuf.append(e.data.data() + off, n);
      off += n;
    }
  }
  // log_entry.size is data bytes; pad to multiple of 4096 for raft_entry_to_log reverse.
  out.size = nblocks * 4096;
  return out;
}

} // namespace

void disk_log::append_aos(std::vector<log_entry_t>& entries,
                          log_op_complete cb_fn, void* arg) {
  if (_aos_log == nullptr) {
    if (cb_fn) {
      cb_fn(arg, -EINVAL);
    }
    return;
  }
  std::vector<fastblock::aos::log_entry> batch;
  batch.reserve(entries.size());
  for (auto& e : entries) {
    batch.push_back(to_aos_entry(e));
  }
  _aos_log->append_batch(
      std::move(batch),
      [cb_fn = std::move(cb_fn)](void* a, int err) {
        if (cb_fn) {
          cb_fn(a, err);
        }
      },
      arg);
}

void disk_log::read_aos(uint64_t start_index, uint64_t end_index,
                        log_op_with_entry_complete cb_fn, void* arg) {
  if (_aos_log == nullptr) {
    if (cb_fn) {
      cb_fn(arg, {}, -EINVAL);
    }
    return;
  }
  _aos_log->read(
      start_index, end_index,
      [cb_fn = std::move(cb_fn)](void* a,
                                 std::vector<fastblock::aos::log_entry> entries,
                                 int err) {
        if (err != 0) {
          if (cb_fn) {
            cb_fn(a, {}, err);
          }
          return;
        }
        std::vector<log_entry_t> out;
        out.reserve(entries.size());
        for (auto& e : entries) {
          out.push_back(from_aos_entry(e));
        }
        if (cb_fn) {
          cb_fn(a, std::move(out), 0);
        }
      },
      arg);
}

void make_aos_disk_log(fastblock::aos::log_store* log,
                       make_disklog_complete cb_fn, void* arg) {
  if (log == nullptr) {
    if (cb_fn) {
      cb_fn(arg, nullptr, -EINVAL);
    }
    return;
  }
  auto* dlog = new disk_log(log);
  dlog->start();
  if (cb_fn) {
    cb_fn(arg, dlog, 0);
  }
}

struct make_disklog_ctx {
    std::string pg;
    rolling_blob* rblob;

    make_disklog_complete cb_fn;
    void* arg;

    uint32_t shard_id;
};

static void
make_disklog_sync_done(void *arg, int logerrno) {
  struct make_disklog_ctx *ctx = (struct make_disklog_ctx *)arg;

  if (logerrno) {
      SPDK_ERRLOG("make_disklog failed. error:%s\n", spdk_strerror(logerrno));
      ctx->cb_fn(ctx->arg, nullptr, logerrno);
      delete ctx;
      return;
  }

  SPDK_INFOLOG(disk_log, "make_disklog pg:%s trim_back, in core %u thread %lu .\n", ctx->pg.c_str(),
      core_sharded::get_core_sharded().this_shard_id(), utils::get_spdk_thread_id());
  struct disk_log* dlog = new disk_log(ctx->rblob);
  ctx->cb_fn(ctx->arg, dlog, 0);
  delete ctx;
}

void
make_disklog_blob_done(void *arg, struct rolling_blob* rblob, int logerrno) {
  struct make_disklog_ctx *ctx = (struct make_disklog_ctx *)arg;
  // uint32_t shard_id = core_sharded::get_core_sharded().this_shard_id();
  log_xattr xattr{.shard_id = ctx->shard_id, .pg = ctx->pg};

  if (logerrno) {
      SPDK_ERRLOG("make_disk_log failed. error:%s\n", spdk_strerror(logerrno));
      ctx->cb_fn(ctx->arg, nullptr, logerrno);
      delete ctx;
      return;
  }

  ctx->rblob = rblob;
  xattr.blob_set_xattr(rblob->blob);
  spdk_blob_sync_md(rblob->blob, make_disklog_sync_done, ctx);
}

void make_disk_log(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
    std::string pg, make_disklog_complete cb_fn, void* arg, uint32_t shard_id)
{
  auto make_done = [shard_id, cb_fn = std::move(cb_fn), pg](void *arg, struct disk_log* dlog, int rerrno){
    if(rerrno == 0 && dlog) {
      SPDK_INFOLOG(disk_log, "start disklog pg:%s , in shard %u thread %lu .\n", pg.c_str(),
          shard_id, utils::get_spdk_thread_id());  
      dlog->start();          
    }
    cb_fn(arg, dlog, rerrno);
  };  

  struct make_disklog_ctx* ctx = new make_disklog_ctx{.pg = pg, .cb_fn = std::move(make_done), .arg = arg, .shard_id = shard_id};
  make_rolling_blob(bs, channel, rolling_blob::huge_blob_size, make_disklog_blob_done, ctx);
}

disk_log* make_disk_log(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
                    struct spdk_blob* blob){
    
    struct rolling_blob* rblob = make_rolling_blob(bs, channel, blob);
    struct disk_log* dlog = new disk_log(rblob);
    dlog->start();
    return dlog;
}