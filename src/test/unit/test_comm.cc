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

/**
 * @file test_comm.cc
 * @brief First-cut unit tests for communication-link modules (msg / rpc /
 *        monclient). Keeps the scope deliberately small — just the data
 *        contracts every wire-touching caller depends on. More cases can be
 *        layered in later commits.
 *
 * The real msg/rpc/monclient libs pull in rdmacm + spdk_env transitively, so
 * we mirror the trivially-copyable structs and enums here instead of linking
 * the production sources. If the production layout drifts, update these
 * mirrors deliberately.
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// ============================================================================
// Test Suite: msg — RDMA request_meta wire framing
// ============================================================================

namespace {

static constexpr uint8_t max_rpc_meta_string_size{31};

struct request_meta {
    using name_size_type = uint8_t;
    using data_size_type = uint32_t;

    char service_name[max_rpc_meta_string_size + 1];
    name_size_type service_name_size;
    char method_name[max_rpc_meta_string_size + 1];
    name_size_type method_name_size;
    data_size_type data_size;
};
static_assert(std::is_trivially_copyable_v<request_meta>);

inline request_meta make_request_meta(
  std::string_view service_name,
  std::string_view method_name,
  request_meta::data_size_type data_size) noexcept {
    request_meta meta{};
    auto service_size = std::min(service_name.size(), static_cast<size_t>(max_rpc_meta_string_size));
    auto method_size  = std::min(method_name.size(),  static_cast<size_t>(max_rpc_meta_string_size));
    meta.service_name_size = static_cast<request_meta::name_size_type>(service_size);
    meta.method_name_size  = static_cast<request_meta::name_size_type>(method_size);
    std::memcpy(meta.service_name, service_name.data(), service_size);
    std::memcpy(meta.method_name,  method_name.data(),  method_size);
    meta.data_size = data_size;
    return meta;
}

} // anonymous namespace

FB_SUITE_SETUP(msg_request_meta) {}
FB_SUITE_TEARDOWN(msg_request_meta) {}

FB_TEST(msg_request_meta, layout_is_trivially_copyable) {
    // Whole point of request_meta is that we can memcpy it in/out of an RDMA
    // buffer. Lock the layout down.
    FB_ASSERT_TRUE(std::is_trivially_copyable_v<request_meta>);
}

FB_TEST(msg_request_meta, build_normal_request) {
    auto meta = make_request_meta("RaftService", "AppendEntries", 1024);
    FB_ASSERT_EQ(meta.service_name_size, 11);
    FB_ASSERT_EQ(meta.method_name_size, 13);
    FB_ASSERT_EQ(meta.data_size, 1024u);
}

FB_TEST(msg_request_meta, oversize_names_are_truncated) {
    // Names longer than max_rpc_meta_string_size must be silently clamped so
    // the fixed-size buffer stays safe.
    std::string long_name(40, 'a');
    auto meta = make_request_meta(long_name, long_name, 0);
    FB_ASSERT_EQ(meta.service_name_size, max_rpc_meta_string_size);
    FB_ASSERT_EQ(meta.method_name_size,  max_rpc_meta_string_size);
}

// ============================================================================
// Test Suite: msg — reply status code surface
// ============================================================================

namespace {

enum class status : uint8_t {
    success = 1,
    no_content,
    method_not_found,
    service_not_found,
    request_timeout,
    bad_request_body,
    bad_response_body,
    terminating,
    server_error
};

} // anonymous namespace

FB_SUITE_SETUP(msg_reply_status) {}
FB_SUITE_TEARDOWN(msg_reply_status) {}

FB_TEST(msg_reply_status, underlying_type_is_uint8) {
    // reply_meta carries one byte on the wire; guard against accidental
    // widening of the underlying type.
    using under = std::underlying_type_t<status>;
    FB_ASSERT_TRUE((std::is_same_v<under, uint8_t>));
    FB_ASSERT_EQ(sizeof(status), 1u);
}

FB_TEST(msg_reply_status, success_is_one_not_zero) {
    // Zero is reserved so a default-init reply_meta isn't mistaken for success.
    FB_ASSERT_EQ(static_cast<int>(status::success), 1);
}

// ============================================================================
// Test Suite: rpc — connect_cache shard/node bookkeeping
//
// Mirrors the production data layout: one connection map per shard, keyed by
// node_id. Replaces the real RDMA connection with a fake_connection struct
// so we can exercise the contract without bringing up RDMA.
// ============================================================================

namespace {

struct fake_connection {
    int node_id{0};
    std::string addr{};
    uint16_t port{0};
};

class fake_connect_cache {
public:
    using connect_ptr = std::shared_ptr<fake_connection>;

    explicit fake_connect_cache(uint32_t shard_count) {
        _cache.resize(shard_count);
    }

    bool create_connect(uint32_t shard_id, int node_id, std::string addr, uint16_t port) {
        if (shard_id >= _cache.size()) return false;
        if (_cache[shard_id].count(node_id) > 0) return false;
        _cache[shard_id][node_id] = std::make_shared<fake_connection>(
          fake_connection{node_id, std::move(addr), port});
        return true;
    }

    bool contains(uint32_t shard_id, int node_id) const {
        if (shard_id >= _cache.size()) return false;
        return _cache[shard_id].count(node_id) > 0;
    }

    connect_ptr get_connect(uint32_t shard_id, int node_id) const {
        if (shard_id >= _cache.size()) return nullptr;
        auto it = _cache[shard_id].find(node_id);
        if (it == _cache[shard_id].end()) return nullptr;
        return it->second;
    }

    bool remove_connect(uint32_t shard_id, int node_id) {
        if (shard_id >= _cache.size()) return false;
        return _cache[shard_id].erase(node_id) > 0;
    }

private:
    std::vector<std::map<int, connect_ptr>> _cache;
};

} // anonymous namespace

FB_SUITE_SETUP(rpc_connect_cache) {}
FB_SUITE_TEARDOWN(rpc_connect_cache) {}

FB_TEST(rpc_connect_cache, create_and_lookup_basic) {
    fake_connect_cache cache(2);
    FB_ASSERT_TRUE(cache.create_connect(0, 101, "10.0.0.1", 5000));
    FB_ASSERT_TRUE(cache.contains(0, 101));
    FB_ASSERT_FALSE(cache.contains(0, 999));
    FB_ASSERT_FALSE(cache.contains(1, 101)); // wrong shard

    auto conn = cache.get_connect(0, 101);
    FB_ASSERT_NOT_NULL(conn.get());
    FB_ASSERT_EQ(conn->node_id, 101);
}

FB_TEST(rpc_connect_cache, out_of_range_shard_is_safe) {
    fake_connect_cache cache(2);
    FB_ASSERT_FALSE(cache.create_connect(5, 1, "a", 1));
    FB_ASSERT_FALSE(cache.contains(5, 1));
    FB_ASSERT_NULL(cache.get_connect(5, 1).get());
    FB_ASSERT_FALSE(cache.remove_connect(5, 1));
}

FB_TEST(rpc_connect_cache, remove_clears_entry) {
    fake_connect_cache cache(1);
    cache.create_connect(0, 42, "x", 1);
    FB_ASSERT_TRUE(cache.remove_connect(0, 42));
    FB_ASSERT_FALSE(cache.contains(0, 42));
    // Idempotent: removing again returns false but doesn't crash.
    FB_ASSERT_FALSE(cache.remove_connect(0, 42));
}

// ============================================================================
// Test Suite: monclient — response_status surface
// ============================================================================

namespace {

enum mon_response_status {
    ok = 0,
    OSD_ERR_NOT_APPLY            = -156,
    OSD_ERR_ID_CONFLICT          = -157,
    OSD_ERR_ADDRESS_INVALID      = -158,
    OSD_ERR_UPDATE_STATE_FAILED  = -159,
    OSD_ERR_CORE_NUM             = -160,
};

} // anonymous namespace

FB_SUITE_SETUP(monclient_status) {}
FB_SUITE_TEARDOWN(monclient_status) {}

FB_TEST(monclient_status, ok_is_zero) {
    FB_ASSERT_EQ(static_cast<int>(ok), 0);
}

FB_TEST(monclient_status, osd_register_errors_are_negative) {
    // OSD register errors use negative codes so callers can branch on sign
    // to separate monitor-side errors from osd-side errors.
    FB_ASSERT_TRUE(static_cast<int>(OSD_ERR_NOT_APPLY) < 0);
    FB_ASSERT_TRUE(static_cast<int>(OSD_ERR_CORE_NUM)  < 0);
}

FB_TEST(monclient_status, osd_register_error_values_are_stable) {
    // Numeric values appear in logs and (potentially) in client retry logic;
    // changing them should require updating this test deliberately.
    FB_ASSERT_EQ(static_cast<int>(OSD_ERR_NOT_APPLY),           -156);
    FB_ASSERT_EQ(static_cast<int>(OSD_ERR_ID_CONFLICT),         -157);
    FB_ASSERT_EQ(static_cast<int>(OSD_ERR_ADDRESS_INVALID),     -158);
    FB_ASSERT_EQ(static_cast<int>(OSD_ERR_UPDATE_STATE_FAILED), -159);
    FB_ASSERT_EQ(static_cast<int>(OSD_ERR_CORE_NUM),            -160);
}

// ============================================================================
// Test Suite: monclient — pg_state bitmask
// ============================================================================

namespace {

enum pg_state {
    PgCreating  = 1 << 0,
    PgActive    = 1 << 1,
    PgUndersize = 1 << 2,
    PgDown      = 1 << 3,
    PgRemapped  = 1 << 4
};

} // anonymous namespace

FB_SUITE_SETUP(monclient_pg_state) {}
FB_SUITE_TEARDOWN(monclient_pg_state) {}

FB_TEST(monclient_pg_state, values_are_powers_of_two) {
    // pg_state is a bitmask; aliased values would make bitwise checks
    // ambiguous (Active|Undersize, etc.).
    FB_ASSERT_EQ(PgCreating,  1);
    FB_ASSERT_EQ(PgActive,    2);
    FB_ASSERT_EQ(PgUndersize, 4);
    FB_ASSERT_EQ(PgDown,      8);
    FB_ASSERT_EQ(PgRemapped, 16);
}

FB_TEST(monclient_pg_state, states_can_be_combined) {
    // A pg can be Active AND Undersize at the same time. The bitwise OR
    // must keep both bits.
    int combined = PgActive | PgUndersize;
    FB_ASSERT_TRUE((combined & PgActive) != 0);
    FB_ASSERT_TRUE((combined & PgUndersize) != 0);
    FB_ASSERT_FALSE((combined & PgDown) != 0);
}

// Main function for test runner
FB_TEST_MAIN()
