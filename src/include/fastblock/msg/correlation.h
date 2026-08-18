/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include <cstdint>

namespace msg {

using correlation_index_type = uint64_t;

inline uint32_t correlation_index_lo(correlation_index_type k) noexcept {
    return static_cast<uint32_t>(k);
}

inline uint32_t correlation_index_gen(correlation_index_type k) noexcept {
    return static_cast<uint32_t>(k >> 32);
}

inline correlation_index_type make_correlation_index(uint32_t gen, uint32_t idx) noexcept {
    return (static_cast<uint64_t>(gen) << 32) | idx;
}

} // namespace msg
