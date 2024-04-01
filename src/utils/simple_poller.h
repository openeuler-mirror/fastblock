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

#include <spdk/thread.h>

#include <utility>

namespace utils {

struct simple_poller {

    simple_poller() = default;

    // simple_poller(::spdk_poller* p) : poller{p} {}

    simple_poller(const simple_poller&) = delete;

    simple_poller(simple_poller&& r)  
    : _poller{std::exchange(r._poller, nullptr)}
    , _thread_id{std::exchange(r._thread_id, 0)} {}

    simple_poller& operator=(const simple_poller&) = delete;

    simple_poller& operator=(simple_poller&&) = delete;

    ~simple_poller() noexcept {
        if (not _poller) {
            return;
        }

        unregister_poller();
    }

    static void unregister_poller(void *ctx){
        simple_poller* sp = (simple_poller *)ctx;
        ::spdk_poller_unregister(&sp->_poller);
        sp->_poller = nullptr;
        sp->_thread_id = 0;
    }

    void unregister_poller() noexcept {
        if (not _poller || _thread_id == 0) {
            return;
        }

        auto thread = spdk_get_thread();
        auto poller_thread = spdk_thread_get_by_id(_thread_id);
        if(!poller_thread)
            return;
        if(thread == poller_thread){
            unregister_poller(this);
        }else{
            spdk_thread_send_msg(poller_thread, simple_poller::unregister_poller, this);
        }
    }

    void register_poller(spdk_poller_fn fn, void *arg, uint64_t period_microseconds) {
        _poller = SPDK_POLLER_REGISTER(fn, arg, period_microseconds);
        _thread_id = spdk_thread_get_id(spdk_get_thread());
    }

private:
    ::spdk_poller* _poller{nullptr};
    uint64_t _thread_id;
};

}
