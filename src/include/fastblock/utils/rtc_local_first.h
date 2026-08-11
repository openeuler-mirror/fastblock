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

#include "fastblock/base/core_sharded.h"

#include <spdk/log.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>

/**
 * RTC local-first dispatch (PR-2).
 * Inline only when shard id matches AND current SPDK thread is home shard thread
 * (same condition as core_sharded::invoke_on). Otherwise invoke_on.
 */
namespace rtc {

enum class hop_kind : uint8_t {
    client_data = 0,
    raft_replica = 1,
    leader_lookup = 2,
};

struct metrics {
    std::atomic<uint64_t> local_hits{0};
    std::atomic<uint64_t> cross_core_hops{0};
    std::atomic<uint64_t> thread_hops{0};
    std::atomic<uint64_t> leader_lookup_hops{0};
    std::atomic<uint64_t> raft_cross_core_hops{0};
};

inline metrics& global_metrics() noexcept {
    static metrics m;
    return m;
}

inline bool local_process_enabled() noexcept {
    static const bool enabled = [] {
        const char* v = std::getenv("FASTBLOCK_RTC_LOCAL_PROCESS");
        if (!v || v[0] == '\0') {
            return true; // default on
        }
        return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
    }();
    return enabled;
}

/** Pure predicate (testable without SPDK when stubs supplied). */
inline bool is_on_home_spdk_thread_pred(
  const uint32_t this_shard,
  const uint32_t home_shard,
  const bool same_spdk_thread) noexcept {
    return this_shard == home_shard
        && this_shard != std::numeric_limits<uint32_t>::max()
        && same_spdk_thread;
}

inline bool is_on_home_spdk_thread(const uint32_t home_shard) noexcept {
    auto& cs = core_sharded::get_core_sharded();
    const uint32_t self = cs.this_shard_id();
    if (self != home_shard || self == std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    // get_thread index is shard index (see invoke_on)
    return spdk_get_thread() == core_sharded::get_thread(home_shard);
}

inline bool is_on_some_osd_rpc_spdk_thread() noexcept {
    auto& cs = core_sharded::get_core_sharded();
    const uint32_t self = cs.this_shard_id();
    if (self == std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    return spdk_get_thread() == core_sharded::get_thread(self);
}

inline void rate_limited_hop_log(
  const hop_kind kind,
  const uint32_t home,
  const uint32_t self) noexcept {
    // Token bucket: allow ~1 log / 1024 hops per process.
    static std::atomic<uint64_t> tokens{0};
    const uint64_t n = tokens.fetch_add(1, std::memory_order_relaxed);
    if ((n & 1023ULL) != 0) {
        return;
    }
    const char* k =
      kind == hop_kind::client_data     ? "client_data" :
      kind == hop_kind::raft_replica    ? "raft_replica" :
                                          "leader_lookup";
    SPDK_NOTICELOG(
      "RTC hop kind=%s home_shard=%u self_shard=%u (sampled)\n",
      k,
      home,
      self);
}

/**
 * Run work on home shard with local-first (K17).
 * Never strict-rejects (strict is PR-7 / data plane only).
 */
template <typename Func>
inline int local_first(
  core_sharded& shard,
  const uint32_t home,
  Func&& run,
  const hop_kind kind) {
    if (!local_process_enabled()) {
        return shard.invoke_on(home, std::forward<Func>(run));
    }

    if (is_on_home_spdk_thread(home)) {
        global_metrics().local_hits.fetch_add(1, std::memory_order_relaxed);
        std::forward<Func>(run)();
        return 0;
    }

    auto& m = global_metrics();
    const uint32_t self = core_sharded::get_core_sharded().this_shard_id();

    if (kind == hop_kind::leader_lookup) {
        m.leader_lookup_hops.fetch_add(1, std::memory_order_relaxed);
    } else if (kind == hop_kind::raft_replica) {
        m.raft_cross_core_hops.fetch_add(1, std::memory_order_relaxed);
    }

    if (self != home && self != std::numeric_limits<uint32_t>::max()
        && is_on_some_osd_rpc_spdk_thread()) {
        m.cross_core_hops.fetch_add(1, std::memory_order_relaxed);
        rate_limited_hop_log(kind, home, self);
    } else {
        m.thread_hops.fetch_add(1, std::memory_order_relaxed);
    }

    return shard.invoke_on(home, std::forward<Func>(run));
}

inline void maybe_log_metrics_notice() noexcept {
    static std::atomic<uint64_t> last_log{0};
    auto& m = global_metrics();
    const uint64_t local = m.local_hits.load(std::memory_order_relaxed);
    const uint64_t cross = m.cross_core_hops.load(std::memory_order_relaxed);
    const uint64_t thr = m.thread_hops.load(std::memory_order_relaxed);
    const uint64_t leader = m.leader_lookup_hops.load(std::memory_order_relaxed);
    const uint64_t raft = m.raft_cross_core_hops.load(std::memory_order_relaxed);
    const uint64_t total = local + cross + thr;
    // Log every ~4096 events
    if (total == 0 || (total & 4095ULL) != 0) {
        return;
    }
    const uint64_t prev = last_log.exchange(total, std::memory_order_relaxed);
    if (prev == total) {
        return;
    }
    const double hop_rate =
      total > 0 ? (100.0 * static_cast<double>(cross + thr) / static_cast<double>(total)) : 0.0;
    SPDK_NOTICELOG(
      "RTC metrics: local=%lu cross_core=%lu thread_hop=%lu leader_lookup_hop=%lu "
      "raft_hop=%lu hop_rate=%.3f%%\n",
      static_cast<unsigned long>(local),
      static_cast<unsigned long>(cross),
      static_cast<unsigned long>(thr),
      static_cast<unsigned long>(leader),
      static_cast<unsigned long>(raft),
      hop_rate);
}

} // namespace rtc
