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

#include <spdk/log.h>

#include <cstdint>
#include <string>
#include <type_traits>

namespace msg {
namespace rdma {

static constexpr uint8_t max_rpc_meta_string_size{31};

enum class status : uint32_t {
    success = 200,
    no_content = 204,
    method_not_found = 404,
    service_not_found,
    request_timeout = 408,
    bad_response_body = 422,
    server_error = 500,
};

namespace {
static char* success_string = (char*)"success";
static char* no_content_string = (char*)"no_content";
static char* method_not_found_string = (char*)"method_not_found";
static char* service_not_found_string = (char*)"service_not_found";
static char* request_timeout_string = (char*)"request_timeout_found";
static char* bad_response_string = (char*)"bad_response_found";
static char* server_string = (char*)"server_found";
static char* unknown_status_string = (char*)"unknown status";
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

inline char*  string_status(const status s) noexcept {
    switch (s) {
    case status::success:
        return success_string;
    case status::no_content:
        return no_content_string;
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

inline char*  string_status(const std::underlying_type_t<status> s) noexcept {
    return string_status(static_cast<status>(s));
}

} // namespace rdma
} // namespace msg
