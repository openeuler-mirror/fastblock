#pragma once

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/rdma_client.h>

#include <cstring>
#include <memory>

namespace msg {
namespace rdma {

class pooled_chunk {

public:

    pooled_chunk() = delete;

    pooled_chunk(::spdk_mempool* pool, const size_t n = 1)
      : _pool{pool}, _capacity{n} {
        _data = std::make_unique<char[]>(_capacity);
        if (_capacity == 1) {
            _data[0] = reinterpret_cast<char*>(::spdk_mempool_get(_pool));
            if (_data[0]) { _got = true; }
        } else {
            auto rc = ::spdk_mempool_get_bulk(_pool, reinterpret_cast<void**>(_data.get()), _capacity);
            if (rc == 0) { _got = true; }
        }
    }

    pooled_chunk(const pooled_chunk&) = delete;

    pooled_chunk(pooled_chunk&& r)
      : _pool{std::exchange(r._pool, nullptr)}
      , _capacity{std::exchange(r._capacity, 0)}
      , _data{std::move(r._data)}
      , _got{std::exchange(r._got, false)} {}

    pooled_chunk& operator=(const pooled_chunk&) = delete;

    pooled_chunk& operator=(pooled_chunk&&) = delete;

    ~pooled_chunk() noexcept {
        if (not _got) { return; }

        if (_capacity == 1) {
            ::spdk_mempool_put(_pool, _data[0]);
        } else {
            ::spdk_mempool_put_bulk(_pool, reinterpret_cast<void**>(_data.get()), _capacity);
        }
    }

public:

    bool empty() noexcept { return !_got; }

    char* data() noexcept { return _data.get(); }

    char* head() noexcept { return &(_data[0]); }

    size_t capacity() noexcept { return _capacity; }

    void append(const size_t nth, const void* src) {
        std::memcpy(&(_data[nth]), src, SPDK_SRV_MEMORY_POOL_ELEMENT_SIZE);
    }

    std::unique_ptr<::iovec[]> to_iovs() {
        if (not _got) { return nullptr; }
        auto ret = std::make_unique<::iovec[]>(new ::iovec[_capacity]);
        for (size_t i{0}; i < _capacity; ++i) {
            ret[i].iov_base = &(_data[i]);
            ret[i].iov_len = SPDK_SRV_MEMORY_POOL_ELEMENT_SIZE;
        }
        return ret;
    }

private:

    ::spdk_mempool* _pool{nullptr};
    size_t _capacity{0};
    std::unique_ptr<char[]> _data{nullptr};
    bool _got{false};
};

} // namespace rdma
} // namespace msg
