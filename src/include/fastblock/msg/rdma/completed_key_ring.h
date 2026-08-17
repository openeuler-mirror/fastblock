/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace msg {
namespace rdma {

// Recently completed correlation keys. Late / duplicate fragments that
// match a remembered key are dropped quietly (not unknown_key).
class completed_key_ring {
public:
    static constexpr size_t default_capacity{4096};

    explicit completed_key_ring(size_t cap = default_capacity)
      : _cap{cap == 0 ? default_capacity : cap}
      , _ring(_cap, 0) {}

    void remember(uint64_t key) noexcept {
        if (key == 0 or _cap == 0) {
            return;
        }
        const auto old = _ring[_head];
        if (old != 0) {
            _set.erase(old);
        }
        _ring[_head] = key;
        _set.insert(key);
        _head = (_head + 1) % _cap;
    }

    bool contains(uint64_t key) const noexcept {
        return key != 0 && _set.find(key) != _set.end();
    }

    size_t size() const noexcept { return _set.size(); }

private:
    size_t _cap;
    size_t _head{0};
    std::vector<uint64_t> _ring;
    std::unordered_set<uint64_t> _set;
};

} // namespace rdma
} // namespace msg
