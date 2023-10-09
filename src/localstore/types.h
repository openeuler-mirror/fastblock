#pragma once

#include "spdk_buffer.h"
#include "utils/varint.h"

#include <spdk/blob.h>
#include <string>
#include <optional>

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
};

inline std::string type_string(blob_type& type) {
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
    default:
      return "blob_type::unknown";
  }
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