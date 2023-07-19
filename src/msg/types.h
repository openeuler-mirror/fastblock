#pragma once

#include <fmt/core.h>

#include <cstdint>
#include <string>
#include <type_traits>

namespace msg {
namespace rdma {

enum class status : uint32_t {
    success = 200,
    method_not_found = 404,
    service_not_found,
    request_timeout = 408,
    bad_response_body = 422,
    server_error = 500,
};

std::string string_status(const status s) noexcept {
    switch (s) {
    case status::success:
        return std::string{"success"};
    case status::method_not_found:
        return std::string{"method_not_found"};
    case status::service_not_found:
        return std::string{"service_not_found"};
    case status::request_timeout:
        return std::string{"request_timeout"};
    case status::bad_response_body:
        return std::string{"bad_response_body"};
    case status::server_error:
        return std::string{"server_error"};
    default:
        return fmt::format(
          "unknown status({})",
          static_cast<std::underlying_type_t<decltype(s)>>(s));
    }
}

std::string string_status(const std::underlying_type_t<status> s) noexcept {
    return string_status(static_cast<status>(s));
}

bool is_good_status(const int s) noexcept {
    auto enum_s = static_cast<status>(s);
    switch(enum_s) {
    case status::success:
        return true;
    default:
        return false;
    }
}

} // namespace rdma
} // namespace msg
