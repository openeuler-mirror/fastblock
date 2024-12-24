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

#include "fastblock/msg/rdma/pd.h"
#include "fastblock/utils/fmt.h"


#include <spdk/env.h>
#include <spdk/string.h>

#include <csignal>
#include <list>
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
      const size_t cache_size = SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
      const int sock_id = SPDK_ENV_SOCKET_ID_ANY)
      : _capacity{capacity}
      , _element_size{element_size}
      , _name{name} {
        size_t i{0};
        _data_ptr = ::spdk_zmalloc(
          _element_size * _capacity,
          0x1000,
          nullptr,
          SPDK_ENV_LCORE_ID_ANY,
          SPDK_MALLOC_DMA);

        if (not _data_ptr) {
            throw std::runtime_error{"allocate data mem failed"};
        }

        _net_ctx_ptr = (net_context*)::spdk_zmalloc(
          sizeof(net_context) * _capacity,
          0x1000,
          nullptr,
          SPDK_ENV_LCORE_ID_ANY,
          SPDK_MALLOC_DMA);

        if (not _net_ctx_ptr) {
            throw std::runtime_error{"allocate net_context mem failed"};
        }

        auto* data_byte_ptr = reinterpret_cast<uint8_t*>(_data_ptr);
        net_context* ctx_ptr{nullptr};
        for (i = 0; i < _capacity; ++i) {
            auto* mr = ::ibv_reg_mr(
              pd, data_byte_ptr, _element_size,
              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
            data_byte_ptr += _element_size;

            if (not mr) {
                SPDK_ERRLOG(
                  "ERROR: Call ibv_reg_mr() failed, errno is %d(%s)\n",
                  errno, std::strerror(errno));
                std::raise(SIGABRT);
            }

            ctx_ptr = &(_net_ctx_ptr[i]);
            ctx_ptr->mr = mr;
            ctx_ptr->sge.addr = uint64_t(mr->addr);
            ctx_ptr->sge.length = mr->length;
            ctx_ptr->sge.lkey = mr->lkey;
            ctx_ptr->wr.num_sge = 1;
            ctx_ptr->wr.sg_list = &(ctx_ptr->sge);

            _net_context_list.push_back(ctx_ptr);
        }
    }

    memory_pool(const memory_pool&) = delete;

    memory_pool(memory_pool&&) = delete;

    memory_pool& operator=(const memory_pool&) = delete;

    memory_pool& operator=(memory_pool&&) = delete;

    ~memory_pool() noexcept {
        free();
    }

public:

    net_context* get() noexcept {
        if (_net_context_list.empty()) {
            return nullptr;
        }

        auto* ret = _net_context_list.front();
        _net_context_list.pop_front();
        return ret;
    }

    void put(net_context* ctx) noexcept {
        _net_context_list.push_back(ctx);
    }

    std::unique_ptr<net_context*[]> get_bulk(const size_t n) noexcept {
        if (n > _capacity) {
            return nullptr;
        }

        auto ctxs = std::make_unique<net_context*[]>(n);
        auto it = _net_context_list.begin();
        for (size_t i{0}; i < _net_context_list.size(); ++i) {
            ctxs[i] = *it;
            ++it;
        }

        return ctxs;
    }

    void put_bulk(std::unique_ptr<net_context*[]> ctxs, const size_t n) noexcept {
        return;
    }

    size_t size() noexcept {
        return _net_context_list.size();
    }

    [[gnu::always_inline]] size_t element_size() noexcept {
        return _element_size;
    }

    void free() noexcept {
        if (_is_free) {
            return;
        }

        _is_free = true;

        SPDK_INFOLOG(msg, "Start closing rpc memory_pool %s\n", _name.c_str());
        if (this->_net_ctx_ptr) {
            for (auto* ctx : _net_context_list) {
                ::ibv_dereg_mr(ctx->mr);
            }
            SPDK_INFOLOG(msg, "Deregistered all memory region\n");
            ::spdk_free(_net_ctx_ptr);
        }

        if (_data_ptr) {
            ::spdk_free(_data_ptr);
        }
        SPDK_INFOLOG(msg, "The memory pool %s has been freed\n", _name.c_str());
    }

private:

    size_t _capacity{0};
    size_t _element_size{0};
    std::string _name{};
    bool _is_free{false};
    void* _data_ptr{nullptr};
    net_context* _net_ctx_ptr{nullptr};
    std::list<net_context*> _net_context_list{};
};

} // namespace rdma
} // namespace msg
