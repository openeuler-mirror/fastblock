/* Copyright (c) 2023 ChinaUnicom
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

#include <bit>
#include <chrono>

#include "utils/fmt.h"

namespace msg {
namespace rdma {

namespace {
    consteval uint64_t make_mask(const uint64_t length, const uint64_t offset) noexcept {
        return ((static_cast<uint64_t>(1) << length) - 1) << offset;
    }
}

class work_request_id {

public:

    using value_type = uint64_t;
    using dispatch_id_type = uint64_t;
    using connection_id_type = uint16_t;
    using request_id_type = uint16_t;
    using request_id_pointer = request_id_type*;

private:

    using shard_id_type = uint16_t;
    using epoch_type = uint32_t;

private:

    static constexpr uint8_t _shard_id_len{9};
    static constexpr uint8_t _epoch_len{27}; // seconds level
    static constexpr uint8_t _connection_id_len{12};
    static constexpr uint8_t _request_id_len{16};

    static constexpr uint8_t _shard_id_shift{static_cast<uint8_t>(sizeof(value_type)) * 8 - _shard_id_len};
    static constexpr uint8_t _epoch_shift{_shard_id_shift - _epoch_len};
    static constexpr uint8_t _connection_id_shift{_epoch_shift - _connection_id_len};
    static constexpr uint8_t _request_id_shift{_connection_id_shift - _request_id_len};

    static constexpr uint64_t _shard_id_mask{make_mask(_shard_id_len, _shard_id_shift)};
    static constexpr uint64_t _epoch_mask{make_mask(_epoch_len, _epoch_shift)};
    static constexpr uint64_t _connection_id_mask{make_mask(_connection_id_len, _connection_id_shift)};
    static constexpr uint64_t _request_id_mask{make_mask(_request_id_len, _request_id_shift)};

private:

    void copy_bits(const uint64_t src, const uint8_t offset, const uint64_t mask) noexcept {
        _value |= (src << offset) & mask;
    }

    request_id_pointer request_id_position() noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            return reinterpret_cast<request_id_pointer>(&_value);
        } else {
            auto val_pos = reinterpret_cast<request_id_pointer>(&_value);
            return reinterpret_cast<request_id_pointer>(
              val_pos + sizeof(value_type) - sizeof(request_id_type));
        }
    }

public:

    explicit work_request_id(value_type v) noexcept : _value{v} {}

    explicit work_request_id(connection_id_type conn_id) noexcept : _value{0} {
        auto shard_id = ::spdk_env_get_current_core();
        copy_bits(static_cast<uint64_t>(shard_id), _shard_id_shift, _shard_id_mask);

        auto epoch = std::chrono::system_clock::now().time_since_epoch().count();
        copy_bits(epoch, _epoch_shift, _epoch_mask);
        copy_bits(static_cast<uint64_t>(conn_id), _connection_id_shift, _connection_id_mask);

        _request_id_pointer = request_id_position();
    }

    work_request_id(const work_request_id& w) noexcept
      : _value{w._value}, _request_id_pointer{request_id_position()} {}

    work_request_id& operator=(const work_request_id& w) noexcept {
        _value = w._value;
        _request_id_pointer = request_id_position();

        return *this;
    }

    work_request_id(work_request_id&& w) noexcept
      : _value{w._value}, _request_id_pointer{request_id_position()} {}

    work_request_id& operator=(work_request_id&& w) noexcept {
        _value = w._value;
        _request_id_pointer = request_id_position();

        return *this;
    }

    ~work_request_id() noexcept = default;

public:

    value_type value() noexcept {
        return _value;
    }

    value_type value() const noexcept {
        return _value;
    }

#if defined(__arm__) || defined(__aarch64__)
#pragma GCC push_options
#pragma GCC optimize ("O2")
#endif
    void inc_request_id() noexcept {
        *_request_id_pointer = *_request_id_pointer + 1;
    }
#if defined(__arm__) || defined(__aarch64__)
#pragma GCC pop_options
#endif

    dispatch_id_type dispatch_id() noexcept {
        return _value & ~_request_id_mask;
    }

    request_id_type request_id() noexcept {
        return *_request_id_pointer;
    }

    shard_id_type shard_id() const noexcept {
        return work_request_id::shard_id(_value);
    }

    connection_id_type connection_id() const noexcept {
        return work_request_id::connection_id(_value);
    }

    request_id_type request_id() const noexcept {
        return work_request_id::request_id(_value);
    }

    std::string fmt() {
        return work_request_id::fmt(_value);
    }


public:

    static dispatch_id_type dispatch_id(value_type* id) noexcept {
        return (*id) & ~_request_id_mask;
    }

    static dispatch_id_type dispatch_id(value_type id) noexcept {
        return id & ~_request_id_mask;
    }

    static request_id_type request_id(value_type id) noexcept {
        return id & _request_id_mask;
    }

    static shard_id_type shard_id(value_type id) noexcept {
        return (id & _shard_id_mask) >> _shard_id_shift;
    }

    static connection_id_type connection_id(value_type id) noexcept {
        return (id & _connection_id_mask) >> _connection_id_shift;
    }

    static std::string fmt(value_type id) {
        return FMT_5(
          "{value: %1%, shard id: %2%, connection id: %3%, dispatch id: %4% request id: %5%}",
          id, shard_id(id), connection_id(id), dispatch_id(id), request_id(id));
    }

    friend std::ostream& operator<<(std::ostream& o, const work_request_id& v) {
        o << "{value: " << v._value
          << ", shard id: " << v.shard_id()
          << ", connection id: " << v.connection_id()
          << ", request id: " << v.request_id() << "}";

        return o;
    }

private:

    value_type _value;
    request_id_pointer _request_id_pointer;
};

} // namespace rdma
} // namespace msg

namespace std {
template<>
struct hash<msg::rdma::work_request_id> {
    size_t operator()(const msg::rdma::work_request_id& id) const noexcept {
        return id.value();
    }
};
} // namespace std
