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

#include <cstdint>
#include <list>

#include "fastblock/utils/utils.h"

#ifndef FB_OP_TYPE_LOCK_DEBUG
#include "spdk/log.h"
#define FB_OP_TYPE_LOCK_DEBUG(...) SPDK_DEBUGLOG(osd, __VA_ARGS__)
#endif

// Allows READ-READ, or WRITE-WRITE.
// But exclusive on different op_type, like READ-WRITE or WRITE-READ.
// Thus, two or more WRITE can be replicated without blocking.
//       Two or more READ can be applied without blocking.
// But READ-WRITE-READ or WRITE-READ-WRITE will be applied sequencially.
// One op shouldn't unlock until everything done, and exception should be handled properly between lock/unlock.
//
// Waiter wakeup is iterative: unlock() must not re-enter wake() if the
// granted waiter's complete() immediately fails and unlocks again
// (nvmf mkfs / -ENOMEM reads). pop the waiter before complete() so a
// nested unlock cannot wake the same context twice (UAF + stack overflow).
template<typename op_type>
class op_type_excl_lock {
public:
    op_type_excl_lock()
    : _lock_type(op_type::NONE)
    , _runners(0)
    , _waking(false) {
        _waiters.clear();
    };

    op_type_excl_lock(op_type_excl_lock&& o) = default;
    op_type_excl_lock& operator=(op_type_excl_lock&&) = default;

    // Move-only.
    op_type_excl_lock(const op_type_excl_lock&) = delete;
    void operator=(const op_type_excl_lock&) = delete;

    void lock(const op_type type, utils::context *complete) {
        FB_OP_TYPE_LOCK_DEBUG("enter lock type %u\n", (uint32_t)type);

        if (try_lock(type)) {
            FB_OP_TYPE_LOCK_DEBUG("got lock type %u\n", (uint32_t)type);
            _runners++;
            complete->complete(0);
            return;
        }

        FB_OP_TYPE_LOCK_DEBUG("wait lock type %u\n", (uint32_t)type);

        // Wait.
        _waiters.emplace_back(type, complete);
    }

    void unlock(const op_type type) {
        FB_OP_TYPE_LOCK_DEBUG("enter unlock type %u\n", (uint32_t)type);

        _runners--;

        if (0 == _runners) {
            _lock_type = op_type::NONE;
        }

        if (_waking) {
            return;
        }
        _waking = true;
        wake();
        _waking = false;
    }

    // holders includes all the running and waiting workers.
    uint64_t holders() const {
        return _runners + _waiters.size();
    }

private:
    // TODO: range lock
    op_type   _lock_type;   // Current lock type. Each type is exclusive. NONE is compatible with either type.
    uint64_t  _runners;
    bool      _waking;

    struct waiter {
        waiter(op_type _type, utils::context *_complete)
        : complete(_complete)
        , type(_type) {}

        utils::context *complete;
        op_type type;
    };

    std::list<waiter> _waiters;   // FIFO. A list for all the (non-running) waiters.


    static inline bool is_none(const op_type type) {
        return type == op_type::NONE;
    }

    static inline bool is_compatible_type(const op_type type1, const op_type type2) {
        if (is_none(type1) || is_none(type2)) {
            return true;
        }

        return type1 == type2;
    }

    bool try_lock(const op_type type) noexcept {
        FB_OP_TYPE_LOCK_DEBUG("enter try_lock type %u, current type %u\n", (uint32_t)type, (uint32_t)_lock_type);

        // If running type is the same, and there is no waiters, just return true.
        if (is_none(_lock_type)   // The first try to get log.
                || (is_compatible_type(type, _lock_type) && _waiters.empty()) // Someone else got the lock, with the same op type.
                ) {
            if (is_none(_lock_type)) {
                // The first one, update _lock_type to our type.
                _lock_type = type;
            }

            return true;
        }

        return false;
    }

    void wake() {
        FB_OP_TYPE_LOCK_DEBUG("enter lock wake, current type %u\n", (uint32_t)_lock_type);

        // Try to wake front waiters.
        while (!_waiters.empty()) {
            auto& w = _waiters.front();

            if (!try_lock(w.type)) {
                FB_OP_TYPE_LOCK_DEBUG("lock wake->break type %u, current type %u\n", (uint32_t)w.type, (uint32_t)_lock_type);
                break;
            }

            _runners++;
            utils::context* complete = w.complete;
            _waiters.pop_front();
            // May unlock synchronously (failed read/write). Nested unlock
            // sees _waking and returns; this loop drains the rest.
            complete->complete(0);
        }

        FB_OP_TYPE_LOCK_DEBUG("leave lock wake, current type %u, runners %lu, _waiters size %lu\n",
                            (uint32_t)_lock_type, _runners, _waiters.size());
    }
};
