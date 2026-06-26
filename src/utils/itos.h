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

#include <algorithm>
#include <string>
#include <stdint.h>
#include <concepts>

template <class T>
requires std::is_integral_v<T>
inline std::string itos(T i) {
    if (i == 0) return "0";

    bool neg = false;
    // Use int64_t to handle minimum negative values correctly (e.g., INT8_MIN = -128)
    int64_t val = i;
    if (val < 0) { neg = true; val = -val; }

    std::string str;
    while(val) {
        str += "0123456789"[val % 10];
        val /= 10;
    }
    if (neg) { str += "-"; }

    reverse(str.begin(), str.end());
    return str;
}