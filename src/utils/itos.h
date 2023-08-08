#pragma once
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