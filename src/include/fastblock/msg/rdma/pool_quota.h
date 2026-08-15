/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>

namespace msg {
namespace rdma {

enum class alloc_class : uint8_t {
    priority = 1,
    reserved,
    bulk
};

struct pool_quota {
    size_t reserved_priority{4};
    size_t reserved_per_connection{2};
    size_t n_connections{0};
    size_t low_watermark{0};
    size_t high_watermark{0};

    size_t reserved_floor() const noexcept {
        return reserved_priority + reserved_per_connection * n_connections;
    }
};

inline void apply_default_pool_quota(pool_quota& q, const size_t capacity) noexcept {
    if (capacity == 0) {
        q = {};
        return;
    }
    if (q.reserved_priority == 0) {
        q.reserved_priority = std::min<size_t>(4, capacity);
    }
    if (q.reserved_per_connection == 0) {
        q.reserved_per_connection = std::min<size_t>(2, capacity);
    }
    if (q.low_watermark == 0) {
        q.low_watermark = std::max<size_t>(q.reserved_priority + 4, capacity / 16);
        if (q.low_watermark > capacity) {
            q.low_watermark = capacity / 2 == 0 ? 1 : capacity / 2;
        }
    }
    if (q.high_watermark == 0) {
        q.high_watermark = std::min(
          capacity, std::max(q.low_watermark * 2, q.low_watermark + 8));
    }
    if (q.high_watermark < q.low_watermark) {
        q.high_watermark = q.low_watermark;
    }
}

inline void update_bulk_gate(
  const size_t free_n,
  const pool_quota& q,
  bool* bulk_allowed,
  uint64_t* watermark_hits) noexcept {
    if (bulk_allowed == nullptr) {
        return;
    }
    if (q.low_watermark > 0 && free_n < q.low_watermark) {
        if (*bulk_allowed && watermark_hits != nullptr) {
            ++(*watermark_hits);
        }
        *bulk_allowed = false;
        return;
    }
    if (q.high_watermark == 0 || free_n >= q.high_watermark) {
        *bulk_allowed = true;
    }
}

inline bool can_alloc(
  const size_t free_n,
  const size_t n,
  const alloc_class cls,
  const pool_quota& q,
  const bool bulk_allowed) noexcept {
    if (n == 0 || n > free_n) {
        return false;
    }
    switch (cls) {
    case alloc_class::priority:
        return true;
    case alloc_class::reserved:
        return (free_n - n) >= q.reserved_priority;
    case alloc_class::bulk:
        return bulk_allowed && (free_n - n) >= q.reserved_floor();
    }
    return false;
}

template <typename T>
std::unique_ptr<T*[]> freelist_take(std::list<T*>& list, const size_t n) {
    if (n == 0 || n > list.size()) {
        return nullptr;
    }
    auto out = std::make_unique<T*[]>(n);
    for (size_t i{0}; i < n; ++i) {
        out[i] = list.front();
        list.pop_front();
    }
    return out;
}

template <typename T>
void freelist_put(std::list<T*>& list, T* const* ctxs, const size_t n) {
    if (ctxs == nullptr) {
        return;
    }
    for (size_t i{0}; i < n; ++i) {
        if (ctxs[i] != nullptr) {
            list.push_back(ctxs[i]);
        }
    }
}

} // namespace rdma
} // namespace msg
