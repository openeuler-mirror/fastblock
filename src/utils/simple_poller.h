/* Copyright (c) 2024 ChinaUnicom
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

    simple_poller(::spdk_poller* p) : poller{p} {}

    simple_poller(const simple_poller&) = delete;

    simple_poller(simple_poller&& r) : poller{std::exchange(r.poller, nullptr)} {}

    simple_poller& operator=(const simple_poller&) = delete;

    simple_poller& operator=(simple_poller&&) = delete;

    ~simple_poller() noexcept {
        if (not poller) {
            return;
        }

        ::spdk_poller_unregister(&poller);
    }

    void unregister() noexcept {
        if (not poller) {
            return;
        }

        ::spdk_poller_unregister(&poller);
        poller = nullptr;
    }

    ::spdk_poller* poller{nullptr};
};

}
