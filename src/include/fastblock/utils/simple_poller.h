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

#include <spdk/log.h>
#include <spdk/thread.h>
#include <optional>

#include <utility>

namespace utils {

class simple_poller {

public:

    struct register_context {
        simple_poller* this_poller{nullptr};
        ::spdk_poller_fn fn{nullptr};
        void* arg{nullptr};
        uint64_t period_microseconds{0};
        std::optional<std::string> name{std::nullopt};
    };

    struct unregister_context {
        simple_poller* this_poller{nullptr};
        std::optional<std::function<void()>> cb{std::nullopt};
    };

public:

    simple_poller() : _thread{::spdk_get_thread()} {}

    simple_poller(::spdk_thread* thread) : _thread{thread} {}

    simple_poller(const simple_poller&) = delete;

    simple_poller(simple_poller&& r)
      : _poller{std::exchange(r._poller, nullptr)}
      , _thread{std::exchange(r._thread, nullptr)} {}

    simple_poller& operator=(const simple_poller&) = delete;

    simple_poller& operator=(simple_poller&&) = delete;

    ~simple_poller() noexcept {
        if (not _poller) {
            return;
        }

        unregister_poller();
    }

public:

    static void handle_unregister(void *arg) noexcept {
        auto* ctx = reinterpret_cast<unregister_context*>(arg);
        ::spdk_poller_unregister(&(ctx->this_poller->_poller));
        ctx->this_poller->_poller = nullptr;
        SPDK_NOTICELOG("unregistered the poller\n");
        if (ctx->cb) {
            try {
                (ctx->cb.value())();
            } catch (const std::exception& e) {
                SPDK_ERRLOG("unregister_poller call user callback error: %s\n", e.what());
            }
        }

        delete ctx;
    }

    static void handle_register(void* arg) noexcept {
        auto* ctx = reinterpret_cast<register_context*>(arg);
        if (ctx->name) {
            ctx->this_poller->_poller =
              ::spdk_poller_register_named(ctx->fn, ctx->arg, ctx->period_microseconds, ctx->name.value().c_str());
        } else {
            ctx->this_poller->_poller = SPDK_POLLER_REGISTER(ctx->fn, ctx->arg, ctx->period_microseconds);
        }
        delete ctx;
    }

public:

    int unregister_poller(std::optional<std::function<void()>>&& cb = std::nullopt) noexcept {
        if (not _poller) {
            return 0;
        }

        auto* ctx = new unregister_context{this, std::move(cb)};
        return ::spdk_thread_send_msg(_thread, simple_poller::handle_unregister, ctx);
    }

    int register_poller(::spdk_poller_fn fn, void *arg, uint64_t period_microseconds, std::optional<std::string> name = std::nullopt) {
        auto* ctx = new register_context{this, fn, arg, period_microseconds, name};
        return ::spdk_thread_send_msg(_thread, simple_poller::handle_register, ctx);
    }

    void set_thread(::spdk_thread* thread) noexcept {
        _thread = thread;
    }

private:
    ::spdk_poller* _poller{nullptr};
    ::spdk_thread* _thread{nullptr};
};

}
