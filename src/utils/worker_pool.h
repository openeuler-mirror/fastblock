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

    // static void on_start(void* arg) {
    //     auto* worker = reinterpret_cast<worker_state*>(arg);
    //     worker->this_pool->handle_start(worker);
    // }

    static void on_stop(void* arg) {
        auto* worker = reinterpret_cast<worker_state*>(arg);
        worker->this_pool->handle_stop(worker);
    }

    static void on_message(void* arg) {
        auto* ctx = reinterpret_cast<msg_context*>(arg);
        ctx->worker->handler(ctx->msg, &ctx->worker->user_state);
        // ctx->worker->msg_queue.emplace_back(std::move(ctx->msg));
        delete ctx;
    }

    static int message_poller(void* arg) {
        auto* worker = reinterpret_cast<worker_state*>(arg);
        return worker->this_pool->handle_message(worker);
    }

    /************************************************************
     * spdk_thread_send_msg callbacks
     ************************************************************/

    // void handle_start(worker_state* worker) {
    //     // worker->poller = std::make_unique<::utils::simple_poller>(worker->thread);
    //     // worker->poller->register_poller(message_poller, worker);
    //     SPDK_NOTICELOG("worker %s started\n", worker->name.c_str());
    // }

    void handle_stop(worker_state* worker) {
        // worker->poller->unregister_poller();
        ::spdk_thread_exit(worker->thread);
        SPDK_NOTICELOG("worker %s stopped\n", worker->name.c_str());
    }

    int handle_message(worker_state* worker) {
        if (_is_termianted or worker->msg_queue.empty()) {
            return SPDK_POLLER_IDLE;
        }

        auto& msg = worker->msg_queue.front();
        try {
            worker->handler(std::move(msg), &worker->user_state);
        } catch (const std::exception& e) {
            SPDK_ERRLOG("Errror in handling worker message: %s\n", e.what());
        }
        worker->msg_queue.pop_front();

        return SPDK_POLLER_BUSY;
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

    // void start() {
    //     if (_is_started) { return; }
    //     _is_started = true;
    //     for (auto& worker : _workers) {
    //         ::spdk_thread_send_msg(worker.thread, on_start, &worker);
    //     }
    // }

    void register_handler(message_handler_type&& handler) {
        for (auto& worker : _workers) {
            worker.handler = std::move(handler);
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

    // bool _is_started{false};
    bool _is_termianted{false};
    std::vector<worker_state> _workers{};
};

} // namespace utils
} // namespace fastblock
