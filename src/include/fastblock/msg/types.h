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
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
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
    }

#define conf_or_s(json_conf, json_key, opts, opts_key) conf_or(json_conf, json_key, opts, opts_key)

namespace msg {
namespace rdma {

template<typename T>
using spdk_unique_ptr_t = std::unique_ptr<T, decltype(::spdk_free)*>;

enum class iterate_tag {
    keep = 1,
    stop
};

static constexpr uint8_t max_rpc_meta_string_size{31};

// Documented development default for msg_{client,server}_rpc_timeout_us
// when the json key is omitted. Long regression / snapshot-wait must set
// the json key (vstart: FASTBLOCK_RPC_TIMEOUT_US=600000000).
static constexpr int64_t default_rpc_timeout_us{1'000'000};

enum class status : uint8_t {
    success = 1,
    no_content,
    method_not_found,
    service_not_found,
    request_timeout,
    bad_request_body,
    bad_response_body,
    terminating,
    server_error,
    resource_exhausted
};

namespace {
static char* success_string = (char*)"success";
static char* no_content_string = (char*)"no_content";
static char* method_not_found_string = (char*)"method_not_found";
static char* service_not_found_string = (char*)"service_not_found";
static char* request_timeout_string = (char*)"request_timeout";
static char* bad_request_string = (char*)"bad_request_body";
static char* bad_response_string = (char*)"bad_response_body";
static char* server_string = (char*)"server_error";
static char* terminating_string = (char*)"terminating";
static char* resource_exhausted_string = (char*)"resource_exhausted";
static char* unknown_status_string = (char*)"unknown status";
}

struct request_meta {
    using name_size_type = uint8_t;
    using data_size_type = uint32_t;

    char service_name[max_rpc_meta_string_size + 1];
    name_size_type service_name_size;
    char method_name[max_rpc_meta_string_size + 1];
    name_size_type method_name_size;
    data_size_type data_size;     // protobuf envelope only
    data_size_type payload_size;  // raw bulk after the envelope; 0 = none
};
static constexpr size_t request_meta_size{sizeof(request_meta)};
static_assert(std::is_trivially_copyable_v<request_meta>);

inline request_meta make_request_meta(
  std::string_view service_name,
  std::string_view method_name,
  request_meta::data_size_type data_size,
  request_meta::data_size_type payload_size = 0) noexcept {
    request_meta meta{};
    auto service_size = std::min(service_name.size(), static_cast<size_t>(max_rpc_meta_string_size));
    auto method_size = std::min(method_name.size(), static_cast<size_t>(max_rpc_meta_string_size));
    meta.service_name_size = static_cast<request_meta::name_size_type>(service_size);
    meta.method_name_size = static_cast<request_meta::name_size_type>(method_size);
    std::memcpy(meta.service_name, service_name.data(), service_size);
    std::memcpy(meta.method_name, method_name.data(), method_size);
    meta.data_size = data_size;
    meta.payload_size = payload_size;
    return meta;
}

inline request_meta load_request_meta(const void* raw) noexcept {
    request_meta meta{};
    std::memcpy(&meta, raw, sizeof(meta));
    return meta;
}

inline void store_request_meta(void* raw, const request_meta& meta) noexcept {
    std::memcpy(raw, &meta, sizeof(meta));
}

struct reply_meta {
    std::underlying_type_t<status> reply_status;
    uint8_t _pad[3]{};
    uint32_t data_size{0};     // protobuf envelope
    uint32_t payload_size{0};  // raw bulk after the envelope
};
static constexpr size_t reply_meta_size{sizeof(reply_meta)};
static_assert(std::is_trivially_copyable_v<reply_meta>);

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
    case status::terminating:
        return terminating_string;
    case status::resource_exhausted:
        return resource_exhausted_string;
    default:
        return unknown_status_string;
    }
}

inline char*  string_status(const std::underlying_type_t<status> s) noexcept {
    return string_status(static_cast<status>(s));
}

// Caller-facing class. Transport sets this on rpc_controller; Raft / libblk
// must not parse ErrorText().
enum class rpc_error_class : uint8_t {
    ok = 0,
    resource_exhausted,
    timeout,
    request_timeout,
    too_large,
    application,
    peer_terminating,
    connection
};

inline bool is_retryable(rpc_error_class c) noexcept {
    return c == rpc_error_class::resource_exhausted ||
           c == rpc_error_class::timeout ||
           c == rpc_error_class::request_timeout;
}

inline bool tears_connection(rpc_error_class c) noexcept {
    return c == rpc_error_class::connection ||
           c == rpc_error_class::peer_terminating;
}

// Fallback for SetFailed(string) paths that have not been given a class.
// Exact tokens only — callers must not depend on this.
inline rpc_error_class classify_rpc_error(std::string_view err) noexcept {
    if (err.empty()) {
        return rpc_error_class::ok;
    }
    if (err == "resource_exhausted") {
        return rpc_error_class::resource_exhausted;
    }
    if (err == "timeout") {
        return rpc_error_class::timeout;
    }
    if (err == "request_timeout") {
        return rpc_error_class::request_timeout;
    }
    if (err == "too_large") {
        return rpc_error_class::too_large;
    }
    if (err == "terminating" || err == "peer terminating" ||
        err == "server terminating") {
        return rpc_error_class::peer_terminating;
    }
    if (err == "connection terminated" || err == "bad ib/cm event") {
        return rpc_error_class::connection;
    }
    return rpc_error_class::application;
}

inline bool is_soft_rpc_error(std::string_view err) noexcept {
    return is_retryable(classify_rpc_error(err));
}

} // namespace rdma
} // namespace msg
