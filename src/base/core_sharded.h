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

#include "utils/fmt.h"

#include <utils/log.h>

#include <memory>
#include <vector>
#include <string>
#include <tuple>
#include <limits>

#include <spdk/stdinc.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/thread.h>
#include <spdk/util.h>
#include <spdk/log.h>

class core_sharded;
namespace {
std::unique_ptr<core_sharded> g_core_sharded{nullptr};
}


class core_context {
protected:
    virtual void run_task() = 0;
public:
    core_context() {}
    virtual ~core_context() {}
    static void run(void *arg) {
        core_context *con = (core_context *)arg;
        con->run_task();
        delete con;
    }
};

template <typename Func, typename... Args>
class lambda_ctx : public core_context {
public:
  lambda_ctx(lambda_ctx* l) = delete;
  lambda_ctx(Func func, Args&&... args)
  : func(std::move(func))
  , args(std::make_tuple(std::forward<Args>(args)...))
  { }

  virtual void run_task() override {
    std::apply(func, args);
  }

private:
  Func func;
  std::tuple<Args...> args;
};

class core_sharded{

public:

    using core_id_type = uint32_t;
    using core_container_type = std::vector<core_id_type>;
    using xcore_iterator = core_container_type::iterator;

public:

    struct system {
        using size_type = uint32_t;

        static core_id_type first_core() noexcept {
            return ::spdk_env_get_first_core();
        }

        static core_id_type next_core(const core_id_type core) noexcept {
            return ::spdk_env_get_next_core(core);
        }

        static core_id_type last_core() noexcept {
            return ::spdk_env_get_last_core();
        }

        static size_type capacity() noexcept {
            return ::spdk_env_get_core_count();
        }

        class core_iterator {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = core_id_type;
            using difference_type = std::ptrdiff_t;
            using pointer = core_id_type*;
            using reference = core_id_type&;

            core_iterator() = default;

            core_iterator(const value_type core) : _core{core} {}

            core_iterator(const core_iterator& it) = default;

            core_iterator& operator=(const core_iterator& it) = default;

            core_iterator(core_iterator&&) = default;

            core_iterator& operator=(core_iterator&&) = default;

            reference operator*() { return _core; }
            pointer operator->() { return &_core; }

            core_iterator& operator++() {
                _core = next_core(_core);
                return *this;
            }

            core_iterator operator++(int) {
                core_iterator temp(*this);
                _core = next_core(_core);
                return temp;
            }

            friend bool operator==(const core_iterator& lhs, const core_iterator& rhs) {
                return lhs._core == rhs._core;
            }

            friend bool operator!=(const core_iterator& lhs, const core_iterator& rhs) {
                return !(lhs == rhs);
            }

        private:

            value_type _core{};
        };

        static core_iterator begin() {
            return core_iterator{first_core()};
        }

        static core_iterator end() {
            return core_iterator{UINT32_MAX};
        }
    };

public:

    core_sharded() = delete;

    core_sharded(system::core_iterator begin, system::size_type n_core, std::string app_name) {
        system::size_type counter{0};
        ::spdk_cpuset cpumask{};
        ::spdk_thread* thread{nullptr};

        while (counter < n_core) {
            _shard_cores.push_back(*begin);
            ::spdk_cpuset_zero(&cpumask);
            ::spdk_cpuset_set_cpu(&cpumask, *begin, true);
            auto thread_name = FMT_2("%1%%2%", app_name, *begin);
            thread = ::spdk_thread_create(thread_name.c_str(), &cpumask);
            _threads.push_back(thread);
            SPDK_NOTICELOG_EX("Created public spdk thread on core %d\n", *begin);

            ++counter;
            ++begin;
        }
    }

    core_sharded(const core_sharded&) = delete;

    core_sharded(core_sharded&&) = delete;

    core_sharded& operator=(const core_sharded&) = delete;

    core_sharded& operator=(core_sharded&&) = delete;

    ~core_sharded() noexcept = default;

public:

    static void construct(auto&&... args) {
        g_core_sharded = std::make_unique<core_sharded>(std::forward<decltype(args)>(args)...);
    }

    [[gnu::optimize("O0")]] static core_sharded& get_core_sharded() noexcept {
        return *g_core_sharded;
    }

    static core_container_type get_shard_cores() {
        return g_core_sharded->shard_cores();
    }

    static auto make_cpumake(const core_id_type core) noexcept {
        auto cpumask = std::make_unique<::spdk_cpuset>();
        ::spdk_cpuset_zero(cpumask.get());
        ::spdk_cpuset_set_cpu(cpumask.get(), core, true);

        return cpumask;
    }

public:

    template <typename Func, typename... Args>
    int invoke_on(uint32_t shard_id, Func func, Args&&... args){
        auto lambda = new lambda_ctx(std::move(func), std::forward<Args>(args)...);

        uint32_t core = _shard_cores[shard_id];
        auto cur_thread = spdk_get_thread();
        if(core == spdk_env_get_current_core() && cur_thread == _threads[shard_id]){
            core_context::run((void *)lambda);
            return 0;
        }else
            return spdk_thread_send_msg(_threads[shard_id], &core_context::run, (void *)lambda);
    }

    uint32_t count(){
        return _shard_cores.size();
    }

    uint32_t this_shard_id() {
        for(uint32_t id = 0; id < _shard_cores.size(); id++) {
            if (_shard_cores[id] == spdk_env_get_current_core()) {
                return id;
            }
        }
        return std::numeric_limits<uint32_t>::max();
    }

    void stop() noexcept {
        auto current_thread = spdk_get_thread();

        for (auto* thread : _threads) {
            if (not thread) { continue; }
            ::spdk_set_thread(thread);
            ::spdk_thread_exit(thread);
            if(current_thread == thread){
                ::spdk_set_thread(nullptr);
            }else{
                ::spdk_set_thread(current_thread);
            }
        }
    }

    core_container_type shard_cores() noexcept {
        return _shard_cores;
    }

private:

    core_container_type _shard_cores;
    std::vector<::spdk_thread *> _threads;
};
