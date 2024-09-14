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

#ifdef FASTBLOCK_COLLECT_DURATION

#include "utils/fmt.h"

#include <chrono>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

#define FASTBLOCK_DUR_MAP_CREATE(key_type, var, name) fastblock::utils::duration_map<key_type> var{name}
#define FASTBLOCK_DUR_MAP_EMPLACE_START(var, k) var.emplace_start(k)
#define FASTBLOCK_DUR_MAP_EMPLACE_END(var, k) var.emplace_end(k)
#define FASTBLOCK_DUR_MAP_EMPLACE_END_LAST(var) var.emplace_end()
#define FASTBLOCK_DUR_MAP_PRINT(var) var.print_result()

namespace fastblock {
namespace utils {

template <typename key_type>
class duration_map {

public:

    struct dur_pair {
        std::chrono::system_clock::time_point start_at{std::chrono::system_clock::now()};
        std::chrono::system_clock::time_point end_at{};
    };

public:

    duration_map() = default;

    duration_map(std::string name) : _name{name} {};

    duration_map(const duration_map&) = delete;

    duration_map(duration_map&&) = delete;

    duration_map& operator=(const duration_map&) = delete;

    duration_map& operator=(duration_map&&) = delete;

    ~duration_map() noexcept = default;

public:

    [[gnu::always_inline]] void emplace_start(key_type k) {
        _last_key = k;
        _durs.emplace(k, dur_pair{});
    }

    [[gnu::always_inline]] void emplace_end() {
        emplace_end(_last_key);
    }

    [[gnu::always_inline]] void emplace_end(key_type k) {
        auto it = _durs.find(k);
        it->second.end_at = std::chrono::system_clock::now();
    }

    void print_result() {
        std::vector<double> durations{};
        std::chrono::system_clock::duration dur{};
        for (auto& kv : _durs) {
            dur = kv.second.end_at - kv.second.start_at;
            durations.emplace_back(static_cast<double>(dur.count()));
        }

        double mean{0.0};
        for (double dur : durations) {
            mean += dur;
        }
        mean /= durations.size();

        double accum{0.0};
        std::for_each(
            durations.begin(), durations.end(),
            [&accum, mean] (const double v) {
                accum += (v - mean) * (v - mean);
            }
        );
        auto biased_stdv = std::sqrt(accum / (durations.size()));

        std::sort(durations.begin(), durations.end());
        static constexpr size_t lat_tag_count{6};
        static constexpr std::array<double, lat_tag_count> latency_tag = {0.1, 0.5, 0.9, 0.95, 0.99, 0.999};

        auto fmt = RFMT_1("\n[%1%] ", _name);
        size_t count{0};
        for (auto lat_tag : latency_tag) {
            auto lat_at = static_cast<size_t>(lat_tag * durations.size());

            auto lat = durations.at(lat_at) / 1000;
            if (count == 0) {
                fmt = boost::format("%1%p%2%: %3%us") % fmt % lat_tag % lat;
            } else {
                fmt = boost::format("%1%, p%2%: %3%us") % fmt % lat_tag % lat;
            }
            ++count;
        }

        auto min = durations.front() / 1000;
        auto max = durations.back() / 1000;

        fmt = boost::format("%1%, mean: %2%us, min: %3%us, max: %4%us, biased stdv: %5%, total_count: %6%")
          % fmt % (mean / 1000) % min % max % (biased_stdv / 1000) % durations.size();

        SPDK_ERRLOG("%s\n", fmt.str().c_str());
    }

private:

    std::string _name{""};
    std::unordered_map<key_type, dur_pair> _durs{};
    key_type _last_key{};
};

} // namespace utils
} // namespace fastblock

#else

#define FASTBLOCK_DUR_MAP_CREATE(key_type, var, name)
#define FASTBLOCK_DUR_MAP_EMPLACE_START(var, k)
#define FASTBLOCK_DUR_MAP_EMPLACE_END(var, k)
#define FASTBLOCK_DUR_MAP_EMPLACE_END_LAST(var)
#define FASTBLOCK_DUR_MAP_PRINT(var)

#endif
