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

#include <string>
#include <type_traits>

#include <spdk/env.h>

#define make_spdk_unique_n(type, size)                                                                                      \
    std::unique_ptr<type, decltype(::spdk_free)*>{                                                                          \
      reinterpret_cast<type*>(::spdk_zmalloc(sizeof(type) * size, 0, nullptr, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA)),    \
      ::spdk_free}

#define make_spdk_unique(type) make_spdk_unique_n(type, 1)

#define spdk_unique_ptr(type, ptr)    \
    std::unique_ptr<type, decltype(::spdk_free)*>{reinterpret_cast<type*>(ptr), ::spdk_free}

#define log_conf_pair_raw(conf_key, val)     \
    SPDK_NOTICELOG("configuration item \"%s\" value is %s\n", conf_key, val)

#define log_conf_pair(conf_key, val)     \
    log_conf_pair_raw(conf_key,  std::to_string(val).c_str())

#define conf_or(json_conf, json_key, opts, opts_key)                                        \
    if (json_conf.count(json_key) != 0) {                                                   \
        opts->opts_key =                                                                    \
            json_conf.get_child(json_key).get_value<decltype(opts->opts_key)>();            \
    }                                                                                       \
    log_conf_pair(json_key, opts->opts_key)

#define conf_or_s(json_conf, opts, opts_key) conf_or(json_conf, #opts_key, opts, opts_key)

namespace msg {
namespace rdma {

template<typename T>
using spdk_unique_ptr_t = std::unique_ptr<T, decltype(::spdk_free)*>;

enum class iterate_tag {
    keep = 1,
    stop
};

static constexpr uint8_t max_rpc_meta_string_size{31};

enum class status : uint8_t {
    success = 1,
    no_content,
    method_not_found,
    service_not_found,
    request_timeout,
    bad_request_body,
    bad_response_body,
    server_error
};

namespace {
static char* success_string = (char*)"success";
static char* no_content_string = (char*)"no_content";
static char* method_not_found_string = (char*)"method_not_found";
static char* service_not_found_string = (char*)"service_not_found";
static char* request_timeout_string = (char*)"request_timeout_found";
static char* bad_request_string = (char*)"bad_request_body";
static char* bad_response_string = (char*)"bad_response_body";
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

inline char* string_status(const status s) noexcept {
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
    case status::bad_request_body:
        return bad_request_string;
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
