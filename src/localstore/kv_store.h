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

#include "spdk_buffer.h"
#include "buffer_pool.h"
#include "types.h"
#include "rolling_blob.h"
#include "kv_checkpoint.h"

#include <spdk/env.h>
#include <spdk/util.h>
#include <absl/container/flat_hash_map.h>
#include <optional>
#include <string>
#include <vector>
#include <stdlib.h>

using kvstore_rw_complete = std::function<void (void *, int)>;

class kvstore;
class kvstore_loader;

struct op {
  std::string key;
  std::optional<std::string> value;
};


struct kvstore_write_ctx {
  std::vector<op> ops;
  uint64_t op_length;
  kvstore* kvs;

  kvstore_rw_complete cb_fn;
  void* arg;
};

struct kvstore_read_ctx {
  kvstore* kvs;
  kvstore_loader* kvloader;

  kvstore_rw_complete cb_fn;
  void* arg;
  uint64_t start_pos;
  uint64_t len;
  spdk_buffer read_buf;
  rolling_blob* rblob;
};

struct kvstore_ckpt_ctx {
  kvstore* kvs;
  kv_checkpoint* kv_ckpt;

  kvstore_rw_complete cb_fn;
  void* arg;

  buffer_list bl;
};

class kvstore {
public:
    kvstore(rolling_blob* rblob) : rblob(rblob) {
        char* wbuf = (char*)spdk_malloc(32_MB,
                        0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
                        SPDK_MALLOC_DMA);
        char* rbuf = (char*)spdk_malloc(32_MB,
                        0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
                        SPDK_MALLOC_DMA);
        write_buf = spdk_buffer(wbuf, 32_MB);
        read_buf = spdk_buffer(rbuf, 32_MB);
        _worker_poller = SPDK_POLLER_REGISTER(worker_poll, this, 100000); // 10ms写一次
    }

    ~kvstore() {
        spdk_free(write_buf.get_buf());
        spdk_free(read_buf.get_buf());
        if (_worker_poller) {
            spdk_poller_unregister(&_worker_poller);
        }
    }

    void clear() {
        table.clear();
    }

    //只有在load_kvstore时才会调用
    void set_checkpoint_blobid(spdk_blob_id checkpoint_blob_id, spdk_blob_id new_checkpoint_blob_id){
        checkpoint.set_checkpoint_blobid(checkpoint_blob_id, new_checkpoint_blob_id);
    }

    void stop(kvstore_rw_complete cb_fn, void* arg) {
        if (_worker_poller) {
            spdk_poller_unregister(&_worker_poller);
            _worker_poller = nullptr;
        }

        rblob->close(
          [cb_fn = std::move(cb_fn), this](void* arg, int rberrno){
            SPDK_NOTICELOG("kvstore stop\n");
            rblob->stop();
            delete rblob; // rblob指针由kvstore负责delete
            checkpoint.stop(
              [cb_fn = std::move(cb_fn), this](void* arg, int rberrno){
                  cb_fn(arg, rberrno);
              },
              arg);
          },
          arg);
    }

    void put(std::string key, std::optional<std::string> value) {
        op_length += LengthString(key)  + LengthOptString(value);
        op_log.emplace_back(std::move(key), std::move(value));
    }

    // remove只是放在op_log数组中，可以直接move进来
    void remove(std::string key) {
        op_length += LengthString(key)  + LengthOptString(std::nullopt);
        op_log.emplace_back(std::move(key), std::nullopt);
    }

    std::optional<std::string> get(std::string key) {
        if (auto it = table.find(key); it != table.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void apply_op(std::string key, std::optional<std::string> value) {
        auto it = table.find(key);
        if (it != table.end()) {
            if (value) {
                it->second = std::move(*value);                     // 更新
            } else {
                table.erase(it);                                    // 删除
            }
        } else {
            if (value) {
                table.emplace(std::move(key), std::move(*value));   // 插入
            } else {
                // 非法操作，删除一个不存在的值
                SPDK_DEBUGLOG(kvlog, "error deleting non existent key: %s", key.c_str());
            }
        }
    }

    bool need_commit() { return op_log.size() > 0; }

    /**
     * commit: 把当前的op_log追加到磁盘。
     *
     * 现在用户不应该主动调用了，因为poller中会执行。
     */
    void commit(kvstore_rw_complete cb_fn, void* arg) {
        if (working) {
            cb_fn(arg, -EBUSY);
            return;
        }

        if (!need_commit()) {
            cb_fn(arg, 0);
            return;
        }
        working = true;

        struct kvstore_write_ctx* ctx = new kvstore_write_ctx();
        ctx->ops = std::exchange(op_log, {});
        ctx->op_length = std::exchange(op_length, 0);
        ctx->cb_fn = cb_fn;
        ctx->arg = arg;
        ctx->kvs = this;

        serialize_op(ctx->ops);

        buffer_list bl;
        auto buf = spdk_buffer(write_buf.get_buf(), SPDK_ALIGN_CEIL(write_buf.used(), 4096));
        SPDK_DEBUGLOG(kvlog, "op size:%lu op_length:%lu op used:%lu commit used:%lu aligned size:%lu\n",
                ctx->ops.size(), ctx->op_length, write_buf.used()-sizeof(uint64_t)*2, write_buf.used(), buf.size());
        bl.append_buffer(buf);
        rblob->append(bl, commit_done, ctx);
    }

    static void commit_done(void *arg, rblob_rw_result result, int rberrno) {
        struct kvstore_write_ctx* ctx = (kvstore_write_ctx*)arg;

        if (rberrno) {
            SPDK_ERRLOG("kvstore commit failed:%s\n", spdk_strerror(rberrno));
            ctx->kvs->op_log.insert(ctx->kvs->op_log.begin(), ctx->ops.begin(), ctx->ops.end());
            ctx->kvs->op_length += ctx->op_length;
            ctx->cb_fn(ctx->arg, rberrno);
            delete ctx;
            return;
        }

        for (auto& op : ctx->ops) {
            ctx->kvs->apply_op(op.key, op.value);
        }

        SPDK_DEBUGLOG(kvlog, "commit result start_pos:%lu len:%lu, used:%lu size:%lu remain:%lu\n",
                result.start_pos, result.len, ctx->kvs->rblob->used(), ctx->kvs->rblob->size(), ctx->kvs->rblob->remain());
        SPDK_DEBUGLOG(kvlog, "commit_done, rblob back pos %lu  front pos %lu\n", ctx->kvs->rblob->back_pos(), ctx->kvs->rblob->front_pos());
        ctx->kvs->working = false;
        ctx->cb_fn(ctx->arg, 0);
        delete ctx;
    }

    void serialize_op(std::vector<op>& ops) {
        bool rc;
        write_buf.reset();

        // 1.保存本次写的 total_size (uint64_t)：
        //    total_size = sizeof(uint64_t) + sizeof(uint64_t) + op序列化后的size
        //   先占好位置，序列化结束后再写入
        write_buf.inc(sizeof(uint64_t));

        // 2.保存op个数
        PutFixed64(write_buf, ops.size());

        for (auto& op : ops) {
            SPDK_DEBUGLOG(kvlog, "++++++  key: %s value: %s\n", op.key.c_str(), op.value->c_str());
            PutString(write_buf, op.key);
            PutOptString(write_buf, op.value);
        }

        uint64_t data_size = write_buf.used();
        encode_fixed64(write_buf.get_buf(), data_size);
    }

    std::vector<op> deserialize_op() {
        bool rc;
        uint64_t data_size = 0;
        uint64_t op_size = 0;
        std::string key;
        std::optional<std::string> value;
        std::vector<op> ops;

        read_buf.reset();
        rc = GetFixed64(read_buf, data_size);
        rc = GetFixed64(read_buf, op_size);
        if(data_size == 0 || op_size == 0)
            return ops;

        ops.reserve(op_size);
        for (size_t i = 0; i < op_size; i++) {
            rc = GetString(read_buf, key);
            rc = GetOptString(read_buf, value);
            ops.emplace_back(std::move(key), std::move(value));
        }
        return ops;
    }

public:
    /**
     * work的逻辑：
     *                   <是否需要checkpoint?>
     *                   Yes /         \ No
     *        save_checkpoint() ---> <是否需要commit?>
     *                                Yes /         \ No
     *                              commit() ---> [no operation]
     * 整个流程不可重入，因此用 working 变量上锁。
     */
    static int worker_poll(void *arg) {
        kvstore* ctx = (kvstore*)arg;
        return ctx->maybe_work() ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
    }

    bool maybe_work() {
        // SPDK_NOTICELOG("maybe_work working:%d need_checkpoint:%d need_commit():%d\n",
        //                 working, need_checkpoint(), need_commit());
        if (working) { return false; }

        if (need_checkpoint()) {
            SPDK_DEBUGLOG(kvlog, "need_checkpoint. op_length:%lu rblob->remain:%lu\n", 
                        op_length, rblob->remain());
            // save checkpoint之后commit
            save_checkpoint(
              [](void* arg, int ckerrno){
                  kvstore* kvs = (kvstore*)arg;
                  kvs->commit([](void *, int){ }, nullptr);
              },
              this);
            return true;
        } else if (need_commit()) {
            SPDK_DEBUGLOG(kvlog, "need_commit. op_length:%lu op_size:%lu rblob->remain:%lu\n", 
                        op_length, op_log.size(), rblob->remain());
            // 如果不需要save checkpoint，就判断是否需要commit
            commit([](void *, int){ }, nullptr);
            return true;
        }
        return false;
    }

    /// TODO: 这里可以改成，判断rblob剩余空间，是否足够op_log下一次写入
    bool need_checkpoint() { return op_length > rblob->remain(); }

public:
    /**
     * checkpoint: 把整个table全部内容序列化到一个blob中，然后把rblob全部trim。
     */
    void save_checkpoint(kvstore_rw_complete cb_fn, void* arg) {
        if (working) {
            cb_fn(arg, -EBUSY);
            return;
        }
        working = true;

        uint64_t pr_size;
        struct kvstore_ckpt_ctx* ctx = new kvstore_ckpt_ctx();

        ctx->kvs = this;
        ctx->cb_fn = std::move(cb_fn);
        ctx->arg = arg;
        ctx->kv_ckpt = &checkpoint;

        buffer_list bl = make_buffer_list(1);
        auto bl_encoder = buffer_list_encoder(bl);
        bl_encoder.put(table.size());
        for (auto& pr : table) {
            // 如果当前sbuf不够存，那就在后面再追加一块内存
            pr_size = LengthString(pr.first) + LengthString(pr.second);
            if (pr_size > bl_encoder.remain()) {
                bl.append_buffer(buffer_pool_get());
            }

            bl_encoder.put(pr.first);
            bl_encoder.put(pr.second);
        }
        ctx->bl = std::move(bl);

        SPDK_DEBUGLOG(kvlog, "table serialized. map size:%lu buffer_list size:%lu\n", table.size(), ctx->bl.bytes());
        checkpoint.start_checkpoint(ctx->bl.bytes(), checkpoint_start_complete, ctx);
    }

    // 开始之后直接写
    static void checkpoint_start_complete(void *arg, int ckerror) {
        struct kvstore_ckpt_ctx* ctx = (kvstore_ckpt_ctx*)arg;

        // SPDK_WARNLOG("checkpoint_start_complete\n");
        ctx->kv_ckpt->write_checkpoint(ctx->bl, checkpoint_write_complete, ctx);
    }

    // 写完就finish
    static void checkpoint_write_complete(void *arg, int ckerror) {
        struct kvstore_ckpt_ctx* ctx = (kvstore_ckpt_ctx*)arg;

        // SPDK_WARNLOG("checkpoint_write_complete\n");
        ctx->kv_ckpt->finish_checkpoint(checkpoint_finish_complete, ctx);
    }

    // finish之后trim
    static void checkpoint_finish_complete(void *arg, int ckerror) {
        struct kvstore_ckpt_ctx* ctx = (kvstore_ckpt_ctx*)arg;

        // SPDK_WARNLOG("checkpoint_finish_complete\n");
        ctx->kvs->rblob->trim_back(ctx->kvs->rblob->used(), checkpoint_trim_complete, ctx);
    }

    // trim后调用cb_fn
    static void checkpoint_trim_complete(void *arg, int ckerror) {
        struct kvstore_ckpt_ctx* ctx = (kvstore_ckpt_ctx*)arg;

        // SPDK_NOTICELOG("checkpoint_trim_complete\n");
        free_buffer_list(ctx->bl);
        ctx->kvs->working = false;
        ctx->cb_fn(ctx->arg, ckerror);
        delete ctx;
    }

public:
    /**
     * 从磁盘中加载checkpoint到table中。一般是从磁盘恢复时使用。
     */
    void load_checkpoint(kvstore_rw_complete cb_fn, void* arg) {
        struct kvstore_ckpt_ctx* ctx = new kvstore_ckpt_ctx();

        ctx->kvs = this;
        ctx->cb_fn = std::move(cb_fn);
        ctx->arg = arg;
        ctx->kv_ckpt = &checkpoint;

        checkpoint.open_checkpoint(checkpoint_open_complete, ctx);
    }

    static void checkpoint_open_complete(void *arg, int ckerror) {
        struct kvstore_ckpt_ctx* ctx = (kvstore_ckpt_ctx*)arg;

        if(err::E_NODEV == ckerror){
            SPDK_INFOLOG(kvlog, "checkpoint open failed:%s\n", err::string_status(ckerror));
            ctx->cb_fn(ctx->arg, 0);
            delete ctx;
            return;
        }
        if (ckerror) {
            SPDK_ERRLOG("checkpoint open failed:%s\n", spdk_strerror(ckerror));
            ctx->cb_fn(ctx->arg, ckerror);
            delete ctx;
            return;
        }

        auto size = ctx->kv_ckpt->checkpoint_size();
        ctx->bl = make_buffer_list(size / 4096);
        // SPDK_WARNLOG("checkpoint_open_complete, size:%lu.\n", size);
        ctx->kv_ckpt->read_checkpoint(ctx->bl, checkpoint_read_complete, ctx);
    }

    /**
     * TODO: 修改整个库的序列化的逻辑。增加异常处理机制。
     */
    static void checkpoint_read_complete(void *arg, int ckerror) {
        struct kvstore_ckpt_ctx* ctx = (kvstore_ckpt_ctx*)arg;
        buffer_list_encoder bl_encoder(ctx->bl);
        uint64_t table_size = 0;
        bool rc = true;

        if (ckerror) {
            SPDK_ERRLOG("checkpoint read failed:%s\n", spdk_strerror(ckerror));
            goto complete;
        }

        rc = bl_encoder.get(table_size);
        if (!rc) { goto complete; }

        // SPDK_DEBUGLOG(kvlog, "table_size:%lu.\n", table_size);
        for (uint64_t i = 0; i < table_size; i++) {
            std::string key, value;
            rc = bl_encoder.get(key);
            if (!rc) { goto complete; }

            rc = bl_encoder.get(value);
            if (!rc) { goto complete; }

            // SPDK_WARNLOG("-- key %s value %s\n", key.c_str(), value.c_str());
            ctx->kvs->table.emplace(std::move(key), std::move(value));
        }
        // SPDK_WARNLOG("after read checkpoint, table size:%lu.\n", ctx->kvs->table.size());

      complete:
        if (!rc) {
            SPDK_ERRLOG("bl_encoder get failed.\n");
        }
        free_buffer_list(ctx->bl);
        ctx->cb_fn(ctx->arg, ckerror);
        delete ctx;
    }

    size_t size() { return table.size(); }

    void replay(kvstore_rw_complete cb_fn, void* arg);

public:
    absl::flat_hash_map<std::string, std::string> table;
    std::vector<op> op_log;
    uint64_t op_length = 0;

    spdk_buffer write_buf;
    spdk_buffer read_buf;
    rolling_blob* rblob;
    kv_checkpoint checkpoint;
    // checkpoint和commit都由poller触发执行
    struct spdk_poller *_worker_poller;
    bool working = false;

    friend class kvstore_loader;
};

class kvstore_loader {
public:
    kvstore_loader(kvstore* k)
    : kvs(k)
    , rblob(kvs->rblob)
    , read_buf(kvs->read_buf)
    , pos(rblob->back_pos())
    , end(rblob->front_pos()) { };

    // commit一批op，叫做一个batch
    // 每次读只读一个batch
    void replay_one_batch(kvstore_rw_complete cb_fn, void* arg) {
        struct kvstore_read_ctx* ctx;

        ctx = new kvstore_read_ctx();
        ctx->kvs = this->kvs;
        ctx->kvloader = this;
        ctx->cb_fn = cb_fn;
        ctx->arg = arg;
        ctx->start_pos = pos;
        ctx->len = 4096;
        ctx->read_buf = read_buf;
        ctx->rblob = rblob;

        read_buf.reset();
        auto buf = spdk_buffer(read_buf.get_buf(), 4096);
        // SPDK_WARNLOG("kv replay_one_batch read start:%lu len:%lu\n", ctx->start_pos, ctx->len);
        rblob->read(ctx->start_pos, 4096, buf, replay_one_batch_done, ctx, true);
    }

    static void replay_one_batch_done(void* arg, rblob_rw_result, int rberrno) {
        struct kvstore_read_ctx* ctx = (struct kvstore_read_ctx*)arg;
        uint64_t data_size;

        if (rberrno) {
            SPDK_ERRLOG("kv replay_one_batch fail. start:%lu len:%lu error:%s\n", ctx->start_pos, ctx->len, spdk_strerror(rberrno));
            ctx->cb_fn(ctx->arg, rberrno);
            delete ctx->kvloader;
            delete ctx;
            return;
        }

        ctx->read_buf.reset();
        GetFixed64(ctx->read_buf, data_size);
        // 每次都先读4096个字节，超出大小才会继续读
        if (data_size > ctx->len) {
            ctx->len = SPDK_ALIGN_CEIL(data_size, 4096);
            auto buf = spdk_buffer(ctx->read_buf.get_buf() + 4096, ctx->len - 4096);
            // SPDK_WARNLOG("kv replay_one_batch continue. start:%lu len:%lu\n", 
                // ctx->start_pos + 4096, ctx->len - 4096);
            ctx->rblob->read(ctx->start_pos + 4096, ctx->len - 4096, buf, replay_one_batch_done, ctx, true);
            return;
        }

        auto ops = ctx->kvs->deserialize_op();
        for (auto& op : ops) {
            SPDK_INFOLOG(kvlog, "------  key: %s value: %s\n", op.key.c_str(), op.value->c_str());
            ctx->kvs->apply_op(op.key, op.value);
        }

        //加载kvstore的rblob时，front_pos()位置之后可能还有有效的数据，必须能到遇到无效数据时才停止读取
        bool finished = ops.size() == 0;
        // 如果还没有 replay 到终点，就继续 replay
        auto replayed_pos = ctx->start_pos + ctx->len;
        // bool finished = replayed_pos >= ctx->kvloader->end;
        // SPDK_DEBUGLOG(kvlog, "kv table size:%lu. replayed_pos:%lu end:%lu finish?%d.\n", 
        //         ctx->kvs->table.size(), replayed_pos, ctx->kvloader->end, finished);
        if (!finished) {
            ctx->start_pos = replayed_pos;
            ctx->len = 4096;

            ctx->read_buf.reset();
            auto buf = spdk_buffer(ctx->read_buf.get_buf(), 4096);
            // SPDK_NOTICELOG("kv replay next batch. start:%lu len:%lu\n", ctx->start_pos, ctx->len);
            ctx->rblob->read(ctx->start_pos, 4096, buf, replay_one_batch_done, ctx, true);
            return;
        }
        ctx->rblob->set_front(ctx->start_pos);
        SPDK_DEBUGLOG(kvlog, "rblob back pos %lu  front pos %lu\n", ctx->rblob->back_pos(), ctx->rblob->front_pos());

        // 到这里说明replay完成了，到达终点
        ctx->cb_fn(ctx->arg, rberrno);
        delete ctx->kvloader;
        delete ctx;
        return;
    }

    kvstore* kvs;
    rolling_blob* rblob;
    spdk_buffer read_buf;
    uint64_t pos;
    uint64_t end;
};

/**
 * 只有在load_kvstore时会调用此函数
 * 先load_checkpoint，然后replay所有的op。
 */
inline void kvstore::replay(kvstore_rw_complete cb_fn, void* arg) {
    /* 
     * 删除kv_checkpoint的_new_blob。
     * 当save_checkpoint没有结束时遇到osd掉线，会导致kv_checkpoint的_new_blob是不完整的，因此需要删除，
     * 之后会重新触发save_checkpoint
     */
    checkpoint.delete_new_blob(
      [this, cb_fn = std::move(cb_fn)](void *arg, int kverrno){
        if(kverrno){
          cb_fn(arg, kverrno); 
          return; 
        }

        auto kvloader = new kvstore_loader(this);
        SPDK_DEBUGLOG(kvlog, "rblob back pos %lu  front pos %lu\n", kvloader->rblob->back_pos(), kvloader->rblob->front_pos());
        load_checkpoint(
          [cb_fn = std::move(cb_fn), arg] (void *arg1, int kverrno) {
              if(kverrno){
                  cb_fn(arg, kverrno); 
                  return;
              }
              kvstore_loader* kvloader = (struct kvstore_loader*)arg1;
              kvloader->replay_one_batch(std::move(cb_fn), arg);
          }, 
          kvloader); 

      },
      arg);

}

using make_kvs_complete = std::function<void (void *arg, struct kvstore* kvs, int kverrno)>;

void make_kvstore(struct spdk_blob_store *bs,
                  struct spdk_io_channel *channel,
                  make_kvs_complete cb_fn, void* arg);

void load_kvstore(spdk_blob_id blob_id, spdk_blob_id checkpoint_blob_id,
                  spdk_blob_id new_checkpoint_blob_id, struct spdk_blob_store *bs, 
                  struct spdk_io_channel *channel,
                  make_kvs_complete cb_fn, void* arg);