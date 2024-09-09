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

#include <chrono>
#include <cstdint>
#include <string>

namespace fastblock {
namespace client {

struct leader_key_type {
    int32_t pool_id;
    int32_t pg_id;
};

struct leader_osd_info {
    int32_t leader_id{-1};
    int32_t pool_id{-1};
    int32_t pg_id{-1};
    std::string addr{};
    int32_t port{-1};
    std::chrono::system_clock::time_point epoch{};
    bool is_valid{false};
    bool is_onflight{true};
};

inline uint64_t make_leader_key(const int32_t pool_id, const int32_t pg_id) noexcept {
    uint64_t leader_osd_key{};
    auto* struct_key = reinterpret_cast<leader_key_type*>(&leader_osd_key);
    struct_key->pg_id = pg_id;
    struct_key->pool_id = pool_id;

    return leader_osd_key;
}

inline leader_key_type from_leader_key(uint64_t key_val) noexcept {
    auto* struct_key = reinterpret_cast<leader_key_type*>(&key_val);
    return { struct_key->pool_id, struct_key->pg_id };
}

} // namespace client
} // namespace fastblock
