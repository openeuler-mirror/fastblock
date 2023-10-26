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

#include <memory>

namespace msg {
namespace rdma {

class memory_pool {

public:

	using value_type = ::spdk_mempool;
	using pointer = value_type*;

public:

	memory_pool() noexcept = default;

	memory_pool(auto&&... args) noexcept {
		_pool = ::spdk_mempool_create(std::forward<decltype(args)>(args)...);
	}

	memory_pool(const memory_pool&) noexcept = delete;

	memory_pool(memory_pool&& v) noexcept : _pool{std::exchange(v._pool, nullptr)} {}

	memory_pool& operator=(const memory_pool&) = delete;

	memory_pool& operator=(memory_pool&&) = delete;

	~memory_pool() noexcept {
		if (_pool) {
			::spdk_mempool_free(_pool);
		}
	}

public:

	inline auto get() noexcept { return _pool; }

    template<typename T>
    T* get() noexcept {
        return reinterpret_cast<T*>(::spdk_mempool_get(_pool));
    }

    void put(void* p) noexcept {
        ::spdk_mempool_put(_pool, p);
    }

private:

	pointer _pool{nullptr};
};

} // namespace rdma
} // namespace msg
