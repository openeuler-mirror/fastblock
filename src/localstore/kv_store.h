#pragma once

#include "spdk_buffer.h"
#include "rolling_blob.h"

#include <spdk/env.h>
#include <spdk/util.h>
#include <absl/container/flat_hash_map.h>
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
    }

    ~kvstore() {
      spdk_free(write_buf.get_buf());
      spdk_free(read_buf.get_buf());
    }

    void clear() {
        table.clear();
    }

    void stop(kvstore_rw_complete cb_fn, void* arg) {
        rblob->close(
          [cb_fn, this](void* arg, int rberrno){
            SPDK_NOTICELOG("kvstore stop\n");
            rblob->stop();
            cb_fn(arg, rberrno);
          },
          arg);
    }

    void put(std::string key, std::optional<std::string> value) {
        op_log.emplace_back(std::move(key), std::move(value));
    }

    // remove只是放在op_log数组中，可以直接move进来
    void remove(std::string key) {
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
                // SPDK_WARNLOG("error deleting non existent key: %s", key.c_str()); 
            }
        }
    }   

    void persist(kvstore_rw_complete cb_fn, void* arg) {
        if (persisting) {
          // 如果需要马上执行下次persist，就设置为true，等待这次写完马上执行下次persist
          if (!persist_waiting) {
            persist_waiting = true;
          }
          return;
        }
        
        struct kvstore_write_ctx* ctx;

        ctx = new kvstore_write_ctx();
        ctx->ops = std::exchange(op_log, {});
        ctx->cb_fn = cb_fn;
        ctx->arg = arg;
        ctx->kvs = this;

        SPDK_NOTICELOG("op size:%lu\n", ctx->ops.size());
        serialize_op(ctx->ops);

        buffer_list bl;
        auto buf = spdk_buffer(write_buf.get_buf(), SPDK_ALIGN_CEIL(write_buf.used_size(), 4096));
        SPDK_NOTICELOG("persist used: %lu aligned size: %lu, \n", write_buf.used_size(), buf.len()); 
        bl.append_buffer(buf);
        rblob->append(bl, persist_done, ctx);
    }

    void replay(kvstore_rw_complete cb_fn, void* arg);

    static void persist_done(void *arg, rblob_rw_result result, int rberrno) {
        struct kvstore_write_ctx* ctx = (kvstore_write_ctx*)arg;

        if (rberrno) {
            SPDK_ERRLOG("kvstore persist failed:%s\n", spdk_strerror(rberrno));
            ctx->kvs->op_log.insert(ctx->kvs->op_log.begin(), ctx->ops.begin(), ctx->ops.end());
            ctx->cb_fn(ctx->arg, rberrno);
            delete ctx;
            return;
        }

        for (auto& op : ctx->ops) {
            ctx->kvs->apply_op(op.key, op.value);
        }

        SPDK_NOTICELOG("persist result start_pos:%lu len:%lu\n", result.start_pos, result.len);
        ctx->cb_fn(ctx->arg, 0);
        delete ctx;
    }

    void serialize_op(std::vector<op>& ops) {
        size_t rc, sz;
        write_buf.reset();
        // 1.保存本次写的size：uint64大小(size) + uint64大小(op个数) + op序列化后的size
        //   先占好位置，序列化结束后再写入
        rc = write_buf.inc(sizeof(uint64_t));
    
        // 2.保存op个数
        sz = encode_fixed64(write_buf.get_append(), ops.size());
        rc = write_buf.inc(sz);

        for (auto& op : ops) {
            auto& key = op.key;
            auto& value = op.value;

            sz = encode_fixed64(write_buf.get_append(), key.size());
            rc = write_buf.inc(sz);

            /// TODO: spdk_buf接口写的不是很好，encode时候需要手动调用inc，append则不需要
            rc = write_buf.append(key);

            uint32_t val_size = value ? value->size() : 0;
            sz = encode_fixed64(write_buf.get_append(), val_size);
            rc = write_buf.inc(sz);

            if (value) {
                rc = write_buf.append(*value);
            }
        }
        uint64_t data_size = write_buf.used_size();
        encode_fixed64(write_buf.get_buf(), data_size);
    }

    std::vector<op> deserialize_op() {
        size_t sz, rc, str_size, op_size;
        std::string key, value;
        std::vector<op> ops;

        read_buf.reset();
        auto [data_size, _] = decode_fixed64(read_buf.get_append(), read_buf.remain());
        read_buf.inc(sizeof(uint64_t));

        std::tie(op_size, sz) = decode_fixed64(read_buf.get_append(), read_buf.remain());
        read_buf.inc(sz);

        ops.reserve(op_size);
        for (size_t i = 0; i < op_size; i++) {
            std::tie(str_size, sz) = decode_fixed64(read_buf.get_append(), read_buf.remain());
            read_buf.inc(sz);

            key = std::string(read_buf.get_append(), str_size);
            read_buf.inc(str_size);
            
            std::tie(str_size, sz) = decode_fixed64(read_buf.get_append(), read_buf.remain());
            read_buf.inc(sz);

            if (str_size) {
                value = std::string(read_buf.get_append(), str_size);
                rc = read_buf.inc(str_size);
                ops.emplace_back(std::move(key), std::move(value));
                continue;
            }

            ops.emplace_back(std::move(key), std::nullopt);
        }
        return ops;
    }

    size_t size() { return table.size(); }

    absl::flat_hash_map<std::string, std::string> table;
    std::vector<op> op_log;

public:
    spdk_buffer write_buf;
    spdk_buffer read_buf;
    rolling_blob* rblob;

    bool persisting = false;
    bool persist_waiting = false;

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

    // persist一批op，叫做一个batch
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
        SPDK_NOTICELOG("kv replay_one_batch read start:%lu len:%lu\n", ctx->start_pos, ctx->len);
        rblob->read(ctx->start_pos, 4096, buf, replay_one_batch_done, ctx);
    }

    static void replay_one_batch_done(void* arg, rblob_rw_result, int rberrno) {
        struct kvstore_read_ctx* ctx = (struct kvstore_read_ctx*)arg;

        if (rberrno) {
            SPDK_ERRLOG("kv replay_one_batch fail. start:%lu len:%lu error:%s\n", ctx->start_pos, ctx->len, spdk_strerror(rberrno));
            ctx->cb_fn(ctx->arg, rberrno);
            delete ctx->kvloader;
            delete ctx;
            return;
        }

        auto [data_size, sz] = decode_fixed64(ctx->read_buf.get_append(), ctx->read_buf.remain());
        // 每次都先读4096个字节，超出大小才会继续读
        if (data_size > ctx->len) {
            ctx->len = SPDK_ALIGN_CEIL(data_size, 4096);
            auto buf = spdk_buffer(ctx->read_buf.get_buf() + 4096, ctx->len - 4096);
            SPDK_NOTICELOG("kv replay_one_batch continue. start:%lu len:%lu\n", 
                ctx->start_pos + 4096, ctx->len - 4096);
            ctx->rblob->read(ctx->start_pos + 4096, ctx->len - 4096, buf, replay_one_batch_done, ctx);
            return;
        }

        SPDK_NOTICELOG("kv replay_one_batch done. start:%lu len:%lu\n", ctx->start_pos, ctx->len);
        auto ops = ctx->kvs->deserialize_op();
        for (auto& op : ops) {
            ctx->kvs->apply_op(op.key, op.value);
        }

        // 如果还没有 replay 到终点，就继续 replay
        auto replayed = ctx->start_pos + ctx->len;
        SPDK_NOTICELOG("kv replayed:%lu end:%lu\n", replayed, ctx->kvloader->end);
        if (replayed < ctx->kvloader->end) {
            ctx->start_pos = replayed;
            ctx->len = 4096;

            ctx->read_buf.reset();
            auto buf = spdk_buffer(ctx->read_buf.get_buf(), 4096);
            SPDK_NOTICELOG("kv replay next batch. start:%lu len:%lu\n", ctx->start_pos, ctx->len);
            ctx->rblob->read(ctx->start_pos, 4096, buf, replay_one_batch_done, ctx);
            return;
        }

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

void kvstore::replay(kvstore_rw_complete cb_fn, void* arg) {
    auto kvloader = new kvstore_loader(this);
    kvloader->replay_one_batch(cb_fn, arg);
}