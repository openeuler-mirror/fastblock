/* Copyright (c) 2023 ChinaUnicom
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

#include "utils/fmt.h"

#include <chrono>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

namespace utils {

template <typename key_type>
class duration_map {

public:

    struct dur_element {
        std::string desc{""};
        std::chrono::system_clock::duration dur{};
    };

public:

    duration_map() = default;

    duration_map(const duration_map&) = delete;

    duration_map(duration_map&&) = delete;

    duration_map& operator=(const dur_element&) = delete;

    duration_map& operator=(dur_element&&) = delete;

    ~duration_map() noexcept = default;

public:

    void add_instance(const key_type& k) {
        _durs.emplace(k, std::vector<std::unique_ptr<dur_element>>{});
    }

    void emplace(const key_type& k, const std::string&& desc, const std::chrono::system_clock::duration dur) {
        auto dur_it = _durs.find(k);
        if (dur_it == _durs.end()) {
            throw std::runtime_error{"cant find duration key"};
        }

        auto e = std::make_unique<dur_element>(desc, dur);
        dur_it->second.push_back(std::move(e));
    }

    void emplace(const key_type& k, const std::string&& desc, const std::chrono::system_clock::time_point at) {
        auto dur = std::chrono::system_clock::now() - at;
        auto dur_it = _durs.find(k);
        if (dur_it == _durs.end()) {
            throw std::runtime_error{"cant find duration key"};
        }

        auto e = std::make_unique<dur_element>(desc, dur);
        dur_it->second.push_back(std::move(e));
    }

    std::string fmt(const key_type&& k) {
        auto dur_it = _durs.find(k);
        if (dur_it == _durs.end()) {
            throw std::runtime_error{"cant find duration key"};
        }

        auto& durs = dur_it->second;
        auto ret_fmt = boost::format();
        for (auto& dur_ptr : durs) {
            ret_fmt = RFMT_3(
              "%1%{%2%: %3%us}",
              ret_fmt, dur_ptr->desc,
              (dur_ptr->dur.count() / 1000));
        }

        return ret_fmt.str();
    }

    std::string fmt_all() {
        auto ret_fmt = boost::format("\n");
        for (auto& kv : _durs) {
            for (auto& dur_ptr : kv.second) {
                ret_fmt = RFMT_4(
                  "%1%{[%2%]%3%: %4%us}",
                  ret_fmt, kv.first, dur_ptr->desc,
                  (dur_ptr->dur.count() / 1000));
            }
            ret_fmt = RFMT_1("%1%\n", ret_fmt);
        }
        return ret_fmt.str();
    }

private:

    std::unordered_map<key_type, std::vector<std::unique_ptr<dur_element>>> _durs{};
};

} // namespace utils

