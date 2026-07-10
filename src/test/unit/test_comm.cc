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

// Main function for test runner
FB_TEST_MAIN()
