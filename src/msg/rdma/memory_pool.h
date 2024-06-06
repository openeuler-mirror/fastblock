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

#include "msg/rdma/pd.h"
#include "utils/fmt.h"
#include "utils/log.h"

#include <spdk/env.h>
#include <spdk/string.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>

namespace msg {
namespace rdma {

template<typename work_request_type>
class memory_pool {

public:

    struct net_context {
        ::ibv_mr* mr{};
        work_request_type wr{};
        ::ibv_sge sge{};
    };

public:

    memory_pool() = delete;

    memory_pool(
      ::ibv_pd* pd,
      std::string name,
      const size_t capacity,
      const size_t element_size,
      const size_t cache_size = SPDK_MEMPOOL_DEFAULT_CACHE_SIZE)
      : _capacity{capacity}
      , _element_size{element_size}
      , _name{name} {
        _pool = ::spdk_mempool_create(
          _name.c_str(),
          _capacity,
          _element_size,
          cache_size,
          SPDK_ENV_SOCKET_ID_ANY);

        if (not _pool) {
            throw std::runtime_error{FMT_3(
              "create memory pool failed, name is %1%, capacity is %2%, element size is %3%",
              _name.c_str(), _capacity, _element_size)};
        }

        _net_contexts = ::spdk_mempool_create(
          FMT_1("%1%_n", _name).c_str(),
          _capacity,
          sizeof(net_context),
          cache_size,
          SPDK_ENV_SOCKET_ID_ANY);

        if (not _net_contexts) {
            throw std::runtime_error{FMT_3(
              "create net context memory pool failed, name is %1%, capacity is %2%, element size is %3%",
              FMT_1("%1%_n", _name).c_str(), _capacity, sizeof(net_context))};
        }

        init_with_rdma(pd);

        SPDK_DEBUGLOG_EX(
          msg,
          "created memory pool '%s' with capacity %ld\n",
          _name.c_str(), ::spdk_mempool_count(_pool));
    }

    memory_pool(const memory_pool&) = delete;

    memory_pool(memory_pool&&) = delete;

    memory_pool& operator=(const memory_pool&) = delete;

    memory_pool& operator=(memory_pool&&) = delete;

    ~memory_pool() noexcept {
        free();
    }

private:

    void init_with_rdma(::ibv_pd* pd) {
        auto ele_cache = std::make_unique<void*[]>(_capacity);
        auto rc = ::spdk_mempool_get_bulk(_pool, ele_cache.get(), _capacity);
        if (rc) {
            throw std::runtime_error{FMT_1("get mempool all elements error: %1%", rc)};
        }

        auto ctx_cache = std::make_unique<net_context*[]>(_capacity);
        rc = ::spdk_mempool_get_bulk(_net_contexts, reinterpret_cast<void**>(ctx_cache.get()), _capacity);
        if (rc) {
            throw std::runtime_error{FMT_1("get net context mempool all elements error: %1%", rc)};
        }

        for (size_t i{0}; i < _capacity; ++i) {
            std::memset(ele_cache[i], 0, _element_size);
            auto t = ele_cache[i];
            auto* mr = ::ibv_reg_mr(
              pd, t, _element_size,
              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);

            if (not mr) {
                SPDK_ERRLOG_EX(
                  "ERROR: Call ibv_reg_mr() failed, errno is %d(%s)\n",
                  errno, std::strerror(errno));
            }

            ctx_cache[i]->mr = mr;
            ctx_cache[i]->sge.addr = uint64_t(mr->addr);
            ctx_cache[i]->sge.length = mr->length;
            ctx_cache[i]->sge.lkey = mr->lkey;
            ctx_cache[i]->wr.num_sge = 1;
            ctx_cache[i]->wr.sg_list = &(ctx_cache[i]->sge);
        }
        ::spdk_mempool_put_bulk(_pool, ele_cache.get(), _capacity);
        ::spdk_mempool_put_bulk(_net_contexts, reinterpret_cast<void**>(ctx_cache.get()), _capacity);
    }

public:

    net_context* get() noexcept {
        if (::spdk_mempool_count(_net_contexts) == 0) {
            return nullptr;
        }

        return reinterpret_cast<net_context*>(::spdk_mempool_get(_net_contexts));
    }

    void put(net_context* ctx) noexcept {
        ::spdk_mempool_put(_net_contexts, ctx);
    }

    std::unique_ptr<net_context*[]> get_bulk(const size_t n) noexcept {
        if (n > _capacity) {
            return nullptr;
        }

        auto ctxs = std::make_unique<net_context*[]>(n);
        auto rc = ::spdk_mempool_get_bulk(_net_contexts, reinterpret_cast<void**>(ctxs.get()), n);
        if (rc) {
            return nullptr;
        }
        return ctxs;
    }

    void put_bulk(std::unique_ptr<net_context*[]> ctxs, const size_t n) noexcept {
        ::spdk_mempool_put_bulk(_net_contexts, reinterpret_cast<void**>(ctxs.get()), n);
    }

    size_t size() noexcept {
        return ::spdk_mempool_count(_net_contexts);
    }

    [[gnu::always_inline]] size_t element_size() noexcept {
        return _element_size;
    }

    void free() noexcept {
        if (_is_free) {
            return;
        }

        _is_free = true;

        SPDK_NOTICELOG_EX("Start closing rpc memory_pool %s\n", _name.c_str());
        if (_net_contexts) {
            auto ele_cache = std::make_unique<void*[]>(_capacity);
            auto rc = ::spdk_mempool_get_bulk(_net_contexts, ele_cache.get(), _capacity);
            if (rc == 0) {
                for (size_t i{0}; i < _capacity; ++i) {
                    auto* ctx = reinterpret_cast<net_context*>(ele_cache[i]);
                    ::ibv_dereg_mr(ctx->mr);
                }
                SPDK_NOTICELOG_EX("Deregistered all memory region\n");
                ::spdk_mempool_put_bulk(_net_contexts, ele_cache.get(), _capacity);
            } else {
                SPDK_ERRLOG_EX(
                  "ERROR: Got bulk of net contexts failed, return code is %s, current pool size is %ld, capacity is %lu\n",
                  ::spdk_strerror(rc),
                  ::spdk_mempool_count(_net_contexts),
                  _capacity);
            }

            ::spdk_mempool_free(_net_contexts);
        }

        if (_pool) {
            ::spdk_mempool_free(_pool);
        }
        SPDK_NOTICELOG_EX("The memory pool %s has been freed\n", _name.c_str());
    }

private:

    size_t _capacity{0};
    size_t _element_size{0};
    std::string _name{};
    ::spdk_mempool* _pool{nullptr};
    ::spdk_mempool* _net_contexts{nullptr};
    bool _is_free{false};
};

} // namespace rdma
} // namespace msg
