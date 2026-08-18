/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace msg {
namespace sock {

class process_quota {
public:
    static process_quota& instance() noexcept;

    void set_limit(size_t bytes) noexcept { _limit.store(bytes, std::memory_order_relaxed); }

    size_t limit() const noexcept { return _limit.load(std::memory_order_relaxed); }

    size_t used() const noexcept { return _used.load(std::memory_order_relaxed); }

    bool try_reserve(size_t n) noexcept {
        if (n == 0) {
            return true;
        }
        auto cur = _used.load(std::memory_order_relaxed);
        const auto lim = _limit.load(std::memory_order_relaxed);
        while (true) {
            if (lim != 0 && cur + n > lim) {
                return false;
            }
            if (_used.compare_exchange_weak(cur, cur + n, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    void release(size_t n) noexcept {
        if (n == 0) {
            return;
        }
        _used.fetch_sub(n, std::memory_order_acq_rel);
    }

private:
    std::atomic<size_t> _used{0};
    std::atomic<size_t> _limit{0};
};

} // namespace sock
} // namespace msg
