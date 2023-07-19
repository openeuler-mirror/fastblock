#pragma once
#include <cstddef>

// https://baike.baidu.com/item/%E5%AD%97%E8%8A%82

constexpr size_t B = 1; // 字节
constexpr size_t KB = 1024 * B;
constexpr size_t MB = 1024 * KB;
constexpr size_t GB = 1024 * MB;

constexpr size_t operator"" _KB(unsigned long long val) { return val * KB; }
constexpr size_t operator"" _MB(unsigned long long val) { return val * MB; }
constexpr size_t operator"" _GB(unsigned long long val) { return val * GB; }
