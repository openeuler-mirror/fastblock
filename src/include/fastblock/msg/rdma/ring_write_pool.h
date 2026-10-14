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

#include "fastblock/msg/rdma/memory_pool.h"
#include "fastblock/utils/fmt.h"

#include <spdk/log.h>
#include <spdk/thread.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace msg {
namespace rdma {

class ring_write_pool {
public:
    using pool_type = memory_pool<::ibv_send_wr>;
    using net_context = pool_type::net_context;

    ring_write_pool() = delete;

    ring_write_pool(
      ::ibv_pd* pd,
      std::string name,
      size_t min_capacity,
      size_t max_capacity,
      size_t element_size,
      int sock_id = SPDK_ENV_SOCKET_ID_ANY)
      : _pd{pd}
      , _name{std::move(name)}
      , _min_capacity{min_capacity}
      , _max_capacity{max_capacity}
      , _element_size{element_size}
      , _sock_id{sock_id} {
        add_chunk(_min_capacity);
        _shrink_poller = SPDK_POLLER_REGISTER(shrink_poller, this, 1000000);
    }

    ring_write_pool(const ring_write_pool&) = delete;
    ring_write_pool(ring_write_pool&&) = delete;
    ring_write_pool& operator=(const ring_write_pool&) = delete;
    ring_write_pool& operator=(ring_write_pool&&) = delete;

    ~ring_write_pool() noexcept {
        free();
    }

    net_context* get() noexcept {
        std::lock_guard<std::mutex> guard{_mutex};
        ++_get_count;
        for (auto& chunk : _chunks) {
            auto* ctx = chunk->pool->get();
            if (!ctx) {
                continue;
            }

            ++chunk->in_use;
            ++_in_use;
            --_free_count;
            if (_in_use > _peak_in_use) {
                _peak_in_use = _in_use;
            }
            sample_pressure();
            return ctx;
        }

        ++_get_fail_count;
        ++_window_get_fail_count;
        sample_pressure();
        return nullptr;
    }

    void put(net_context* ctx) noexcept {
        std::lock_guard<std::mutex> guard{_mutex};
        auto it = _owners.find(ctx);
        if (it == _owners.end()) {
            return;
        }

        auto* chunk = it->second;
        if (chunk->in_use > 0) {
            --chunk->in_use;
        }
        if (_in_use > 0) {
            --_in_use;
        }
        ++_free_count;
        chunk->pool->put(ctx);
    }

    size_t capacity() const noexcept {
        std::lock_guard<std::mutex> guard{_mutex};
        return _capacity;
    }

    size_t size() const noexcept {
        std::lock_guard<std::mutex> guard{_mutex};
        return _free_count;
    }

    size_t element_size() const noexcept {
        return _element_size;
    }

    void free() noexcept {
        if (_shrink_poller) {
            ::spdk_poller_unregister(&_shrink_poller);
        }

        std::lock_guard<std::mutex> guard{_mutex};
        if (_is_free) {
            return;
        }
        _is_free = true;

        if (_in_use != 0) {
            SPDK_ERRLOG(
              "ring write pool %s free while %zu contexts still in use\n",
              _name.c_str(),
              _in_use);
        }

        for (auto& chunk : _chunks) {
            if (chunk->pool) {
                chunk->pool->free();
            }
        }
        _owners.clear();
        _chunks.clear();
        _capacity = 0;
        _free_count = 0;
        _in_use = 0;
    }

private:
    struct chunk_type {
        explicit chunk_type(std::unique_ptr<pool_type> p, size_t c)
          : pool{std::move(p)}, capacity{c} {}

        std::unique_ptr<pool_type> pool;
        size_t capacity{0};
        size_t in_use{0};
    };

    static constexpr uint64_t sample_window{1024};
    static constexpr uint32_t grow_windows{3};
    static constexpr uint32_t grow_fail_percent{10};
    static constexpr size_t grow_step{128};
    static constexpr uint32_t shrink_usage_percent{30};
    static constexpr auto shrink_duration = std::chrono::seconds{30};

    static int shrink_poller(void* arg) {
        auto* pool = reinterpret_cast<ring_write_pool*>(arg);
        std::lock_guard<std::mutex> guard{pool->_mutex};
        pool->maybe_shrink();
        return SPDK_POLLER_IDLE;
    }

    void add_chunk(size_t chunk_capacity) {
        auto pool = std::make_unique<pool_type>(
          _pd,
          FB_FMT_2("%1%_%2%", _name, _chunks.size()),
          chunk_capacity,
          _element_size,
          0,
          _sock_id);

        auto chunk = std::make_unique<chunk_type>(std::move(pool), chunk_capacity);
        std::vector<net_context*> contexts;
        contexts.reserve(chunk_capacity);
        for (size_t i = 0; i < chunk_capacity; ++i) {
            auto* ctx = chunk->pool->get();
            if (!ctx) {
                break;
            }
            _owners[ctx] = chunk.get();
            contexts.push_back(ctx);
        }
        for (auto* ctx : contexts) {
            chunk->pool->put(ctx);
        }

        _capacity += chunk_capacity;
        _free_count += chunk_capacity;
        _chunks.push_back(std::move(chunk));
    }

    void sample_pressure() noexcept {
        if (_get_count % sample_window != 0) {
            return;
        }

        if (_window_get_fail_count * 100 >= sample_window * grow_fail_percent) {
            ++_fail_windows;
        } else {
            _fail_windows = 0;
        }

        if (_capacity > 0 && _in_use * 100 >= _capacity * 85) {
            ++_high_usage_windows;
        } else {
            _high_usage_windows = 0;
        }

        _window_get_fail_count = 0;
        if (_fail_windows >= grow_windows || _high_usage_windows >= grow_windows) {
            grow();
        }
    }

    void grow() noexcept {
        if (_capacity >= _max_capacity) {
            if (!_logged_max_capacity) {
                SPDK_INFOLOG(msg, "ring write pool reached max capacity %zu\n", _max_capacity);
                _logged_max_capacity = true;
            }
            _fail_windows = 0;
            _high_usage_windows = 0;
            return;
        }

        auto old_capacity = _capacity;
        auto target = std::min(_capacity + grow_step, _max_capacity);
        try {
            add_chunk(target - _capacity);
        } catch (const std::exception& e) {
            SPDK_ERRLOG(
              "ring write pool %s grow failed: %s\n",
              _name.c_str(),
              e.what());
            _fail_windows = 0;
            _high_usage_windows = 0;
            return;
        } catch (...) {
            SPDK_ERRLOG("ring write pool %s grow failed: unknown exception\n", _name.c_str());
            _fail_windows = 0;
            _high_usage_windows = 0;
            return;
        }
        SPDK_INFOLOG(msg, "ring write pool grow: %zu -> %zu\n", old_capacity, _capacity);
        _fail_windows = 0;
        _high_usage_windows = 0;
        _logged_max_capacity = false;
    }

    void maybe_shrink() noexcept {
        if (_capacity <= _min_capacity) {
            _low_usage_since.reset();
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (_capacity > 0 && _in_use * 100 <= _capacity * shrink_usage_percent) {
            if (!_low_usage_since.has_value()) {
                _low_usage_since = now;
                return;
            }
            if (now - *_low_usage_since >= shrink_duration) {
                shrink();
                _low_usage_since = now;
            }
        } else {
            _low_usage_since.reset();
        }
    }

    void shrink() noexcept {
        if (_chunks.empty() || _capacity <= _min_capacity) {
            return;
        }

        auto target = std::max(_min_capacity, _capacity / 2);
        auto& chunk = _chunks.back();
        if (chunk->in_use != 0 || _capacity - chunk->capacity < target) {
            return;
        }

        auto old_capacity = _capacity;
        for (auto it = _owners.begin(); it != _owners.end();) {
            if (it->second == chunk.get()) {
                it = _owners.erase(it);
            } else {
                ++it;
            }
        }
        _capacity -= chunk->capacity;
        _free_count -= chunk->capacity;
        chunk->pool->free();
        _chunks.pop_back();
        SPDK_INFOLOG(msg, "ring write pool shrink: %zu -> %zu\n", old_capacity, _capacity);
    }

private:
    ::ibv_pd* _pd{nullptr};
    std::string _name{};
    size_t _min_capacity{0};
    size_t _max_capacity{0};
    size_t _element_size{0};
    int _sock_id{SPDK_ENV_SOCKET_ID_ANY};

    mutable std::mutex _mutex{};
    ::spdk_poller* _shrink_poller{nullptr};
    bool _is_free{false};
    bool _logged_max_capacity{false};
    size_t _capacity{0};
    size_t _free_count{0};
    size_t _in_use{0};
    size_t _peak_in_use{0};
    uint64_t _get_count{0};
    uint64_t _get_fail_count{0};
    uint64_t _window_get_fail_count{0};
    uint32_t _fail_windows{0};
    uint32_t _high_usage_windows{0};
    std::optional<std::chrono::steady_clock::time_point> _low_usage_since{};

    std::vector<std::unique_ptr<chunk_type>> _chunks{};
    std::unordered_map<net_context*, chunk_type*> _owners{};
};

} // namespace rdma
} // namespace msg
