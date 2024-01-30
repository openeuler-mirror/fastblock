/* Copyright (c) 2024 ChinaUnicom
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
#include <cstddef>

// https://baike.baidu.com/item/%E5%AD%97%E8%8A%82

constexpr size_t B = 1; // 字节
constexpr size_t KB = 1024 * B;
constexpr size_t MB = 1024 * KB;
constexpr size_t GB = 1024 * MB;

constexpr size_t operator"" _KB(unsigned long long n) { return n * KB; }
constexpr size_t operator"" _MB(unsigned long long n) { return n * MB; }
constexpr size_t operator"" _GB(unsigned long long n) { return n * GB; }
