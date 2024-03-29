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

#include <stdint.h>
#include <string>
#include <limits>

#include "types.h"
#include "utils/varint.h"

struct log_entry_t {
    uint64_t term_id{init};
    uint64_t index{init};
    uint64_t size{init};

    uint64_t type{init};
    std::string meta;
    buffer_list data;

    static constexpr uint64_t init = std::numeric_limits<uint64_t>::max();
};

static constexpr uint64_t entry_header_size = sizeof(uint64_t) * 3;

inline bool
EncodeLogHeader(spdk_buffer& sbuf, log_entry_t& entry) {
    bool rc;

    rc = PutFixed64(sbuf, entry.term_id);
    if(!rc) return false;

    rc = PutFixed64(sbuf, entry.index);
    if(!rc) return false;

    rc = PutFixed64(sbuf, entry.size);
    if(!rc) return false;

    rc = PutFixed64(sbuf, entry.type);
    if(!rc) return false;

    rc = PutString(sbuf, entry.meta);
    if(!rc) return false;

    return true;
}

inline bool
DecodeLogHeader(spdk_buffer& sbuf, log_entry_t& entry) {
    bool rc;

    rc = GetFixed64(sbuf, entry.term_id);
    if(!rc) return false;

    rc = GetFixed64(sbuf, entry.index);
    if(!rc) return false;

    rc = GetFixed64(sbuf, entry.size);
    if(!rc) return false;

    rc = GetFixed64(sbuf, entry.type);
    if(!rc) return false;

    rc = GetString(sbuf, entry.meta);
    if(!rc) return false;

    return true;
}