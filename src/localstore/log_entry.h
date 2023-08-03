#pragma once

#include <stdint.h>
#include <string>
#include <limits>

#include "utils/varint.h"

struct log_entry_data_t {
    std::string obj_name;
    buffer_list buf;
};

struct log_entry_t {
    uint64_t term_id{init};
    uint64_t index{init};
    uint64_t size{init};
    log_entry_data_t data;

    static constexpr uint64_t init = std::numeric_limits<uint64_t>::max();
};

static constexpr uint64_t entry_header_size = sizeof(uint64_t) * 3;

inline bool 
EncodeLogHeader(spdk_buffer& sbuf, log_entry_t& entry) {
    size_t rc, sz;

    sz = encode_fixed64(sbuf.get_append(), entry.term_id);
    rc = sbuf.inc(sz);
    if (rc != sz) { return false; }

    sz = encode_fixed64(sbuf.get_append(), entry.index);
    rc = sbuf.inc(sz);
    if (rc != sz) { return false; }

    sz = encode_fixed64(sbuf.get_append(), entry.size);
    rc = sbuf.inc(sz);
    if (rc != sz) { return false; }

    auto& str = entry.data.obj_name;
    sz = encode_fixed64(sbuf.get_append(), str.size());
    rc = sbuf.inc(sz);
    if (rc != sz) { return false; }

    rc = sbuf.append(str.c_str(), str.size());
    if (rc != str.size()) { return false; }

    return true;
}

inline bool
DecodeLogHeader(spdk_buffer& sbuf, log_entry_t* entry) {
    size_t sz, rc, str_size;

    std::tie(entry->term_id, sz) = decode_fixed64(sbuf.get_append(), sbuf.remain());
    rc = sbuf.inc(sz);
    
    std::tie(entry->index, sz) = decode_fixed64(sbuf.get_append(), sbuf.remain());
    rc = sbuf.inc(sz);

    std::tie(entry->size, sz) = decode_fixed64(sbuf.get_append(), sbuf.remain());
    rc = sbuf.inc(sz);

    std::tie(str_size, sz) = decode_fixed64(sbuf.get_append(), sbuf.remain());
    rc = sbuf.inc(sz);

    std::string str = std::string(sbuf.get_append(), str_size);
    rc = sbuf.inc(str_size);
    if (rc != str_size) { return false; }

    entry->data.obj_name = std::move(str);
    return true;
}