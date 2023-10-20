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
