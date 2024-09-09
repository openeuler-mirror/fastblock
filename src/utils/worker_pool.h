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

#include "base/core_sharded.h"
#include "utils/simple_poller.h"

#include <spdk/thread.h>

#include <concepts>
#include <list>
#include <vector>

namespace fastblock {
namespace utils {

template<typename MsgType, typename State>
class worker_pool {
public:

    using message_handler_type = std::function<void(MsgType, State*)>;

private:

    struct worker_state {
        ::spdk_thread* thread{nullptr};
        std::string name{""};
        std::unique_ptr<::utils::simple_poller> poller{nullptr};
        std::list<MsgType> msg_queue{};
        State user_state{};
        message_handler_type handler{};
        worker_pool* this_pool{nullptr};
    };

    struct msg_context {
        MsgType msg{};
        worker_state* worker{nullptr};
    };

public:

    worker_pool() = delete;

    worker_pool(std::string name, const size_t n_threads, std::vector<core_sharded::core_id_type>& cores) {
        auto core_it = cores.begin();
        for (size_t i = 0; i < n_threads; ++i) {
            auto mask = core_sharded::make_cpumask(*core_it);
            auto thd_name = FMT_2("%1%_%2%", name, i);
            auto* thread = ::spdk_thread_create(thd_name.c_str(), mask.get());
            auto& w = _workers.emplace_back(thread, std::move(thd_name));
            w.this_pool = this;

            ++core_it;
            if (core_it == cores.end()) { core_it = cores.begin(); }
        }
        _workers.shrink_to_fit();
    }

    worker_pool(const worker_pool&) = delete;

    worker_pool(worker_pool&&) = default;

    worker_pool operator=(const worker_pool&) = delete;

    worker_pool& operator=(worker_pool&&) = default;

    ~worker_pool() noexcept = default;

    ::spdk_thread* operator[](size_t i) { return _workers[i].thread; }

public:

    /************************************************************
     * spdk_thread_send_msg callbacks' callbacks
     ************************************************************/

    static void on_stop(void* arg) {
        auto* worker = reinterpret_cast<worker_state*>(arg);
        worker->this_pool->handle_stop(worker);
    }

    static void on_message(void* arg) {
        auto* ctx = reinterpret_cast<msg_context*>(arg);
        try {
            ctx->worker->handler(ctx->msg, &ctx->worker->user_state);
        } catch (const std::exception& e) {
            SPDK_ERRLOG("Errror in handling worker message: %s\n", e.what());
            std::rethrow_exception(std::current_exception());
        }
        delete ctx;
    }

    /************************************************************
     * spdk_thread_send_msg callbacks
     ************************************************************/

    void handle_stop(worker_state* worker) {
        ::spdk_thread_exit(worker->thread);
        SPDK_NOTICELOG("worker %s stopped\n", worker->name.c_str());
    }

public:

    size_t size() { return _workers.size(); }

    void stop() {
        if (_is_termianted) { return; }
        _is_termianted = true;
        for (auto& worker : _workers) {
            ::spdk_thread_send_msg(worker.thread, on_stop, &worker);
        }
    }

    void register_handler(message_handler_type handler) {
        for (auto& worker : _workers) {
            worker.handler = handler;
        }
    }

    void set_state(State s) {
        for (auto& worker : _workers) {
            worker.user_state = std::move(s);
        }
    }

    auto send_message(const size_t index, MsgType msg) {
        auto* ctx = new msg_context{msg, &_workers[index]};
        return ::spdk_thread_send_msg(ctx->worker->thread, on_message, ctx);
    }

    auto send_message(const size_t index, MsgType&& msg) {
        auto* ctx = new msg_context{std::move(msg), &_workers[index]};
        return ::spdk_thread_send_msg(ctx->worker->thread, on_message, ctx);
    }

    State* get_state(const size_t index) noexcept {
        return &_workers[index].user_state;
    }

private:

    bool _is_termianted{false};
    std::vector<worker_state> _workers{};
};

} // namespace utils
} // namespace fastblock
