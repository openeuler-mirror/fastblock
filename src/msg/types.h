#pragma once

#include <spdk/log.h>

#include <fmt/core.h>

#include <cstdint>
#include <string>
#include <type_traits>

namespace msg {
namespace rdma {

static constexpr uint8_t max_rpc_meta_string_size{31};

enum class status : uint32_t {
    success = 200,
    method_not_found = 404,
    service_not_found,
    request_timeout = 408,
    bad_response_body = 422,
    server_error = 500,
};

namespace {
static const char* const success_string{"success"};
static const char* const method_not_found_string{"method_not_found"};
static const char* const service_not_found_string{"service_not_found"};
static const char* const request_timeout_string{"request_timeout_found"};
static const char* const bad_response_string{"bad_response_found"};
static const char* const server_string{"server_found"};
static const char* const unknown_status_string{"unknown status"};
}

struct request_meta {
    using name_size_type = uint8_t;
    using data_size_type = uint32_t;

    char service_name[max_rpc_meta_string_size + 1];
    name_size_type service_name_size;
    char method_name[max_rpc_meta_string_size + 1];
    name_size_type method_name_size;
    data_size_type data_size;
};
static constexpr size_t request_meta_size{sizeof(request_meta)};

struct reply_meta {
    std::underlying_type_t<status> reply_status;
};
static constexpr size_t reply_meta_size{sizeof(reply_meta)};

inline const char* const string_status(const status s) noexcept {
    switch (s) {
    case status::success:
        return success_string;
    case status::method_not_found:
        return method_not_found_string;
    case status::service_not_found:
        return service_not_found_string;
    case status::request_timeout:
        return request_timeout_string;
    case status::bad_response_body:
        return bad_response_string;
    case status::server_error:
        return server_string;
    default:
        return unknown_status_string;
    }
}

inline const char* const string_status(const std::underlying_type_t<status> s) noexcept {
    return string_status(static_cast<status>(s));
}

} // namespace rdma
} // namespace msg
