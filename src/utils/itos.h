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

#include <algorithm>
#include <string>
#include <stdint.h>
#include <concepts>

template <class T>
requires std::is_integral_v<T>
inline std::string itos(T i) {
    if (i == 0) return "0";

    bool neg = false;
    if (i < 0) { neg = true, i = -1 * i; }

    std::string str;
    while(i) {
        str += "0123456789"[i % 10];
        i /= 10;
    }
    if (neg) { str += "-"; }

    reverse(str.begin(), str.end());
    return str;
}