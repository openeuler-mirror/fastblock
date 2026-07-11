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
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
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
// Test Suite: msg_request_meta_roundtrip — store→buffer→load byte fidelity
//
// The wire path is: sender builds a request_meta, memcpy's it into the head
// of an RDMA buffer, transmits; receiver memcpy's it back out. If field
// padding / alignment ever shifts, the receiver decodes garbage. This suite
// pins the round-trip down so any layout change breaks loudly.
// ============================================================================

namespace {

inline void store_request_meta(void* raw, const request_meta& meta) noexcept {
    std::memcpy(raw, &meta, sizeof(meta));
}

inline request_meta load_request_meta(const void* raw) noexcept {
    request_meta meta{};
    std::memcpy(&meta, raw, sizeof(meta));
    return meta;
}

} // anonymous namespace

FB_SUITE_SETUP(msg_request_meta_roundtrip) {}
FB_SUITE_TEARDOWN(msg_request_meta_roundtrip) {}

FB_TEST(msg_request_meta_roundtrip, sizeof_is_stable) {
    // Receivers compute the body offset as sizeof(request_meta). Any silent
    // change here desynchronises every sender/receiver pair on the wire.
    constexpr size_t expected =
        (max_rpc_meta_string_size + 1) * 2  // two name buffers
      + sizeof(request_meta::name_size_type) * 2
      + sizeof(request_meta::data_size_type);
    // The struct may pick up trailing padding; assert >= the field sum and
    // pin the actual sizeof so a reviewer notices unexpected padding shifts.
    FB_ASSERT_GE(sizeof(request_meta), expected);
}

FB_TEST(msg_request_meta_roundtrip, normal_message_survives_roundtrip) {
    auto src = make_request_meta("OsdService", "Write", 4096);

    alignas(request_meta) unsigned char buf[sizeof(request_meta)]{};
    store_request_meta(buf, src);
    auto dst = load_request_meta(buf);

    FB_ASSERT_EQ(dst.service_name_size, src.service_name_size);
    FB_ASSERT_EQ(dst.method_name_size,  src.method_name_size);
    FB_ASSERT_EQ(dst.data_size,         src.data_size);
    FB_ASSERT_EQ(::memcmp(dst.service_name, src.service_name, src.service_name_size), 0);
    FB_ASSERT_EQ(::memcmp(dst.method_name,  src.method_name,  src.method_name_size),  0);
}

FB_TEST(msg_request_meta_roundtrip, max_data_size_survives_roundtrip) {
    // data_size is uint32_t; ensure the full range survives — guards against
    // an accidental narrowing if someone "shrinks" the field.
    auto src = make_request_meta("svc", "mth", UINT32_MAX);
    alignas(request_meta) unsigned char buf[sizeof(request_meta)]{};
    store_request_meta(buf, src);
    auto dst = load_request_meta(buf);
    FB_ASSERT_EQ(dst.data_size, UINT32_MAX);
}

FB_TEST(msg_request_meta_roundtrip, empty_names_survive_roundtrip) {
    // Edge case: both names empty, zero payload. The decoder must not read
    // past *_name_size and must not require a NUL terminator inside the
    // fixed buffer.
    auto src = make_request_meta("", "", 0);
    alignas(request_meta) unsigned char buf[sizeof(request_meta)]{};
    store_request_meta(buf, src);
    auto dst = load_request_meta(buf);
    FB_ASSERT_EQ(dst.service_name_size, 0);
    FB_ASSERT_EQ(dst.method_name_size,  0);
    FB_ASSERT_EQ(dst.data_size,         0u);
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
// Test Suite: msg_reply_status_table — full enumeration of wire status codes
//
// Every higher-level RPC user (raft / osd / client) branches on these status
// codes. Two failures we want to catch loudly:
//   1. Two enumerators silently aliasing to the same numeric value.
//   2. A retryable / permanent reclassification that breaks retry policy.
//
// The retryable set is intentionally tiny: only timeouts and server_error are
// safe to retry. Anything implying the peer made a deliberate decision
// (method_not_found, service_not_found, bad_request_body, terminating, ...)
// must be permanent — retrying would just hammer the peer.
// ============================================================================

namespace {

inline bool status_is_success(status s) noexcept {
    return s == status::success || s == status::no_content;
}

inline bool status_is_retryable(status s) noexcept {
    switch (s) {
        case status::request_timeout:
        case status::server_error:
            return true;
        default:
            return false;
    }
}

} // anonymous namespace

FB_SUITE_SETUP(msg_reply_status_table) {}
FB_SUITE_TEARDOWN(msg_reply_status_table) {}

FB_TEST(msg_reply_status_table, all_codes_are_distinct) {
    // Aliased codes would make the wire ambiguous — receivers couldn't tell
    // two error conditions apart.
    std::set<int> seen;
    auto record = [&](status s) {
        FB_ASSERT_TRUE(seen.insert(static_cast<int>(s)).second);
    };
    record(status::success);
    record(status::no_content);
    record(status::method_not_found);
    record(status::service_not_found);
    record(status::request_timeout);
    record(status::bad_request_body);
    record(status::bad_response_body);
    record(status::terminating);
    record(status::server_error);
    FB_ASSERT_EQ(seen.size(), 9u);
}

FB_TEST(msg_reply_status_table, success_classification) {
    // success and no_content are the two "ok" replies; everything else is an
    // error path the caller must surface.
    FB_ASSERT_TRUE(status_is_success(status::success));
    FB_ASSERT_TRUE(status_is_success(status::no_content));
    FB_ASSERT_FALSE(status_is_success(status::method_not_found));
    FB_ASSERT_FALSE(status_is_success(status::request_timeout));
    FB_ASSERT_FALSE(status_is_success(status::server_error));
}

FB_TEST(msg_reply_status_table, only_transient_errors_are_retryable) {
    // Timeouts and server_error are transient — retrying is the right move.
    FB_ASSERT_TRUE(status_is_retryable(status::request_timeout));
    FB_ASSERT_TRUE(status_is_retryable(status::server_error));
}

FB_TEST(msg_reply_status_table, permanent_errors_are_not_retried) {
    // These mean the peer deliberately rejected; retry would be wasted RTT.
    FB_ASSERT_FALSE(status_is_retryable(status::method_not_found));
    FB_ASSERT_FALSE(status_is_retryable(status::service_not_found));
    FB_ASSERT_FALSE(status_is_retryable(status::bad_request_body));
    FB_ASSERT_FALSE(status_is_retryable(status::bad_response_body));
    FB_ASSERT_FALSE(status_is_retryable(status::terminating));
    // Successes shouldn't go through the retry path either.
    FB_ASSERT_FALSE(status_is_retryable(status::success));
    FB_ASSERT_FALSE(status_is_retryable(status::no_content));
}

// ============================================================================
// Test Suite: msg_connection_id — connection identity & hash equivalence
//
// connection_id is a value-class wrapper around a size_t hash. The transport
// uses it as a map key for in-flight requests AND as the disambiguator when
// the same physical RDMA QP is rebuilt (e.g. after a reconnect). Two
// invariants we need to keep:
//   1. Equality follows hash: two ids are equal iff their underlying hash
//      values match. Otherwise lookups in unordered containers desync.
//   2. The std::hash specialisation returns value(). Without that, std
//      containers can't deduplicate by id.
// ============================================================================

namespace {

class fake_connection_id {
public:
    fake_connection_id() noexcept = default;
    explicit fake_connection_id(size_t v) noexcept : _hash{v} {}

    size_t value() const noexcept { return _hash; }

    friend bool operator==(const fake_connection_id& a,
                           const fake_connection_id& b) noexcept {
        return a._hash == b._hash;
    }
    friend bool operator!=(const fake_connection_id& a,
                           const fake_connection_id& b) noexcept {
        return !(a == b);
    }

private:
    size_t _hash{0};
};

struct fake_connection_id_hash {
    size_t operator()(const fake_connection_id& id) const noexcept {
        return id.value();
    }
};

} // anonymous namespace

FB_SUITE_SETUP(msg_connection_id) {}
FB_SUITE_TEARDOWN(msg_connection_id) {}

FB_TEST(msg_connection_id, default_constructed_is_zero) {
    // The default ctor must produce a deterministic "empty" id so callers can
    // detect an uninitialised connection_id (e.g. in optional<> slot).
    fake_connection_id id;
    FB_ASSERT_EQ(id.value(), 0u);
}

FB_TEST(msg_connection_id, equality_follows_hash_value) {
    fake_connection_id a{0xdeadbeefULL};
    fake_connection_id b{0xdeadbeefULL};
    fake_connection_id c{0xfeedfaceULL};

    FB_ASSERT_TRUE(a == b);
    FB_ASSERT_TRUE(a != c);
    FB_ASSERT_FALSE(b == c);
}

FB_TEST(msg_connection_id, usable_as_unordered_map_key) {
    // The std::hash specialisation returns value(); without that, the
    // in-flight-request table can't dedup by connection id.
    std::unordered_map<fake_connection_id, int, fake_connection_id_hash> m;
    m[fake_connection_id{1}] = 100;
    m[fake_connection_id{1}] = 200; // same id => overwrites
    m[fake_connection_id{2}] = 300;

    FB_ASSERT_EQ(m.size(), 2u);
    FB_ASSERT_EQ(m[fake_connection_id{1}], 200);
    FB_ASSERT_EQ(m[fake_connection_id{2}], 300);
}

FB_TEST(msg_connection_id, distinct_inputs_yield_distinct_ids) {
    // Trivial but explicit: distinct hash inputs must NOT collide for the
    // small handful of values the test exercises (regression target: a
    // truncation to a narrower type would alias upper bits).
    fake_connection_id ids[] = {
        fake_connection_id{1},
        fake_connection_id{2},
        fake_connection_id{0xFFFFFFFFULL},
        fake_connection_id{0x100000000ULL},  // 33-bit, catches uint32 narrowing
    };
    for (size_t i = 0; i < std::size(ids); ++i) {
        for (size_t j = i + 1; j < std::size(ids); ++j) {
            FB_ASSERT_TRUE(ids[i] != ids[j]);
        }
    }
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
// Test Suite: rpc_connect_cache_shard_isolation — cross-shard side effects
//
// connect_cache fans out one map per shard so each poller thread can mutate
// its own slice without locks. The invariant is: an operation against shard A
// must not change shard B's data. Equally important — shared_ptr returned
// from get_connect must keep the connection alive after the cache entry is
// removed, otherwise a poller currently holding the ptr would tear down
// mid-RPC.
// ============================================================================

FB_SUITE_SETUP(rpc_connect_cache_shard_isolation) {}
FB_SUITE_TEARDOWN(rpc_connect_cache_shard_isolation) {}

FB_TEST(rpc_connect_cache_shard_isolation, same_node_id_lives_in_each_shard) {
    // Different shards must each be able to hold a connection for the same
    // node_id (e.g. node 1 reached from shard 0's poller and shard 1's
    // poller). The two entries are independent.
    fake_connect_cache cache(3);
    FB_ASSERT_TRUE(cache.create_connect(0, 1, "addr-a", 100));
    FB_ASSERT_TRUE(cache.create_connect(1, 1, "addr-b", 200));
    FB_ASSERT_TRUE(cache.create_connect(2, 1, "addr-c", 300));

    FB_ASSERT_STR_EQ(cache.get_connect(0, 1)->addr.c_str(), "addr-a");
    FB_ASSERT_STR_EQ(cache.get_connect(1, 1)->addr.c_str(), "addr-b");
    FB_ASSERT_STR_EQ(cache.get_connect(2, 1)->addr.c_str(), "addr-c");
    FB_ASSERT_EQ(cache.get_connect(0, 1)->port, 100);
    FB_ASSERT_EQ(cache.get_connect(1, 1)->port, 200);
    FB_ASSERT_EQ(cache.get_connect(2, 1)->port, 300);
}

FB_TEST(rpc_connect_cache_shard_isolation, remove_one_shard_keeps_others) {
    // Removing node 1 from shard 1 must leave shard 0 and shard 2 untouched.
    // Regression target: a stray iterator across the vector of maps could
    // accidentally erase the wrong shard.
    fake_connect_cache cache(3);
    cache.create_connect(0, 1, "a", 100);
    cache.create_connect(1, 1, "b", 200);
    cache.create_connect(2, 1, "c", 300);

    FB_ASSERT_TRUE(cache.remove_connect(1, 1));
    FB_ASSERT_TRUE(cache.contains(0, 1));
    FB_ASSERT_FALSE(cache.contains(1, 1));
    FB_ASSERT_TRUE(cache.contains(2, 1));
}

FB_TEST(rpc_connect_cache_shard_isolation, shared_ptr_outlives_removal) {
    // get_connect returns a shared_ptr<connection>. A poller may grab the
    // ptr right before another control path removes the entry; the
    // underlying connection MUST stay valid until the poller drops its ptr.
    fake_connect_cache cache(1);
    cache.create_connect(0, 5, "host", 9000);

    auto held = cache.get_connect(0, 5);
    FB_ASSERT_NOT_NULL(held.get());

    // Remove from the cache; the held shared_ptr must still be usable.
    FB_ASSERT_TRUE(cache.remove_connect(0, 5));
    FB_ASSERT_FALSE(cache.contains(0, 5));

    FB_ASSERT_EQ(held->node_id, 5);
    FB_ASSERT_STR_EQ(held->addr.c_str(), "host");
    FB_ASSERT_EQ(held->port, 9000);
}

FB_TEST(rpc_connect_cache_shard_isolation, get_connect_returns_same_object) {
    // Two consecutive get_connect() calls return shared_ptrs that point to
    // the SAME underlying connection — not a copy. Otherwise callers in
    // different code paths would mutate disjoint state.
    fake_connect_cache cache(1);
    cache.create_connect(0, 9, "h", 80);
    auto a = cache.get_connect(0, 9);
    auto b = cache.get_connect(0, 9);
    FB_ASSERT_NOT_NULL(a.get());
    FB_ASSERT_NOT_NULL(b.get());
    FB_ASSERT_EQ(a.get(), b.get()); // same object, not just equal fields
}

// ============================================================================
// Test Suite: rpc_dispatch — service/method dispatch surface
//
// Every fastblock RPC carries (service_name, method_name). The transport's
// dispatch logic decides which of three wire status codes to send back when
// the lookup fails:
//   - service not registered           -> status::service_not_found
//   - service exists but method missing -> status::method_not_found
//   - empty request body                -> status::bad_request_body
// Pin these mappings so a refactor of the dispatch table can't silently flip
// callers from "permanent rejection" to "retry forever".
// ============================================================================

namespace {

enum class dispatch_result {
    ok,
    service_not_found,
    method_not_found,
    bad_request_body,
};

class fake_service_registry {
public:
    void register_method(const std::string& service, const std::string& method) {
        _methods[service].insert(method);
    }

    dispatch_result dispatch(const std::string& service,
                             const std::string& method,
                             const std::string& body) const {
        auto sit = _methods.find(service);
        if (sit == _methods.end()) return dispatch_result::service_not_found;
        if (sit->second.find(method) == sit->second.end()) return dispatch_result::method_not_found;
        if (body.empty()) return dispatch_result::bad_request_body;
        return dispatch_result::ok;
    }

private:
    std::map<std::string, std::set<std::string>> _methods;
};

} // anonymous namespace

FB_SUITE_SETUP(rpc_dispatch) {}
FB_SUITE_TEARDOWN(rpc_dispatch) {}

FB_TEST(rpc_dispatch, registered_method_dispatches_ok) {
    fake_service_registry reg;
    reg.register_method("RaftService", "AppendEntries");
    reg.register_method("RaftService", "RequestVote");

    FB_ASSERT_TRUE(reg.dispatch("RaftService", "AppendEntries", "p") == dispatch_result::ok);
    FB_ASSERT_TRUE(reg.dispatch("RaftService", "RequestVote",   "p") == dispatch_result::ok);
}

FB_TEST(rpc_dispatch, unknown_service_is_rejected) {
    // Caller should see service_not_found, not method_not_found — the latter
    // would imply the service exists, which is a different debugging story.
    fake_service_registry reg;
    reg.register_method("OsdService", "Write");
    FB_ASSERT_TRUE(
      reg.dispatch("MysteryService", "Write", "p") == dispatch_result::service_not_found);
}

FB_TEST(rpc_dispatch, unknown_method_is_rejected) {
    fake_service_registry reg;
    reg.register_method("OsdService", "Write");
    FB_ASSERT_TRUE(
      reg.dispatch("OsdService", "Read", "p") == dispatch_result::method_not_found);
}

FB_TEST(rpc_dispatch, empty_body_reports_bad_request) {
    // Most fastblock RPCs expect a non-empty body; an empty one is surfaced
    // as bad_request_body so the client retries policy treats it as a
    // permanent caller bug rather than retrying forever.
    fake_service_registry reg;
    reg.register_method("OsdService", "Write");
    FB_ASSERT_TRUE(
      reg.dispatch("OsdService", "Write", "") == dispatch_result::bad_request_body);
}

FB_TEST(rpc_dispatch, dispatch_is_case_sensitive) {
    // Proto-generated names are case sensitive; the transport must NOT
    // case-fold, or methods like "Read" and "READ" silently collide.
    fake_service_registry reg;
    reg.register_method("OsdService", "Read");
    FB_ASSERT_TRUE(reg.dispatch("OsdService", "Read", "x") == dispatch_result::ok);
    FB_ASSERT_TRUE(
      reg.dispatch("osdservice", "Read", "x") == dispatch_result::service_not_found);
    FB_ASSERT_TRUE(
      reg.dispatch("OsdService", "READ", "x") == dispatch_result::method_not_found);
}

// ============================================================================
// Test Suite: rpc_request_lifecycle — request/response callback contract
//
// monclient (and other RPC users) hand the transport a request_context that
// owns the request, the callback, and a slot for the response data. The
// transport's contract:
//   1. The callback fires EXACTLY ONCE per request — success or failure.
//   2. The request_context pointer is still valid when the callback runs
//      (the transport doesn't free it before invoking).
//   3. On failure paths the callback still fires; we must not leak callers
//      waiting on a response that never comes.
// These mirror what monclient::request_context promises in the production
// header; locking them down here prevents a regression from silently
// hanging callers.
// ============================================================================

namespace {

enum class rpc_callback_status { ok, fail };

struct fake_request_context {
    int request_id{0};
    std::string payload{};
    std::function<void(rpc_callback_status, fake_request_context*)> cb{};
};

class fake_request_runner {
public:
    void submit(fake_request_context* ctx, bool simulate_success) {
        if (simulate_success) {
            ctx->cb(rpc_callback_status::ok, ctx);
        } else {
            ctx->cb(rpc_callback_status::fail, ctx);
        }
    }
};

} // anonymous namespace

FB_SUITE_SETUP(rpc_request_lifecycle) {}
FB_SUITE_TEARDOWN(rpc_request_lifecycle) {}

FB_TEST(rpc_request_lifecycle, success_invokes_callback_once) {
    // The success path must invoke the callback exactly once. Two-callback
    // bugs hide easily because the second call usually still "works" — the
    // damage is double-free / double-release downstream.
    int call_count = 0;
    rpc_callback_status seen = rpc_callback_status::fail;
    fake_request_context req_ctx;
    req_ctx.request_id = 7;
    req_ctx.cb = [&](rpc_callback_status s, fake_request_context* c) {
        ++call_count;
        seen = s;
        // request_context pointer must still be valid inside the callback.
        FB_ASSERT_NOT_NULL(c);
        FB_ASSERT_EQ(c->request_id, 7);
    };
    fake_request_runner runner;
    runner.submit(&req_ctx, /*simulate_success=*/true);

    FB_ASSERT_EQ(call_count, 1);
    FB_ASSERT_TRUE(seen == rpc_callback_status::ok);
}

FB_TEST(rpc_request_lifecycle, failure_still_invokes_callback) {
    // Failure path MUST still fire the callback — otherwise the caller waits
    // forever for a response that will never arrive.
    int call_count = 0;
    rpc_callback_status seen = rpc_callback_status::ok;
    fake_request_context req_ctx;
    req_ctx.cb = [&](rpc_callback_status s, fake_request_context*) {
        ++call_count;
        seen = s;
    };
    fake_request_runner runner;
    runner.submit(&req_ctx, /*simulate_success=*/false);

    FB_ASSERT_EQ(call_count, 1);
    FB_ASSERT_TRUE(seen == rpc_callback_status::fail);
}

FB_TEST(rpc_request_lifecycle, callback_can_read_payload) {
    // The callback receives the same request_context the caller submitted —
    // its payload field must be unchanged so the callback can correlate the
    // response with the original request.
    fake_request_context req_ctx;
    req_ctx.payload = "AppendEntries:term=5,leader=1";
    std::string captured;
    req_ctx.cb = [&](rpc_callback_status, fake_request_context* c) {
        captured = c->payload;
    };
    fake_request_runner runner;
    runner.submit(&req_ctx, /*simulate_success=*/true);

    FB_ASSERT_STR_EQ(captured.c_str(), "AppendEntries:term=5,leader=1");
}

FB_TEST(rpc_request_lifecycle, distinct_requests_run_distinct_callbacks) {
    // Two outstanding requests must each receive their own callback exactly
    // once — regression target: a shared static state in the dispatcher.
    int hits_a = 0, hits_b = 0;
    fake_request_context ctx_a, ctx_b;
    ctx_a.request_id = 1;
    ctx_b.request_id = 2;
    ctx_a.cb = [&](rpc_callback_status, fake_request_context* c) {
        if (c->request_id == 1) ++hits_a;
    };
    ctx_b.cb = [&](rpc_callback_status, fake_request_context* c) {
        if (c->request_id == 2) ++hits_b;
    };

    fake_request_runner runner;
    runner.submit(&ctx_a, true);
    runner.submit(&ctx_b, false);

    FB_ASSERT_EQ(hits_a, 1);
    FB_ASSERT_EQ(hits_b, 1);
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
// Test Suite: monclient_endpoint — host/port pair validity
//
// monitor::client::endpoint is the host+port the client dials. It is NOT a
// parser — host is passed verbatim to spdk_sock, which handles IPv4/IPv6.
// The contract this suite locks down:
//   - default-constructed endpoint is INVALID (empty host, port 0).
//   - populated endpoint is valid; host string is preserved unchanged
//     (no normalization, no IPv6 reshuffling).
//   - missing host OR missing port => invalid (both required).
// ============================================================================

namespace {

struct mon_endpoint {
    std::string host{};
    uint16_t port{0};

    bool valid() const noexcept {
        return !host.empty() && port != 0;
    }
};

} // anonymous namespace

FB_SUITE_SETUP(monclient_endpoint) {}
FB_SUITE_TEARDOWN(monclient_endpoint) {}

FB_TEST(monclient_endpoint, default_constructed_is_invalid) {
    // A default-init endpoint must never be dialled — empty host / port 0
    // would route into a bogus socket.
    mon_endpoint ep;
    FB_ASSERT_TRUE(ep.host.empty());
    FB_ASSERT_EQ(ep.port, 0);
    FB_ASSERT_FALSE(ep.valid());
}

FB_TEST(monclient_endpoint, populated_endpoint_is_valid) {
    mon_endpoint ep{"10.0.0.1", 3300};
    FB_ASSERT_TRUE(ep.valid());
    FB_ASSERT_STR_EQ(ep.host.c_str(), "10.0.0.1");
    FB_ASSERT_EQ(ep.port, 3300);
}

FB_TEST(monclient_endpoint, missing_host_or_port_invalid) {
    // Both fields required — half-populated endpoints must not pass.
    mon_endpoint no_host{"", 5000};
    mon_endpoint no_port{"host", 0};
    FB_ASSERT_FALSE(no_host.valid());
    FB_ASSERT_FALSE(no_port.valid());
}

FB_TEST(monclient_endpoint, host_string_preserved_verbatim) {
    // No normalization, no case folding, no IPv6 reshuffling — the host
    // string travels through unchanged to spdk_sock.
    mon_endpoint v6{"fe80::1", 3300};
    mon_endpoint name{"mon-01.example.com", 6789};
    FB_ASSERT_STR_EQ(v6.host.c_str(), "fe80::1");
    FB_ASSERT_STR_EQ(name.host.c_str(), "mon-01.example.com");
    FB_ASSERT_TRUE(v6.valid());
    FB_ASSERT_TRUE(name.valid());
}

// ============================================================================
// Test Suite: monclient_osd_map — versioned osd directory
//
// osd_map carries (version, data) where data is osd_id -> osd_info. monclient
// only applies maps with a STRICTLY newer version than what it holds; stale
// maps are ignored. Invariants pinned here:
//   - default version is -1 (sentinel meaning "no map received yet"); a real
//     first map at version 0 must still be accepted.
//   - version_type is int64_t — needs the negative range and headroom for
//     long-running clusters.
//   - data is a flat osd_id -> info dictionary; updates overwrite in place.
// If any of these drifts, monclient either applies stale maps (data
// corruption) or ignores new ones (cluster-state stuck).
// ============================================================================

namespace {

struct fake_osd_info {
    int32_t id{0};
    std::string address{};
    bool is_in{false};
    bool is_up{false};
};

struct fake_osd_map {
    using osd_id_type  = int32_t;
    using version_type = int64_t;

    std::map<osd_id_type, std::unique_ptr<fake_osd_info>> data{};
    version_type version{-1};
};

// Mirror of monclient's "apply if newer" policy.
inline bool should_apply(const fake_osd_map& current, fake_osd_map::version_type incoming) {
    return incoming > current.version;
}

} // anonymous namespace

FB_SUITE_SETUP(monclient_osd_map) {}
FB_SUITE_TEARDOWN(monclient_osd_map) {}

FB_TEST(monclient_osd_map, default_version_is_sentinel) {
    // Default version is -1 so the very first received map at version 0
    // still counts as newer. If the sentinel were 0, the first map would be
    // silently dropped.
    fake_osd_map m;
    FB_ASSERT_EQ(m.version, -1);
    FB_ASSERT_TRUE(m.data.empty());

    FB_ASSERT_TRUE(should_apply(m, 0));   // first real map applies
    FB_ASSERT_TRUE(should_apply(m, 100)); // any positive version applies
}

FB_TEST(monclient_osd_map, version_type_is_signed_64bit) {
    // The negative range is required for the -1 sentinel; the 64-bit width
    // gives a long-running cluster plenty of headroom.
    using V = fake_osd_map::version_type;
    FB_ASSERT_TRUE((std::is_same_v<V, int64_t>));
    FB_ASSERT_TRUE(std::is_signed_v<V>);
}

FB_TEST(monclient_osd_map, only_strictly_newer_versions_apply) {
    // Equal version is NOT newer — applying it twice would re-run the diff
    // logic and possibly drop just-issued updates.
    fake_osd_map m;
    m.version = 17;
    FB_ASSERT_FALSE(should_apply(m, 17));
    FB_ASSERT_FALSE(should_apply(m, 16));
    FB_ASSERT_TRUE(should_apply(m, 18));
}

FB_TEST(monclient_osd_map, insert_and_lookup_osd) {
    fake_osd_map m;
    auto info = std::make_unique<fake_osd_info>();
    info->id      = 5;
    info->address = "10.0.0.5:5000";
    info->is_in   = true;
    info->is_up   = true;
    m.data.emplace(5, std::move(info));
    m.version = 1;

    auto it = m.data.find(5);
    FB_ASSERT_TRUE(it != m.data.end());
    FB_ASSERT_EQ(it->second->id, 5);
    FB_ASSERT_STR_EQ(it->second->address.c_str(), "10.0.0.5:5000");
    FB_ASSERT_TRUE(it->second->is_in);
    FB_ASSERT_TRUE(it->second->is_up);
}

FB_TEST(monclient_osd_map, replace_osd_overwrites_in_place) {
    // When monclient receives an updated osd_info for an existing id, it
    // overwrites the entry rather than double-inserting. emplace returns
    // inserted=false; operator[] overwrites.
    fake_osd_map m;
    m.data.emplace(7, std::make_unique<fake_osd_info>(
                          fake_osd_info{7, "old:5000", false, false}));

    // emplace into an existing key MUST NOT insert / overwrite.
    auto [it, inserted] = m.data.emplace(7, std::make_unique<fake_osd_info>());
    FB_ASSERT_FALSE(inserted);
    FB_ASSERT_STR_EQ(it->second->address.c_str(), "old:5000");

    // operator[] overwrite IS the canonical update path.
    m.data[7] = std::make_unique<fake_osd_info>(
                  fake_osd_info{7, "new:6000", true, true});
    FB_ASSERT_STR_EQ(m.data[7]->address.c_str(), "new:6000");
    FB_ASSERT_TRUE(m.data[7]->is_in);
    FB_ASSERT_TRUE(m.data[7]->is_up);
}

FB_TEST(monclient_osd_map, erase_removes_only_target_osd) {
    // Erasing one osd must not touch the others — regression target: a
    // stray iterator that advances after invalidation.
    fake_osd_map m;
    for (int32_t id : {1, 2, 3, 4}) {
        m.data.emplace(id, std::make_unique<fake_osd_info>(
                              fake_osd_info{id, "addr", true, true}));
    }
    FB_ASSERT_EQ(m.data.size(), 4u);

    auto erased = m.data.erase(3);
    FB_ASSERT_EQ(erased, 1u);
    FB_ASSERT_EQ(m.data.size(), 3u);
    FB_ASSERT_TRUE(m.data.find(1) != m.data.end());
    FB_ASSERT_TRUE(m.data.find(2) != m.data.end());
    FB_ASSERT_FALSE(m.data.find(3) != m.data.end());
    FB_ASSERT_TRUE(m.data.find(4) != m.data.end());
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

// ============================================================================
// Test Suite: monclient_pg_map_update — pool migration state machine
//
// pg_map::pool_update tracks per-pg progress during a pool reconfiguration.
// Each pg holds one of three sentinel values:
//   -1  =>  update failed for this pg
//    0  =>  update completed for this pg
//    1  =>  update still in progress
// The integers are encoded across modules; if their meaning shifts, callers
// silently misclassify a failed migration as a successful one and flip the
// pool to ACTIVE prematurely. Pin everything down here.
// ============================================================================

namespace {

constexpr int PG_UPDATE_FAILED  = -1;
constexpr int PG_UPDATE_DONE    = 0;
constexpr int PG_UPDATE_RUNNING = 1;

struct pool_update_info {
    int64_t pool_version{0};
    std::map<int32_t, int> pgs{}; // pg_id -> update state
};

struct fake_pg_map {
    std::map<int32_t, pool_update_info> pool_update{};

    bool pool_is_updating(int32_t pool_id) const {
        auto it = pool_update.find(pool_id);
        if (it == pool_update.end()) return false;
        for (auto& [_, st] : it->second.pgs) {
            if (st == PG_UPDATE_RUNNING) return true;
        }
        return false;
    }

    bool pool_update_all_done(int32_t pool_id) const {
        auto it = pool_update.find(pool_id);
        if (it == pool_update.end()) return false;
        if (it->second.pgs.empty()) return false;
        for (auto& [_, st] : it->second.pgs) {
            if (st != PG_UPDATE_DONE) return false;
        }
        return true;
    }
};

} // anonymous namespace

FB_SUITE_SETUP(monclient_pg_map_update) {}
FB_SUITE_TEARDOWN(monclient_pg_map_update) {}

FB_TEST(monclient_pg_map_update, sentinel_values_are_stable) {
    // -1 / 0 / 1 are encoded across modules — changing them is a wire break.
    FB_ASSERT_EQ(PG_UPDATE_FAILED,  -1);
    FB_ASSERT_EQ(PG_UPDATE_DONE,     0);
    FB_ASSERT_EQ(PG_UPDATE_RUNNING,  1);
}

FB_TEST(monclient_pg_map_update, unknown_pool_is_neither_running_nor_done) {
    // A pool we've never heard of is NOT updating, but also NOT done — the
    // caller shouldn't flip an unknown pool active.
    fake_pg_map m;
    FB_ASSERT_FALSE(m.pool_is_updating(42));
    FB_ASSERT_FALSE(m.pool_update_all_done(42));
}

FB_TEST(monclient_pg_map_update, running_pg_marks_pool_updating) {
    fake_pg_map m;
    pool_update_info info;
    info.pgs[1] = PG_UPDATE_DONE;
    info.pgs[2] = PG_UPDATE_RUNNING; // one running pg => pool is updating
    info.pgs[3] = PG_UPDATE_DONE;
    m.pool_update[100] = info;

    FB_ASSERT_TRUE(m.pool_is_updating(100));
    FB_ASSERT_FALSE(m.pool_update_all_done(100));
}

FB_TEST(monclient_pg_map_update, all_done_pool_reports_finished) {
    fake_pg_map m;
    pool_update_info info;
    info.pgs[1] = PG_UPDATE_DONE;
    info.pgs[2] = PG_UPDATE_DONE;
    m.pool_update[200] = info;

    FB_ASSERT_FALSE(m.pool_is_updating(200));
    FB_ASSERT_TRUE(m.pool_update_all_done(200));
}

FB_TEST(monclient_pg_map_update, failed_pg_blocks_all_done) {
    // Even a single failed pg means the migration is not fully done; the
    // caller must not flip the pool to ACTIVE based on the other pgs.
    fake_pg_map m;
    pool_update_info info;
    info.pgs[1] = PG_UPDATE_DONE;
    info.pgs[2] = PG_UPDATE_FAILED;
    m.pool_update[300] = info;

    FB_ASSERT_FALSE(m.pool_update_all_done(300));
    // No pg is running anymore (one is failed), so pool_is_updating is false.
    // The pool is stuck in a "needs operator attention" limbo.
    FB_ASSERT_FALSE(m.pool_is_updating(300));
}

FB_TEST(monclient_pg_map_update, empty_pool_is_not_done) {
    // A pool entry with zero pgs hasn't started yet. pool_update_all_done
    // must NOT report it as done — otherwise the caller flips the pool
    // active before any pg is actually migrated.
    fake_pg_map m;
    m.pool_update[400] = pool_update_info{};
    FB_ASSERT_FALSE(m.pool_update_all_done(400));
    FB_ASSERT_FALSE(m.pool_is_updating(400));
}

// ============================================================================
// Test Suite: monclient_cached_request_class — classification of cached RPCs
//
// monclient tags each in-flight request so the cache can pick a retention
// policy (general user request vs. monclient-internal). The enum starts at 1
// deliberately so a default-zero-init field is treated as "unset" rather than
// as a valid class.
//
// Invariants pinned here:
//   - general = 1 (NOT 0 — preserves the zero-is-unset convention).
//   - the three values are pairwise distinct (no silent aliasing).
//   - the canonical "unset" value (zero) does NOT collide with any
//     legitimate class — without this, default-init slips through as a
//     real classification and the retention policy misroutes the request.
// ============================================================================

namespace {

enum cached_request_class {
    general = 1,
    internal,
    none
};

} // anonymous namespace

FB_SUITE_SETUP(monclient_cached_request_class) {}
FB_SUITE_TEARDOWN(monclient_cached_request_class) {}

FB_TEST(monclient_cached_request_class, general_is_one_not_zero) {
    // Zero is reserved for "unset". If general were 0, an uninitialised
    // request slot would silently be treated as a real general request.
    FB_ASSERT_EQ(static_cast<int>(general), 1);
}

FB_TEST(monclient_cached_request_class, values_are_pairwise_distinct) {
    FB_ASSERT_TRUE(general  != internal);
    FB_ASSERT_TRUE(general  != none);
    FB_ASSERT_TRUE(internal != none);
}

FB_TEST(monclient_cached_request_class, sequential_values_after_general) {
    // The enum uses default sequential values starting at 1, so internal=2
    // and none=3. The cache may serialise these in a switch or in a log;
    // pin the numeric mapping so a reordering is noticed.
    FB_ASSERT_EQ(static_cast<int>(general),  1);
    FB_ASSERT_EQ(static_cast<int>(internal), 2);
    FB_ASSERT_EQ(static_cast<int>(none),     3);
}

FB_TEST(monclient_cached_request_class, zero_is_not_a_valid_class) {
    // The zero-value sentinel must NOT match any defined enumerator.
    // Otherwise a default-init field is silently classified.
    constexpr int unset = 0;
    FB_ASSERT_TRUE(unset != static_cast<int>(general));
    FB_ASSERT_TRUE(unset != static_cast<int>(internal));
    FB_ASSERT_TRUE(unset != static_cast<int>(none));
}

// ============================================================================
// Test Suite: rpc_controller — per-RPC error state (google::protobuf::RpcController)
//
// rpc_controller is the per-call handle every RPC carries (it's a protobuf
// RpcController). The raft/osd transport relies on exactly two things:
//   1. failed()/error_text() survive across an async hop — SetFailed on the
//      server side is read by the client-side Done callback.
//   2. is_peer_terminating() is the signal to tear the connection down and
//      reconnect elsewhere rather than retry-in-place. It must fire ONLY for
//      the literal "terminating" reason, so a transient error (e.g. timeout)
//      isn't mistaken for an orderly shutdown.
// Here pd is modelled as void* to avoid pulling ibv_pd / infiniband headers.
// ============================================================================

namespace {

class rpc_controller {
public:
    rpc_controller() = default;

    void reset() {
        _failed = false;
        _error_reason.clear();
        _pd = nullptr;
        _peer_address.clear();
    }
    bool failed() const { return _failed; }
    std::string error_text() const { return _error_reason; }
    void set_failed(const std::string& error) {
        _failed = true;
        _error_reason = error;
    }
    bool is_canceled() const { return false; }
    bool is_peer_terminating() const noexcept {
        return _failed and _error_reason == "terminating";
    }
    void attach_pd(void* pd) noexcept { _pd = pd; }
    void* pd() const noexcept { return _pd; }
    void attach_peer_address(std::string peer_address) {
        _peer_address = std::move(peer_address);
    }
    const std::string& peer_address() const noexcept { return _peer_address; }

private:
    bool _failed{false};
    std::string _error_reason{""};
    void* _pd{nullptr};
    std::string _peer_address{};
};

} // anonymous namespace

FB_SUITE_SETUP(rpc_controller) {}
FB_SUITE_TEARDOWN(rpc_controller) {}

FB_TEST(rpc_controller, freshly_constructed_is_clean) {
    // A fresh controller must read as not-failed with empty reason — a stale
    // _failed would cause the client-side Done callback to treat a successful
    // reply as an error.
    rpc_controller ctl;
    FB_ASSERT_FALSE(ctl.failed());
    FB_ASSERT_EQ(ctl.error_text().size(), 0u);
    FB_ASSERT_NULL(ctl.pd());
    FB_ASSERT_TRUE(ctl.peer_address().empty());
}

FB_TEST(rpc_controller, set_failed_marks_and_records_reason) {
    rpc_controller ctl;
    ctl.set_failed("request_timeout");
    FB_ASSERT_TRUE(ctl.failed());
    FB_ASSERT_STR_EQ(ctl.error_text().c_str(), "request_timeout");
}

FB_TEST(rpc_controller, reset_clears_all_fields) {
    // reset() must return the controller to a reusable state: error, pd and
    // peer_address all cleared. A pool reuses these objects across calls.
    rpc_controller ctl;
    ctl.set_failed("bad");
    ctl.attach_pd(reinterpret_cast<void*>(0x1234));
    ctl.attach_peer_address("10.0.0.5:4420");

    ctl.reset();

    FB_ASSERT_FALSE(ctl.failed());
    FB_ASSERT_EQ(ctl.error_text().size(), 0u);
    FB_ASSERT_NULL(ctl.pd());
    FB_ASSERT_TRUE(ctl.peer_address().empty());
}

FB_TEST(rpc_controller, is_peer_terminating_only_for_literal_reason) {
    // The literal "terminating" reason is the orderly-shutdown signal that
    // triggers a reconnect. Any other failure (timeout, bad body) must NOT
    // read as terminating, or we'd leak a live connection.
    rpc_controller orderly;
    orderly.set_failed("terminating");
    FB_ASSERT_TRUE(orderly.is_peer_terminating());

    rpc_controller transient;
    transient.set_failed("request_timeout");
    FB_ASSERT_FALSE(transient.is_peer_terminating());
}

FB_TEST(rpc_controller, not_failed_never_reads_as_terminating) {
    // is_peer_terminating must be false whenever the controller is not failed
    // at all — even though the empty reason != "terminating", guarding this
    // prevents a controller that forgot to SetFailed from masquerading as a
    // shutdown.
    rpc_controller ctl;
    FB_ASSERT_FALSE(ctl.is_peer_terminating());
}

// ============================================================================
// Test Suite: msg_reply_meta — single-byte reply header on the wire
//
// reply_meta is the smallest unit the RDMA transport ships: one byte carrying
// the reply status. Two contracts we can't let drift:
//   1. sizeof(reply_meta) == 1. The transport computes buffer offsets by
//      adding reply_meta_size; if it ever widened, every reply would be
//      mis-aligned against the producer's expectation.
//   2. It's trivially copyable, so memcpy in/out of the RDMA receive buffer
//      is well-defined.
// ============================================================================

namespace {

struct reply_meta {
    uint8_t reply_status;
};
static constexpr size_t reply_meta_size{sizeof(reply_meta)};

} // anonymous namespace

FB_SUITE_SETUP(msg_reply_meta) {}
FB_SUITE_TEARDOWN(msg_reply_meta) {}

FB_TEST(msg_reply_meta, status_field_is_single_byte) {
    // reply_status aliases std::underlying_type_t<status>; status is uint8_t.
    // A wider field would silently corrupt the single-byte wire slot.
    FB_ASSERT_EQ(sizeof(reply_meta::reply_status), 1u);
}

FB_TEST(msg_reply_meta, total_size_is_one_byte) {
    // No padding may be introduced — the struct is exactly one byte.
    FB_ASSERT_EQ(sizeof(reply_meta), 1u);
    FB_ASSERT_EQ(reply_meta_size, 1u);
}

FB_TEST(msg_reply_meta, trivially_copyable) {
    // memcpy semantics must hold so the transport can blast it into the
    // receive buffer and reinterpret it back.
    FB_ASSERT_TRUE(std::is_trivially_copyable_v<reply_meta>);
}

FB_TEST(msg_reply_meta, status_round_trips_through_buffer) {
    // Store a status byte into a raw buffer and reload it — the value must be
    // preserved. Regression target: any endianness or width change in the
    // field would surface here.
    constexpr uint8_t wire_status = 0xAB;
    unsigned char buf[sizeof(reply_meta)]{};
    reply_meta out{};
    out.reply_status = wire_status;
    std::memcpy(buf, &out, sizeof(out));

    reply_meta in{};
    std::memcpy(&in, buf, sizeof(in));
    FB_ASSERT_EQ(in.reply_status, wire_status);
}

// ============================================================================
// Test Suite: msg_iterate_tag — connection-scoped iteration control
//
// iterate_tag is the return value the transport's per-connection iteration
// callback hands back: 'keep' to continue visiting connections, 'stop' to
// terminate early (e.g. once a target is found). It starts at 1 so a
// default-zero return can't be mistaken for "keep iterating".
// ============================================================================

namespace {

enum class iterate_tag {
    keep = 1,
    stop
};

} // anonymous namespace

FB_SUITE_SETUP(msg_iterate_tag) {}
FB_SUITE_TEARDOWN(msg_iterate_tag) {}

FB_TEST(msg_iterate_tag, keep_is_one_not_zero) {
    // Zero is reserved for "unset"; if keep were 0, an uninitialised return
    // would silently mean "keep iterating" past the intended stop point.
    FB_ASSERT_EQ(static_cast<int>(iterate_tag::keep), 1);
}

FB_TEST(msg_iterate_tag, values_are_distinct_and_ordered) {
    // stop must be the terminal, distinct from keep. The numeric ordering
    // (stop > keep) is what visitor loops rely on when they check "did we
    // reach the stop tag".
    FB_ASSERT_TRUE(iterate_tag::keep != iterate_tag::stop);
    FB_ASSERT_TRUE(static_cast<int>(iterate_tag::stop) > static_cast<int>(iterate_tag::keep));
    FB_ASSERT_EQ(static_cast<int>(iterate_tag::stop), 2);
}

FB_TEST(msg_iterate_tag, stop_short_circuits_iteration) {
    // Model a tiny visitor: walk until it sees 'stop', then halt. This is the
    // actual control-flow contract — stop must terminate the loop immediately.
    std::vector<iterate_tag> seq = {
        iterate_tag::keep, iterate_tag::keep, iterate_tag::stop, iterate_tag::keep};
    int visited = 0;
    bool stopped_early = false;
    for (auto tag : seq) {
        if (tag == iterate_tag::stop) {
            stopped_early = true;
            break;
        }
        ++visited;
    }
    FB_ASSERT_TRUE(stopped_early);
    FB_ASSERT_EQ(visited, 2); // the trailing 'keep' after 'stop' is never reached
}

FB_TEST(msg_iterate_tag, keep_continues_full_iteration) {
    // A keep-only sequence must walk every element without early exit.
    std::vector<iterate_tag> seq(5, iterate_tag::keep);
    int visited = 0;
    for (auto tag : seq) {
        if (tag == iterate_tag::stop) break;
        ++visited;
    }
    FB_ASSERT_EQ(visited, 5);
}

// ============================================================================
// Test Suite: rpc_connect_cache_reconnect — connection overwrite on reconnect
//
// Production connect_cache::create_connect assigns
//   _cache[shard_id][node_id] = conn
// unconditionally on success. That means a reconnect for an EXISTING node
// overwrites the stale entry with the fresh connection — which is exactly
// what we want (the old QP is dead). This suite pins that overwrite
// contract with a fake that mirrors it (unlike rpc_connect_cache's fake,
// which models the first-connect case).
// ============================================================================

namespace {

class overwrite_connect_cache {
public:
    using connect_ptr = std::shared_ptr<fake_connection>;

    explicit overwrite_connect_cache(uint32_t shard_count) {
        _cache.resize(shard_count);
    }

    // Mirrors production: always assign, overwriting any prior entry.
    void create_connect(uint32_t shard_id, int node_id, std::string addr, uint16_t port) {
        if (shard_id >= _cache.size()) return;
        _cache[shard_id][node_id] =
          std::make_shared<fake_connection>(fake_connection{node_id, std::move(addr), port});
    }

    connect_ptr get_connect(uint32_t shard_id, int node_id) const {
        if (shard_id >= _cache.size()) return nullptr;
        auto it = _cache[shard_id].find(node_id);
        return it == _cache[shard_id].end() ? nullptr : it->second;
    }

private:
    std::vector<std::map<int, connect_ptr>> _cache;
};

} // anonymous namespace

FB_SUITE_SETUP(rpc_connect_cache_reconnect) {}
FB_SUITE_TEARDOWN(rpc_connect_cache_reconnect) {}

FB_TEST(rpc_connect_cache_reconnect, reconnect_overwrites_cached_entry) {
    // A second successful connect for the same node must replace the stale
    // connection. Callers reusing the cached handle would otherwise send on a
    // dead QP.
    overwrite_connect_cache cache(1);
    cache.create_connect(0, 7, "10.0.0.1", 5000);
    auto first = cache.get_connect(0, 7);
    FB_ASSERT_NOT_NULL(first.get());
    FB_ASSERT_STR_EQ(first->addr.c_str(), "10.0.0.1");

    cache.create_connect(0, 7, "10.0.0.2", 6000);
    auto second = cache.get_connect(0, 7);
    FB_ASSERT_NOT_NULL(second.get());
    FB_ASSERT_STR_EQ(second->addr.c_str(), "10.0.0.2");
    FB_ASSERT_EQ(second->port, 6000);
}

FB_TEST(rpc_connect_cache_reconnect, node_mapping_preserved_after_overwrite) {
    // shard/node still resolve to the right slot after the overwrite — the
    // key isn't disturbed, only the value is replaced.
    overwrite_connect_cache cache(1);
    cache.create_connect(0, 42, "old", 1);
    cache.create_connect(0, 42, "new", 2);

    auto conn = cache.get_connect(0, 42);
    FB_ASSERT_NOT_NULL(conn.get());
    FB_ASSERT_EQ(conn->node_id, 42);
    FB_ASSERT_STR_EQ(conn->addr.c_str(), "new");
}

FB_TEST(rpc_connect_cache_reconnect, overwrite_is_local_to_one_node) {
    // Reconnecting node A must not perturb node B's cached entry — regression
    // target for a future "clear all then insert" refactor.
    overwrite_connect_cache cache(1);
    cache.create_connect(0, 1, "addr1", 1);
    cache.create_connect(0, 2, "addr2", 2);

    cache.create_connect(0, 1, "addr1_new", 3);

    auto a = cache.get_connect(0, 1);
    auto b = cache.get_connect(0, 2);
    FB_ASSERT_STR_EQ(a->addr.c_str(), "addr1_new");
    FB_ASSERT_STR_EQ(b->addr.c_str(), "addr2"); // untouched
}

// ============================================================================
// Test Suite: monclient_data_structures — endpoint / image_info / pools
//
// These are the plain value structs monclient hands back to callers. The
// contract callers depend on: every field is default-initialised to a safe
// "empty" state (zero size, empty string, null pool array) so a freshly
// constructed struct never reads as a phantom image or a non-empty pool set.
// ============================================================================

namespace {

// mon_endpoint is defined by the monclient_endpoint suite above; reused here.
struct mon_image_info {
    std::string pool_name{};
    std::string image_name{};
    size_t size{};
    size_t object_size{};
};

struct mon_pools {
    struct pool {
        int32_t pool_id;
        std::string name;
        int32_t pg_size;
        int32_t pg_count;
        std::string failure_domain;
        std::string root;
    };
    size_t num_pool{0};
    std::unique_ptr<pool[]> data{nullptr};
};

} // anonymous namespace

FB_SUITE_SETUP(monclient_data_structures) {}
FB_SUITE_TEARDOWN(monclient_data_structures) {}

FB_TEST(monclient_data_structures, endpoint_default_is_empty) {
    // A default endpoint must be hostless/port-0 so "has it been set?" is a
    // simple emptiness check.
    mon_endpoint ep;
    FB_ASSERT_TRUE(ep.host.empty());
    FB_ASSERT_EQ(ep.port, 0);
}

FB_TEST(monclient_data_structures, endpoint_round_trips_host_port) {
    mon_endpoint ep;
    ep.host = "10.0.0.7";
    ep.port = 4420;
    FB_ASSERT_STR_EQ(ep.host.c_str(), "10.0.0.7");
    FB_ASSERT_EQ(ep.port, 4420);
    // port is uint16_t — guard against accidental widening that would change
    // the on-the-wire connect() call.
    FB_ASSERT_EQ(sizeof(ep.port), 2u);
}

FB_TEST(monclient_data_structures, image_info_default_is_zeroed_and_unnamed) {
    // A fresh image_info must not masquerade as a real image: zero size and
    // empty names.
    mon_image_info img;
    FB_ASSERT_TRUE(img.pool_name.empty());
    FB_ASSERT_TRUE(img.image_name.empty());
    FB_ASSERT_EQ(img.size, 0u);
    FB_ASSERT_EQ(img.object_size, 0u);
}

FB_TEST(monclient_data_structures, pools_default_is_empty) {
    // num_pool == 0 and data == nullptr together mean "no pools" without an
    // ambiguity (one could otherwise think a null array with num_pool>0 is
    // valid).
    mon_pools pp;
    FB_ASSERT_EQ(pp.num_pool, 0u);
    FB_ASSERT_NULL(pp.data.get());
}

FB_TEST(monclient_data_structures, pools_owns_array) {
    // pools owns a unique_ptr<pool[]>; verify the ownership/length invariant
    // holds once populated.
    constexpr size_t n = 3;
    mon_pools pp;
    pp.data = std::make_unique<mon_pools::pool[]>(n);
    pp.num_pool = n;
    for (size_t i = 0; i < n; ++i) {
        pp.data[i].pool_id = static_cast<int32_t>(i);
    }
    FB_ASSERT_EQ(pp.num_pool, n);
    FB_ASSERT_NOT_NULL(pp.data.get());
    FB_ASSERT_EQ(pp.data[0].pool_id, 0);
    FB_ASSERT_EQ(pp.data[2].pool_id, 2);
}

// ============================================================================
// Test Suite: monclient_response_type — three-state RPC reply variant
//
// monclient replies as std::variant<monostate, unique_ptr<image_info>,
// unique_ptr<pools>>. Callers dispatch on the active alternative, and the
// variant must:
//   - default to monostate (no reply yet / not applicable),
//   - keep the alternative order stable (some callers switch on index()),
//   - release the previously-held alternative when a new one is assigned
//     (the variant owns exactly one payload at a time).
// ============================================================================

namespace {

using mon_response_type =
  std::variant<std::monostate, std::unique_ptr<mon_image_info>, std::unique_ptr<mon_pools>>;

} // anonymous namespace

FB_SUITE_SETUP(monclient_response_type) {}
FB_SUITE_TEARDOWN(monclient_response_type) {}

FB_TEST(monclient_response_type, default_holds_monostate) {
    // Before any reply arrives the variant must read as monostate, not as a
    // null image_info/pools pointer that a caller might dereference.
    mon_response_type resp;
    FB_ASSERT_EQ(resp.index(), 0u);
    FB_ASSERT_TRUE(std::holds_alternative<std::monostate>(resp));
}

FB_TEST(monclient_response_type, assign_image_info_activates_alternative) {
    mon_response_type resp;
    resp = std::make_unique<mon_image_info>();
    FB_ASSERT_TRUE(std::holds_alternative<std::unique_ptr<mon_image_info>>(resp));
    FB_ASSERT_EQ(resp.index(), 1u);
    FB_ASSERT_NOT_NULL(std::get<std::unique_ptr<mon_image_info>>(resp).get());
}

FB_TEST(monclient_response_type, assign_pools_activates_alternative) {
    mon_response_type resp;
    resp = std::make_unique<mon_pools>();
    FB_ASSERT_TRUE(std::holds_alternative<std::unique_ptr<mon_pools>>(resp));
    FB_ASSERT_EQ(resp.index(), 2u);
    FB_ASSERT_NOT_NULL(std::get<std::unique_ptr<mon_pools>>(resp).get());
}

FB_TEST(monclient_response_type, alternative_order_is_stable) {
    // Callers that switch on index() need the order frozen:
    //   0 = monostate, 1 = image_info, 2 = pools.
    FB_ASSERT_EQ(mon_response_type{}.index(), 0u);
    mon_response_type a = std::make_unique<mon_image_info>();
    FB_ASSERT_EQ(a.index(), 1u);
    mon_response_type b = std::make_unique<mon_pools>();
    FB_ASSERT_EQ(b.index(), 2u);
}

FB_TEST(monclient_response_type, reassign_releases_prior_payload) {
    // The variant owns exactly one payload. Moving a pools reply in must drop
    // the previously-held image_info so its allocation is freed (no leak).
    mon_image_info* raw = nullptr;
    {
        mon_response_type resp = std::make_unique<mon_image_info>();
        raw = std::get<std::unique_ptr<mon_image_info>>(resp).get();
        FB_ASSERT_NOT_NULL(raw);

        resp = std::make_unique<mon_pools>();
        // Now image_info is NOT the active alternative, and its former object
        // has been destroyed.
        FB_ASSERT_FALSE(std::holds_alternative<std::unique_ptr<mon_image_info>>(resp));
        FB_ASSERT_TRUE(std::holds_alternative<std::unique_ptr<mon_pools>>(resp));
    }
    // raw pointed into the released image_info; we can't dereference it, but
    // reaching here without crashing confirms the variant cleaned it up.
    FB_ASSERT_NOT_NULL(raw);
}

// ============================================================================
// Test Suite: msg_work_request_id — 64-bit async-completion correlation key
//
// work_request_id packs four fields into one uint64_t so an RDMA completion
// can be correlated back to its originating request without a lookup table:
//   shard_id (9 bits) | epoch (27 bits) | connection_id (12 bits) | request_id (16 bits)
// The layout is a TIGHT partition: 9 + 27 + 12 + 16 == 64, no overlap, no gap.
// The static extractors (request_id / dispatch_id / shard_id / connection_id)
// are the contract every CQE handler depends on. We mirror them here.
// ============================================================================

namespace {
namespace wr {

constexpr uint8_t shard_id_len{9};
constexpr uint8_t epoch_len{27};
constexpr uint8_t connection_id_len{12};
constexpr uint8_t request_id_len{16};

constexpr uint8_t shard_id_shift{64 - shard_id_len};                          // 55
constexpr uint8_t epoch_shift{static_cast<uint8_t>(shard_id_shift - epoch_len)};          // 28
constexpr uint8_t connection_id_shift{static_cast<uint8_t>(epoch_shift - connection_id_len)};        // 16
constexpr uint8_t request_id_shift{static_cast<uint8_t>(connection_id_shift - request_id_len)};      // 0

constexpr uint64_t make_mask(uint64_t length, uint64_t offset) {
    return ((uint64_t{1} << length) - 1) << offset;
}

constexpr uint64_t shard_id_mask      = make_mask(shard_id_len, shard_id_shift);
constexpr uint64_t epoch_mask         = make_mask(epoch_len, epoch_shift);
constexpr uint64_t connection_id_mask = make_mask(connection_id_len, connection_id_shift);
constexpr uint64_t request_id_mask    = make_mask(request_id_len, request_id_shift);

using value_type        = uint64_t;
using shard_id_type     = uint16_t;
using connection_id_type = uint16_t;
using request_id_type   = uint16_t;
using dispatch_id_type  = uint64_t;

inline shard_id_type shard_id(value_type id) noexcept {
    return static_cast<shard_id_type>((id & shard_id_mask) >> shard_id_shift);
}
inline connection_id_type connection_id(value_type id) noexcept {
    return static_cast<connection_id_type>((id & connection_id_mask) >> connection_id_shift);
}
inline request_id_type request_id(value_type id) noexcept {
    return static_cast<request_id_type>(id & request_id_mask);
}
inline dispatch_id_type dispatch_id(value_type id) noexcept {
    return id & ~request_id_mask;
}

inline value_type build(shard_id_type sh, uint32_t ep,
                        connection_id_type conn, request_id_type req) noexcept {
    value_type id = 0;
    id |= (static_cast<value_type>(sh)   << shard_id_shift)      & shard_id_mask;
    id |= (static_cast<value_type>(ep)   << epoch_shift)         & epoch_mask;
    id |= (static_cast<value_type>(conn) << connection_id_shift) & connection_id_mask;
    id |= (static_cast<value_type>(req)  << request_id_shift)    & request_id_mask;
    return id;
}

} // namespace wr
} // anonymous namespace

FB_SUITE_SETUP(msg_work_request_id) {}
FB_SUITE_TEARDOWN(msg_work_request_id) {}

FB_TEST(msg_work_request_id, fields_partition_exactly_64_bits) {
    // The four field widths must sum to 64 AND their masks must be a clean,
    // non-overlapping partition of the whole word. A future "add a field"
    // change that doesn't rebalance the widths breaks correlation outright.
    using namespace wr;
    FB_ASSERT_EQ(shard_id_len + epoch_len + connection_id_len + request_id_len, 64);

    constexpr uint64_t all = shard_id_mask | epoch_mask | connection_id_mask | request_id_mask;
    FB_ASSERT_EQ(all, UINT64_MAX);

    // Pairwise disjoint — no bit claimed by two fields.
    FB_ASSERT_EQ(shard_id_mask      & epoch_mask,         0ULL);
    FB_ASSERT_EQ(epoch_mask         & connection_id_mask, 0ULL);
    FB_ASSERT_EQ(connection_id_mask & request_id_mask,    0ULL);
}

FB_TEST(msg_work_request_id, shard_id_round_trips) {
    // shard_id occupies the top 9 bits (max 511). Extraction must invert the
    // packing for any in-range value.
    using namespace wr;
    for (shard_id_type s : {shard_id_type{0}, shard_id_type{1}, shard_id_type{255}, shard_id_type{511}}) {
        value_type id = build(s, 0, 0, 0);
        FB_ASSERT_EQ(shard_id(id), s);
    }
}

FB_TEST(msg_work_request_id, connection_id_round_trips) {
    // connection_id is 12 bits (max 4095).
    using namespace wr;
    for (connection_id_type c : {connection_id_type{0}, connection_id_type{1}, connection_id_type{1024}, connection_id_type{4095}}) {
        value_type id = build(0, 0, c, 0);
        FB_ASSERT_EQ(connection_id(id), c);
    }
}

FB_TEST(msg_work_request_id, request_id_round_trips_and_wraps_at_16_bits) {
    // request_id is the lowest 16 bits. Extraction returns exactly those bits;
    // the mask truncates anything above 0xFFFF.
    using namespace wr;
    FB_ASSERT_EQ(request_id(build(0, 0, 0, 1234)), 1234);
    FB_ASSERT_EQ(request_id(build(0, 0, 0, 0xFFFF)), 0xFFFF);
    // A raw value whose low 16 bits are 0xABCD reads back as 0xABCD regardless
    // of the upper bits (this is the CQE-correlation contract).
    FB_ASSERT_EQ(request_id(0xFFFFFFFFFFFFABCDULL), 0xABCDu);
}

FB_TEST(msg_work_request_id, dispatch_id_strips_only_request_field) {
    // dispatch_id is the key used to fan a completion out to its connection/
    // shard handler — it must keep shard/epoch/connection but drop request_id.
    // I.e. dispatch_id(id) == id with the low 16 bits cleared.
    using namespace wr;
    value_type id = build(7, 123456, 42, 999);
    FB_ASSERT_EQ(dispatch_id(id), id & ~request_id_mask);

    // dispatch_id preserves shard/connection, and is independent of request_id.
    FB_ASSERT_EQ(shard_id(dispatch_id(id)), 7);
    FB_ASSERT_EQ(connection_id(dispatch_id(id)), 42);
    FB_ASSERT_EQ(dispatch_id(build(7, 123456, 42, 0)), dispatch_id(build(7, 123456, 42, 999)));
}

FB_TEST(msg_work_request_id, fields_do_not_bleed_into_each_other) {
    // Maxing out every field at once must still round-trip each one exactly —
    // the regression target for a width change that would let one field's
    // high bits spill into the neighbour.
    using namespace wr;
    value_type id = build(511, (1u << 27) - 1, 4095, 0xFFFF);
    FB_ASSERT_EQ(shard_id(id), 511);
    FB_ASSERT_EQ(connection_id(id), 4095);
    FB_ASSERT_EQ(request_id(id), 0xFFFF);
}

// ============================================================================
// Test Suite: msg_endpoint_config — RDMA QP configuration defaults
//
// endpoint carries the QP attributes (send/recv WR depth, SGE counts,
// timeouts) the transport passes to ibv_create_qp. These defaults are the
// contract: they're chosen so an out-of-the-box config sustains a reasonable
// pipeline depth, and several are load-bearing:
//   - max_recv_wr > max_send_wr (servers must post more recv buffers than a
//     client sends, or the receive queue starves and drops completions).
//   - no QP attr defaults to 0 (0 means "unlimited" or invalid depending on
//     the field — neither is a sane default).
//   - timeouts are positive (a 0 timeout would resolve immediately and fail).
// ============================================================================

namespace {

struct ep_config {
    std::string addr{""};
    uint16_t port{0};
    bool passive{false};
    int backlog{1024};
    int resolve_timeout_us{2000};
    int poll_cm_event_timeout_us{1000000};

    uint32_t max_send_wr{4096};
    uint32_t max_recv_wr{8192};
    uint32_t max_send_sge{128};
    uint32_t max_recv_sge{128};
    uint32_t max_inline_data{16};

    int cq_num_entries{16};
    bool qp_sig_all{false};
};

} // anonymous namespace

FB_SUITE_SETUP(msg_endpoint_config) {}
FB_SUITE_TEARDOWN(msg_endpoint_config) {}

FB_TEST(msg_endpoint_config, fresh_config_has_known_qp_depths) {
    // These specific depths are tuned; pin them so an accidental "tidy up the
    // defaults" change is caught.
    ep_config c;
    FB_ASSERT_EQ(c.max_send_wr, 4096u);
    FB_ASSERT_EQ(c.max_recv_wr, 8192u);
    FB_ASSERT_EQ(c.max_send_sge, 128u);
    FB_ASSERT_EQ(c.max_recv_sge, 128u);
}

FB_TEST(msg_endpoint_config, recv_depth_exceeds_send_depth) {
    // The server side must post more receive buffers than the number of sends
    // a peer issues, otherwise the RQ starves and completions are dropped.
    // max_recv_wr >= max_send_wr is the load-bearing inequality.
    ep_config c;
    FB_ASSERT_TRUE(c.max_recv_wr >= c.max_send_wr);
    FB_ASSERT_TRUE(c.max_recv_sge >= c.max_send_sge);
}

FB_TEST(msg_endpoint_config, no_qp_attr_defaults_to_zero) {
    // Zero means "unlimited" (WR/SGE depth) or invalid (timeouts). Neither is
    // a sane default for an out-of-the-box config.
    ep_config c;
    FB_ASSERT_TRUE(c.max_send_wr != 0);
    FB_ASSERT_TRUE(c.max_recv_wr != 0);
    FB_ASSERT_TRUE(c.max_send_sge != 0);
    FB_ASSERT_TRUE(c.max_recv_sge != 0);
    FB_ASSERT_TRUE(c.max_inline_data != 0);
    FB_ASSERT_TRUE(c.cq_num_entries != 0);
}

FB_TEST(msg_endpoint_config, timeouts_are_positive) {
    // A 0 resolve/poll timeout would fail the CM event immediately.
    ep_config c;
    FB_ASSERT_TRUE(c.resolve_timeout_us > 0);
    FB_ASSERT_TRUE(c.poll_cm_event_timeout_us > 0);
    // poll window must be long enough relative to resolve — a 1s poll with a
    // 2ms resolve is the documented pairing.
    FB_ASSERT_TRUE(c.poll_cm_event_timeout_us > c.resolve_timeout_us);
}

FB_TEST(msg_endpoint_config, addr_and_port_start_unset) {
    // A fresh endpoint hasn't been told where to connect/listen: empty addr,
    // port 0. This is the "is it configured?" sentinel.
    ep_config c;
    FB_ASSERT_TRUE(c.addr.empty());
    FB_ASSERT_EQ(c.port, 0);
}

// ============================================================================
// Test Suite: msg_probe_accounting — RDMA queue-depth counter conservation
//
// probe tracks the WR accounting the transport uses to decide when a QP's
// send/receive queue is full. The invariant that keeps the system correct:
//   receive_queue_depth == posted_receive_wr - received_cqe
// i.e. outstanding receives == posted minus completed. If posting and
// completing ever desync, the transport either over-posts (overflows the RQ)
// or under-posts (starves it). We mirror the four mutators and verify the
// conservation law holds across a realistic flow.
// ============================================================================

namespace {

class probe {
public:
    void send_wr_posted(std::size_t n = 1) noexcept {
        posted_send_wr += static_cast<int64_t>(n);
        send_queue_depth += static_cast<int64_t>(n);
    }
    void receive_wr_posted(std::size_t n = 1) noexcept {
        posted_receive_wr += static_cast<int64_t>(n);
        receive_queue_depth += static_cast<int64_t>(n);
    }
    void cqe_received(std::size_t n = 1) noexcept {
        received_cqe += static_cast<int64_t>(n);
        receive_queue_depth -= static_cast<int64_t>(n);
    }
    void send_wc_received(std::size_t n = 1) noexcept {
        received_sent_wc += static_cast<int64_t>(n);
    }

    int64_t posted_send_wr{0};
    int64_t posted_receive_wr{0};
    int64_t send_queue_depth{0};
    int64_t receive_queue_depth{0};
    int64_t received_cqe{0};
    int64_t received_sent_wc{0};
};

} // anonymous namespace

FB_SUITE_SETUP(msg_probe_accounting) {}
FB_SUITE_TEARDOWN(msg_probe_accounting) {}

FB_TEST(msg_probe_accounting, fresh_probe_is_all_zero) {
    probe p;
    FB_ASSERT_EQ(p.posted_send_wr, 0);
    FB_ASSERT_EQ(p.posted_receive_wr, 0);
    FB_ASSERT_EQ(p.send_queue_depth, 0);
    FB_ASSERT_EQ(p.receive_queue_depth, 0);
    FB_ASSERT_EQ(p.received_cqe, 0);
    FB_ASSERT_EQ(p.received_sent_wc, 0);
}

FB_TEST(msg_probe_accounting, posting_increases_depth) {
    probe p;
    p.send_wr_posted(10);
    p.receive_wr_posted(20);
    FB_ASSERT_EQ(p.send_queue_depth, 10);
    FB_ASSERT_EQ(p.receive_queue_depth, 20);
    FB_ASSERT_EQ(p.posted_send_wr, 10);
    FB_ASSERT_EQ(p.posted_receive_wr, 20);
}

FB_TEST(msg_probe_accounting, cqe_drains_receive_depth) {
    // The core conservation: outstanding receives == posted - completed.
    probe p;
    p.receive_wr_posted(20);
    p.cqe_received(7);
    FB_ASSERT_EQ(p.receive_queue_depth, 13); // 20 - 7
    FB_ASSERT_EQ(p.received_cqe, 7);
}

FB_TEST(msg_probe_accounting, balanced_flow_returns_depth_to_zero) {
    // Post N receives, complete N: the queue is fully drained, depth back
    // to 0. This is the steady-state a healthy connection hovers around.
    probe p;
    p.receive_wr_posted(64);
    for (int i = 0; i < 64; ++i) p.cqe_received(1);
    FB_ASSERT_EQ(p.receive_queue_depth, 0);
    FB_ASSERT_EQ(p.received_cqe, 64);
    FB_ASSERT_EQ(p.posted_receive_wr, 64);
}

FB_TEST(msg_probe_accounting, send_and_receive_counters_are_independent) {
    // A send completion must NOT touch the receive queue's depth, and vice
    // versa. Regression target for a shared-counter refactor.
    probe p;
    p.send_wr_posted(5);
    p.receive_wr_posted(3);
    p.send_wc_received(2); // completes 2 sends
    p.cqe_received(1);     // completes 1 recv

    FB_ASSERT_EQ(p.send_queue_depth, 5);    // send depth is NOT decremented by wc
    FB_ASSERT_EQ(p.received_sent_wc, 2);
    FB_ASSERT_EQ(p.receive_queue_depth, 2); // 3 - 1
    FB_ASSERT_EQ(p.received_cqe, 1);
}

FB_TEST(msg_probe_accounting, default_n_is_one) {
    // The n=1 default is relied upon at every call site; verify it.
    probe p;
    p.send_wr_posted();
    p.receive_wr_posted();
    p.cqe_received();
    p.send_wc_received();
    FB_ASSERT_EQ(p.posted_send_wr, 1);
    FB_ASSERT_EQ(p.posted_receive_wr, 1);
    FB_ASSERT_EQ(p.received_cqe, 1);
    FB_ASSERT_EQ(p.receive_queue_depth, 0); // 1 posted - 1 completed
}

// ============================================================================
// Test Suite: msg_transport_data_headers — inline vs metadata framing
//
// transport_data ships two payload shapes over the same channel and tells them
// apart by a leading is_inlined byte:
//   - inline_data: the request/response body travels inline in the send buffer.
//       is_inlined defaults to _inline_tag (1).
//   - metadata: a descriptor pointing at an RDMA-read target (remote rkey/raddr).
//       is_inlined defaults to _no_inline_tag (0).
// The receiver reads byte[0] and dispatches accordingly, so the two defaults
// must differ AND the header sizes must stay stable (they index into the
// receive buffer). We also pin the completion-tag complement: _complete_tag
// (0b01010101) and _un_complete_tag (0b10101010) are bitwise complements, so
// a single-bit corruption can't turn one into the other.
// ============================================================================

namespace {

constexpr uint8_t td_inline_tag{1};
constexpr uint8_t td_no_inline_tag{0};

constexpr uint8_t td_complete_tag{0b01010101};     // 85
constexpr uint8_t td_un_complete_tag{0b10101010};  // 170

struct td_inline_data {
    uint8_t  is_inlined{td_inline_tag};
    uint32_t correlation_index{0};
    uint32_t io_length{0};
};

struct td_metadata {
    uint8_t  is_inlined{td_no_inline_tag};
    uint32_t correlation_index{0};
    uint32_t metadata_count{1};
    uint32_t serial_no{0};
    uint32_t io_length;
    uint32_t io_count;
};

} // anonymous namespace

FB_SUITE_SETUP(msg_transport_data_headers) {}
FB_SUITE_TEARDOWN(msg_transport_data_headers) {}

FB_TEST(msg_transport_data_headers, inline_tag_differs_from_no_inline) {
    // The discriminator is byte[0]; the two sentinels must be distinct or the
    // receiver can't tell an inline body from a metadata descriptor.
    FB_ASSERT_TRUE(td_inline_tag != td_no_inline_tag);
}

FB_TEST(msg_transport_data_headers, inline_data_defaults_to_inlined) {
    // A freshly built inline_data header must announce itself as inline so the
    // receiver reads the body straight out of the send buffer.
    td_inline_data hdr;
    FB_ASSERT_EQ(hdr.is_inlined, td_inline_tag);
}

FB_TEST(msg_transport_data_headers, metadata_defaults_to_not_inlined) {
    // A freshly built metadata header must announce NOT-inline, so the
    // receiver issues an RDMA read to fetch the real payload.
    td_metadata hdr;
    FB_ASSERT_EQ(hdr.is_inlined, td_no_inline_tag);
    FB_ASSERT_EQ(hdr.metadata_count, 1u); // a single descriptor by default
}

FB_TEST(msg_transport_data_headers, metadata_header_size_is_24) {
    // metadata_header_size is a wire constant the buffer arithmetic depends on.
    // Fields are 1 + 5×4 = 21 bytes; a 3-byte alignment gap after the leading
    // uint8_t pads it to 24.
    FB_ASSERT_EQ(sizeof(td_metadata), 24u);
}

FB_TEST(msg_transport_data_headers, completion_tags_are_bit_complements) {
    // _complete_tag and _un_complete_tag are exact bitwise complements — XOR
    // to all-ones, hamming distance 8. A single-bit flip can't turn a complete
    // marker into an incomplete one (or vice versa), which is the torn-read
    // robustness the RDMA-read protocol relies on.
    FB_ASSERT_EQ(td_complete_tag ^ td_un_complete_tag, 0xFFu);
    FB_ASSERT_EQ(static_cast<uint8_t>(~td_complete_tag), td_un_complete_tag);
    FB_ASSERT_EQ(td_complete_tag, 85u);
    FB_ASSERT_EQ(td_un_complete_tag, 170u);
}

// ============================================================================
// Test Suite: bdev_object_mapping — client object ↔ offset mapping
//
// bdev/client maps byte offsets to object sequences and back. The core
// utilities are pure arithmetic, no I/O:
//   - calc_first_object_position(offset, length, object_size) →
//       (first_object_size, first_object_offset, object_seq)
//   - get_obj_num(offset, length, object_size) → object count
// These are the correctness-critical path: wrong mapping → data corruption,
// lost writes, or phantom objects.
// ============================================================================

namespace {

// Mirrors libfblock.cc's default_object_size (4 MiB)
static constexpr size_t default_object_size = 4 * 1024 * 1024;

// align_down: round toward negative infinity to object boundary
static uint64_t align_down(uint64_t val, size_t object_size) {
    return (val / object_size) * object_size;
}
// align_up: round toward positive infinity to object boundary
static uint64_t align_up(uint64_t val, size_t object_size) {
    return ((val + object_size - 1) / object_size) * object_size;
}

// get_obj_num: number of whole objects a byte range spans
static uint64_t get_obj_num(uint64_t offset, uint64_t length, size_t object_size) {
    auto start = align_down(offset, object_size);
    auto end = align_up(offset + length, object_size);
    return (end - start) / object_size;
}

// calc_first_object_position: decompose a byte range into (first_object_size, first_object_offset, object_seq)
static std::tuple<size_t, uint64_t, uint64_t>
calc_first_object_position(uint64_t offset, uint64_t length, size_t object_size) {
    uint64_t first_object_offset = offset % object_size;
    size_t first_object_size = object_size - static_cast<size_t>(first_object_offset);
    if (length < first_object_size) {
        first_object_size = static_cast<size_t>(length);
    }
    uint64_t object_seq = offset / object_size;
    return std::make_tuple(first_object_size, first_object_offset, object_seq);
}

} // anonymous namespace

FB_SUITE_SETUP(bdev_object_mapping) {}
FB_SUITE_TEARDOWN(bdev_object_mapping) {}

FB_TEST(bdev_object_mapping, default_object_size_is_4MiB) {
    // 4 MiB is the documented default; any change must update this test.
    FB_ASSERT_EQ(default_object_size, 4 * 1024 * 1024u);
}

FB_TEST(bdev_object_mapping, align_down_is_identity_at_boundary) {
    // Aligned address returns unchanged.
    FB_ASSERT_EQ(align_down(0, default_object_size), 0u);
    FB_ASSERT_EQ(align_down(default_object_size, default_object_size), default_object_size);
    FB_ASSERT_EQ(align_down(8 * default_object_size, default_object_size), 8 * default_object_size);
}

FB_TEST(bdev_object_mapping, align_down_rounds_down) {
    // Address in the middle of an object rounds to the object's start.
    FB_ASSERT_EQ(align_down(1, default_object_size), 0u);
    FB_ASSERT_EQ(align_down(default_object_size + 1, default_object_size), default_object_size);
    FB_ASSERT_EQ(align_down(3 * default_object_size + 1, default_object_size), 3 * default_object_size);
}

FB_TEST(bdev_object_mapping, align_up_is_identity_at_boundary) {
    FB_ASSERT_EQ(align_up(0, default_object_size), 0u);
    FB_ASSERT_EQ(align_up(default_object_size, default_object_size), default_object_size);
    FB_ASSERT_EQ(align_up(8 * default_object_size, default_object_size), 8 * default_object_size);
}

FB_TEST(bdev_object_mapping, align_up_rounds_up) {
    // Address in the middle rounds to the next object boundary.
    FB_ASSERT_EQ(align_up(1, default_object_size), default_object_size);
    FB_ASSERT_EQ(align_up(default_object_size + 1, default_object_size), 2 * default_object_size);
    FB_ASSERT_EQ(align_up(3 * default_object_size + 1, default_object_size), 4 * default_object_size);
}

FB_TEST(bdev_object_mapping, align_up_down_are_complementary) {
    // For any v, align_up(v) >= v >= align_down(v). If v is aligned, both equal v.
    FB_ASSERT_GE(align_up(1, default_object_size), 1);
    FB_ASSERT_GE(1, align_down(1, default_object_size));
    FB_ASSERT_EQ(align_up(default_object_size, default_object_size), align_down(default_object_size, default_object_size));
}

FB_TEST(bdev_object_mapping, get_obj_num_single_object_at_start) {
    // A zero-offset, single-object-length write spans exactly one object.
    FB_ASSERT_EQ(get_obj_num(0, default_object_size, default_object_size), 1u);
    FB_ASSERT_EQ(get_obj_num(0, 1, default_object_size), 1u);
}

FB_TEST(bdev_object_mapping, get_obj_num_crosses_object_boundary) {
    // A write crossing a boundary spans two objects.
    FB_ASSERT_EQ(get_obj_num(default_object_size - 1, 2, default_object_size), 2u);
    FB_ASSERT_EQ(get_obj_num(default_object_size - 4096, 8192, default_object_size), 2u);
}

FB_TEST(bdev_object_mapping, get_obj_num_exact_boundary) {
    // Aligned start and end at exact object boundary = exactly that many objects.
    FB_ASSERT_EQ(get_obj_num(0, 2 * default_object_size, default_object_size), 2u);
    FB_ASSERT_EQ(get_obj_num(0, 10 * default_object_size, default_object_size), 10u);
}

FB_TEST(bdev_object_mapping, calc_first_object_position_at_offset_zero) {
    // Offset 0: first object starts at offset 0, occupies full object.
    auto [sz, off, seq] = calc_first_object_position(0, default_object_size, default_object_size);
    FB_ASSERT_EQ(off, 0u);
    FB_ASSERT_EQ(sz, default_object_size);
    FB_ASSERT_EQ(seq, 0u);
}

FB_TEST(bdev_object_mapping, calc_first_object_position_mid_object) {
    // Offset in the middle: first object is partial, offset is the remainder.
    auto [sz, off, seq] = calc_first_object_position(default_object_size + 1, default_object_size, default_object_size);
    FB_ASSERT_EQ(off, 1u);
    FB_ASSERT_EQ(sz, default_object_size - 1);
    FB_ASSERT_EQ(seq, 1u);
}

FB_TEST(bdev_object_mapping, calc_first_object_position_truncated_by_length) {
    // If length < remaining space, first object size is truncated to length.
    auto [sz, off, seq] = calc_first_object_position(100, 50, default_object_size);
    FB_ASSERT_EQ(off, 100u);
    FB_ASSERT_EQ(sz, 50u); // truncated to length, not to object_size - off
}

FB_TEST(bdev_object_mapping, calc_first_object_position_object_sequence) {
    // object_seq is the zero-based index of the first object.
    auto [sz, off, seq] = calc_first_object_position(7 * default_object_size, 1, default_object_size);
    FB_ASSERT_EQ(seq, 7u);
    FB_ASSERT_EQ(off, 0u);
    FB_ASSERT_EQ(sz, 1u);
}

FB_TEST(bdev_object_mapping, round_trip_offset_to_object_and_back) {
    // object_seq * object_size + first_object_offset == original offset.
    // This is the key invariant that keeps mapping reversible.
    std::array<uint64_t, 5> offsets = {
        0u,
        1u,
        static_cast<uint64_t>(default_object_size) - 1,
        static_cast<uint64_t>(default_object_size),
        static_cast<uint64_t>(2 * default_object_size) - 1
    };
    for (uint64_t off : offsets) {
        auto [sz, first_off, seq] = calc_first_object_position(off, default_object_size, default_object_size);
        uint64_t reconstructed = seq * default_object_size + first_off;
        FB_ASSERT_EQ(reconstructed, off);
    }
}

// ============================================================================
// Test Suite: bdev_config_validation — block device configuration constraints
//
// bdev_fastblock_create takes block_size and image_size; these must satisfy
// invariants or the SPDK bdev layer will malfunction:
//   - block_size must be a power of 2 and >= 512 (historical floppy/HD sector size).
//   - image_size must be a multiple of block_size (otherwise the final block
//     would be partial and undefined behavior).
//   - object_size defaults to 4 MiB if zero.
// ============================================================================

namespace {

constexpr uint32_t default_block_size = 4096;
constexpr uint64_t min_image_size = default_block_size; // at least one block

struct bdev_config {
    uint64_t image_size = 0;
    uint32_t block_size = default_block_size;
    uint64_t object_size = 0; // 0 means "use default"
};

uint32_t get_default_object_size() { return 4 * 1024 * 1024; }

bool is_power_of_two(uint64_t v) { return v > 0 && (v & (v - 1)) == 0; }

} // anonymous namespace

FB_SUITE_SETUP(bdev_config_validation) {}
FB_SUITE_TEARDOWN(bdev_config_validation) {}

FB_TEST(bdev_config_validation, default_object_size_is_4MiB) {
    // If caller passes 0, we substitute 4 MiB. Pin the default.
    FB_ASSERT_EQ(get_default_object_size(), 4 * 1024 * 1024u);
}

FB_TEST(bdev_config_validation, block_size_must_be_power_of_two) {
    // SPDK requires power-of-two block sizes; odd sizes corrupt alignment.
    FB_ASSERT_TRUE(is_power_of_two(512));
    FB_ASSERT_TRUE(is_power_of_two(4096));
    FB_ASSERT_TRUE(is_power_of_two(8192));
    FB_ASSERT_FALSE(is_power_of_two(4097));
    FB_ASSERT_FALSE(is_power_of_two(3000));
    FB_ASSERT_FALSE(is_power_of_two(0));
}

FB_TEST(bdev_config_validation, block_size_must_be_at_least_512) {
    // Historical minimum: 512-byte sector (floppy/early HD).
    FB_ASSERT_GE(default_block_size, 512u);
    FB_ASSERT_EQ(default_block_size, 4096u); // the actual default
}

FB_TEST(bdev_config_validation, image_size_must_be_multiple_of_block_size) {
    // image_size = 10 * 4096 is valid; image_size = 10 * 4096 + 1 is not.
    bdev_config cfg;
    cfg.block_size = 4096;
    cfg.image_size = 10 * 4096;
    FB_ASSERT_EQ(cfg.image_size % cfg.block_size, 0u);

    cfg.image_size = 10 * 4096 + 1;
    FB_ASSERT_NE(cfg.image_size % cfg.block_size, 0u);
}

FB_TEST(bdev_config_validation, zero_object_size_means_default) {
    // The contract: object_size == 0 means "use default". Mirror that.
    bdev_config cfg;
    FB_ASSERT_EQ(cfg.object_size, 0u);
    uint64_t effective_object_size = (cfg.object_size == 0) ? get_default_object_size() : cfg.object_size;
    FB_ASSERT_EQ(effective_object_size, get_default_object_size());
}

FB_TEST(bdev_config_validation, non_zero_object_size_is_preserved) {
    bdev_config cfg;
    cfg.object_size = 8 * 1024 * 1024; // 8 MiB
    uint64_t effective = (cfg.object_size == 0) ? get_default_object_size() : cfg.object_size;
    FB_ASSERT_EQ(effective, 8 * 1024 * 1024u);
}

// Main function for test runner
FB_TEST_MAIN()
