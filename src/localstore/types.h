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
#include "utils/varint.h"
#include "utils/log.h"

#include <spdk/blob.h>
#include <string>
#include <optional>
#include <variant>
#include <functional>
#include <map>
#include <spdk/string.h>

struct fb_blob {
    struct spdk_blob* blob   = nullptr;
    spdk_blob_id      blobid = 0;
};

enum class blob_type : uint32_t {
  log = 0,
  object = 1,
  kv = 2,
  kv_checkpoint = 3,
  kv_checkpoint_new = 4,
  super_blob = 5,
  free = 6,
};

inline std::string type_string(const blob_type& type) {
  switch (type) {
    case blob_type::log:
      return "blob_type::log";
    case blob_type::object:
      return "blob_type::object";
    case blob_type::kv:
      return "blob_type::kv";
    case blob_type::kv_checkpoint:
      return "blob_type::kv_checkpoint";
    case blob_type::kv_checkpoint_new:
      return "blob_type::kv_checkpoint_new";
    case blob_type::super_blob:
      return "blob_type::super_blob";
    case blob_type::free:
      return "blob_type::free";
    default:
      return "blob_type::unknown";
  }
}

using xattr_val_type = std::variant<blob_type, uint32_t, std::string>;

using rblob_xattr_complete = std::function<void (void *, int)>;

struct set_xattr_ctx {
    rblob_xattr_complete cb_fn;
    void* arg;
};

inline void sync_md_done(void *arg, int bserrno){
    struct set_xattr_ctx *ctx = (struct set_xattr_ctx *)arg;

    if (bserrno) {
        SPDK_ERRLOG_EX("set_blob_xattr failed. error:%s\n", spdk_strerror(bserrno));
        ctx->cb_fn(ctx->arg, bserrno);
        delete ctx;
        return;
    }
  
    ctx->cb_fn(ctx->arg, 0);
    delete ctx;    
}

inline void set_blob_xattr(
        struct spdk_blob* blob, 
        std::map<std::string, xattr_val_type>& xattr, 
        rblob_xattr_complete&& cb_fn, 
        void* arg){
    std::map<std::string, int> xattr_int = {
        {"type",  1},
        {"shard", 2},
        {"pg",    3},
        {"name",  4},
        {"snap",  5},
    };

    auto it = xattr.begin();
    std::string key;
    while(it != xattr.end()){
        key = it->first;
        if(xattr_int.find(key) == xattr_int.end()){
            it++;
            continue;
        }

        int key_int = xattr_int[key];
        switch (key_int){
        case 1:
        {
            blob_type type = std::get<blob_type>(it->second);
            spdk_blob_set_xattr(blob, key.c_str(), &type, sizeof(type));
            break;
        }
        case 2:
        {
            uint32_t shard_id = std::get<uint32_t>(it->second);
            spdk_blob_set_xattr(blob, key.c_str(), &shard_id, sizeof(shard_id));
            break;
        }
        case 3:
        {
            std::string pg = std::get<std::string>(it->second);
            spdk_blob_set_xattr(blob, key.c_str(), pg.c_str(), pg.size());
            break;
        }
        case 4:
        {
            std::string name = std::get<std::string>(it->second);
            spdk_blob_set_xattr(blob, key.c_str(), name.c_str(), name.size());
            break;
        }
        case 5:
        {
            std::string snap = std::get<std::string>(it->second);
            spdk_blob_set_xattr(blob, key.c_str(), snap.c_str(), snap.size());
            break; 
        }
        default:
            break;           
        }
        it++;
    }
    struct set_xattr_ctx *ctx = new set_xattr_ctx{.cb_fn = std::move(cb_fn), .arg = arg};
    spdk_blob_sync_md(blob, sync_md_done, ctx);
}

/**
 * 从spdk_buffer中读取数据的一系列函数。
 *
 * \param sbuf 等待解析的地址。
 * \param out 出参，获取的值放在out中，是一个引用类型。
 * \return bool值，表示是否读取成功。这里注意如果返回false，
 *         sbuf的状态要保持之前的状态，即sbuf的used数字不应该改变。
 *         所以都是先判断sbuf内存是否足够，然后才调用inc和append的。
 */
inline bool GetFixed32(spdk_buffer& sbuf, uint32_t& out) {
    if (sbuf.remain() < sizeof(uint32_t)) { return false; }

    out = decode_fixed32(sbuf.get_append());
    sbuf.inc(sizeof(uint32_t));
    return true;
}

inline bool GetFixed64(spdk_buffer& sbuf, uint64_t& out) {
    if (sbuf.remain() < sizeof(uint64_t)) { return false; }

    out = decode_fixed64(sbuf.get_append());
    sbuf.inc(sizeof(uint64_t));
    return true;
}

inline bool GetString(spdk_buffer& sbuf, std::string& out) {
    if (sbuf.remain() < sizeof(uint64_t)) { return false; }

    uint64_t str_size = decode_fixed64(sbuf.get_append());
    if (sbuf.remain() < sizeof(uint64_t) + str_size) { return false; }

    sbuf.inc(sizeof(uint64_t));

    out = std::string(sbuf.get_append(), str_size);
    sbuf.inc(str_size);
    return true;;
}

inline bool GetOptString(spdk_buffer& sbuf, std::optional<std::string>& out) {
    std::string str;
    bool rc = GetString(sbuf, str);
    if(!rc) {
        return false;
    }

    if (str.size() == 0) {
        out = std::nullopt;
    } else {
        out.emplace(std::move(str));
    }
    return true;
}

/**
 * 把数据序列化放进sbuf的一系列函数。
 *
 * \param sbuf 等待放入的地址，引用类型。
 * \param value 放入的值。
 * \return bool值，表示是否成功序列化。如果返回false，则sbuf应该
 *         保持进入函数之前的状态，即sbuf的used数字不应该改变。
 */
inline bool PutFixed32(spdk_buffer& sbuf, uint32_t value) {
    if (sbuf.remain() < sizeof(uint32_t)) { return false; }

    encode_fixed32(sbuf.get_append(), value);
    sbuf.inc(sizeof(uint32_t));
    return true;
}

inline bool PutFixed64(spdk_buffer& sbuf, uint64_t value) {
    if (sbuf.remain() < sizeof(uint64_t)) { return false; }

    encode_fixed64(sbuf.get_append(), value);
    sbuf.inc(sizeof(uint64_t));
    return true;
}

inline bool PutString(spdk_buffer& sbuf, const std::string& value) {
    if (sbuf.remain() < sizeof(uint64_t) + value.size()) { return false; }

    // 先把size序列化进去
    encode_fixed64(sbuf.get_append(), value.size());
    sbuf.inc(sizeof(uint64_t));

    sbuf.append(value.c_str(), value.size());
    return true;
}

inline bool PutOptString(spdk_buffer& sbuf, const std::optional<std::string>& value) {
    if (value) {
        return PutString(sbuf, *value);
    } else {
        // 如果没有值，只需要让序列化的size = 0，传入一个空字符串即可
        return PutString(sbuf, std::string());
    }
}

/**
 * 预先计算序列化后的长度，这样申请内存时候心里有数。
 */
inline uint64_t LengthString(const std::string& value) {
    return sizeof(uint64_t) + value.size();
}

inline uint64_t LengthOptString(const std::optional<std::string>& value) {
    return value ? sizeof(uint64_t) + value->size(): sizeof(uint64_t);
}