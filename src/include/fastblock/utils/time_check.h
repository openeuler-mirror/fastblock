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

#include <chrono>

namespace utils {

class time_check {

public:

    time_check() = delete;

    time_check(const std::chrono::system_clock::duration dur) : _check_dur{dur} {}

public:

    bool check_and_update() {
        if (_check_point >= std::chrono::system_clock::now()) {
            return false;
        }

        _check_point = std::chrono::system_clock::now() + _check_dur;
        return true;
    }

private:

    std::chrono::system_clock::duration _check_dur{};
    std::chrono::system_clock::time_point _check_point{std::chrono::system_clock::now()};
};

}
