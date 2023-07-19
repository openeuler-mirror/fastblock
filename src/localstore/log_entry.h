#pragma once

#include <stdint.h>
#include <string>

struct log_entry_data_t {
    std::string obj_name;
    buffer_list buf;
};

struct log_entry_t {
    uint64_t term_id{0};
    uint64_t index{0};
    uint64_t size{0};
    log_entry_data_t data;
};

static constexpr uint64_t u64_size = sizeof(uint64_t);

static constexpr uint64_t entry_header_size = sizeof(uint64_t) * 3;

inline bool 
EncodeLogHeader(spdk_buffer& sbuf, log_entry_t* entry) {
    size_t rc;

    EncodeFixed64(sbuf.get_append(), entry->term_id);
    rc = sbuf.inc(u64_size);
    if (rc != u64_size) { return false; }

    EncodeFixed64(sbuf.get_append(), entry->index);
    rc = sbuf.inc(u64_size);
    if (rc != u64_size) { return false; }

    EncodeFixed64(sbuf.get_append(), entry->size);
    rc = sbuf.inc(u64_size);
    if (rc != u64_size) { return false; }

    auto& str = entry->data.obj_name;
    EncodeFixed64(sbuf.get_append(), str.size());
    rc = sbuf.inc(u64_size);
    if (rc != u64_size) { return false; }

    rc = sbuf.append(str.c_str(), str.size());
    if (rc != str.size()) { return false; }
}

inline bool
DecodeLogHeader(spdk_buffer& sbuf, log_entry_t* entry) {
    uint64_t str_size;
    size_t rc;

    entry->term_id = DecodeFixed64(sbuf.get_append());
    rc = sbuf.inc(u64_size);
    if (rc != u64_size) { return false; }

    entry->index = DecodeFixed64(sbuf.get_append());
    rc = sbuf.inc(u64_size);
    if (rc != u64_size) { return false; }

    entry->size = DecodeFixed64(sbuf.get_append());
    rc = sbuf.inc(u64_size);
    if (rc != u64_size) { return false; }

    str_size = DecodeFixed64(sbuf.get_append());
    rc = sbuf.inc(u64_size);
    if (rc != u64_size) { return false; }

    std::string str = std::string(sbuf.get_append(), str_size);
    rc = sbuf.inc(str_size);
    if (rc != str_size) { return false; }

    entry->data.obj_name = std::move(str);
    return true;
}