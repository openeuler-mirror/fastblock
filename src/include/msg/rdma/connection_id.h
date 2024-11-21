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

#include <spdk/env.h>

#include <chrono>
#include <compare>

#include <boost/container_hash/hash.hpp>

namespace msg {
namespace rdma {

class connection_id {
public:

    using serial_type = uint32_t;

public:

    connection_id() noexcept = default;

    connection_id(const uint64_t guid, const serial_type serial) noexcept
      :_hash{guid} {
        boost::hash_combine(_hash, ::spdk_env_get_current_core());
        boost::hash_combine(_hash, now_epoch());
        boost::hash_combine(_hash, serial);
    }

    connection_id(
      const uint64_t guid,
      const uint32_t shard_id,
      const serial_type serial) noexcept : _hash{guid} {
        boost::hash_combine(_hash, shard_id);
        boost::hash_combine(_hash, now_epoch());
        boost::hash_combine(_hash, serial);
    }

    connection_id(size_t value) noexcept : _hash{value} {}

    connection_id(const connection_id&) noexcept = default;

    connection_id& operator=(const connection_id&) noexcept = default;

    connection_id(connection_id&&) noexcept = default;

    connection_id& operator=(connection_id&&) noexcept = default;

    ~connection_id() noexcept = default;

    std::partial_ordering operator<=>(const connection_id& that) noexcept {
        if (_hash != that._hash) {
            return std::partial_ordering::unordered;
        }

        return std::partial_ordering::equivalent;
    }

    friend bool
    operator==(const connection_id& lhs, const connection_id& rsh) noexcept {
        return lhs._hash == rsh._hash;
    }

    friend std::ostream& operator<<(std::ostream& o, const connection_id& id) {
        return o << id._hash;
    }

public:

    size_t value() noexcept {
        return _hash;
    }

    size_t value() const noexcept {
        return _hash;
    }

    void update(const uint64_t guid, const serial_type serial) noexcept {
        _hash = guid;
        boost::hash_combine(_hash, ::spdk_env_get_current_core());
        boost::hash_combine(_hash, now_epoch());
        boost::hash_combine(_hash, serial);
    }

private:

    [[gnu::always_inline]] static int64_t now_epoch() noexcept {
        namespace ch = std::chrono;

        const auto epoch = ch::system_clock::now().time_since_epoch();
        return ch::duration_cast<ch::nanoseconds>(epoch).count();
    }

private:

    size_t _hash{0};
};

} // namespace rdma
} // namespace msg

namespace std {
template<>
struct hash<msg::rdma::connection_id> {
    size_t operator()(const msg::rdma::connection_id& cid) const noexcept {
        return cid.value();
    }
};
}
