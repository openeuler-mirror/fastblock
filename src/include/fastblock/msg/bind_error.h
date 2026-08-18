/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include <exception>

namespace msg {

class bind_error : public std::exception {
public:
    bind_error() noexcept = default;
    const char* what() const noexcept override { return "bind addr and port failed"; }
};

} // namespace msg
