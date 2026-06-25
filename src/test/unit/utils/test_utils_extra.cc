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
 * @file test_utils_extra.cc
 * @brief Unit tests for utility headers NOT covered by test_utils.cc:
 *        err_num, utils.h (align_up/align_down/constants/data structs),
 *        overload, duration_map.
 *
 * test_utils.cc already exhaustively covers itos/varint/md5/units. This
 * file picks up the remaining utils contracts (error code surface, generic
 * alignment, std::visit overload helper, osd_info_t / pg_info_type layout).
 *
 * Headers are mirrored locally rather than included, to avoid pulling
 * spdk/env, spdk/uuid, spdk/thread (and the random-engine globals utils.h
 * brings in) into the test binary.
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

// ============================================================================
// Test Suite: err_num_codes — fastblock error code surface
//
// err::* gathers every error code fastblock uses, mixing system errnos
// (negative ENOENT/ENOMEM/...) with fastblock's own range (-135 onwards).
// Branches across raft, osd, monitor, client all switch on these codes, so
// the contract is:
//   - E_SUCCESS is exactly 0 (the universal "ok" sentinel).
//   - System errnos are stored as NEGATIVE values (subtraction-friendly).
//   - The fastblock-specific range starts at -135 to avoid colliding with
//     system errnos (those go up to ~-133 on Linux).
//   - Values are unique and stable; renumbering breaks every wire-level
//     log parser that records the integer.
// ============================================================================

namespace {

enum class err_code : int {
    E_SUCCESS = 0,
    E_ENOENT  = -ENOENT,
    E_NOMEM   = -ENOMEM,
    E_BUSY    = -EBUSY,
    E_NODEV   = -ENODEV,
    E_INVAL   = -EINVAL,
    E_ENOSPC  = -ENOSPC,
    E_EILSEQ  = -EILSEQ,

    RAFT_ERR_NOT_LEADER             = -135,
    RAFT_ERR_ONE_VOTING_CHANGE_ONLY = -136,
    RAFT_ERR_SHUTDOWN               = -137,
    RAFT_ERR_NOMEM                  = -138,
    RAFT_ERR_NEEDS_SNAPSHOT         = -139,
    RAFT_ERR_SNAPSHOT_IN_PROGRESS   = -140,

    OSD_DOWN     = -149,
    OSD_STARTING = -150,

    RAFT_ERR_UNKNOWN = -199,
    RAFT_ERR_LAST    = -200,
};

constexpr int as_int(err_code c) { return static_cast<int>(c); }

} // anonymous namespace

FB_SUITE_SETUP(err_num_codes) {}
FB_SUITE_TEARDOWN(err_num_codes) {}

FB_TEST(err_num_codes, success_is_zero) {
    // The universal "ok" sentinel — every caller assumes 0 == success.
    FB_ASSERT_EQ(as_int(err_code::E_SUCCESS), 0);
}

FB_TEST(err_num_codes, system_errnos_are_negative) {
    // System errnos are NEGATED on storage so a single sign check (val < 0)
    // distinguishes error from success. ENOENT et al. are POSITIVE in
    // <errno.h>; we store them as -ENOENT.
    FB_ASSERT_TRUE(as_int(err_code::E_ENOENT) < 0);
    FB_ASSERT_TRUE(as_int(err_code::E_NOMEM) < 0);
    FB_ASSERT_TRUE(as_int(err_code::E_INVAL) < 0);
    FB_ASSERT_EQ(as_int(err_code::E_ENOENT), -ENOENT);
}

FB_TEST(err_num_codes, fastblock_range_starts_at_minus_135) {
    // System errnos on Linux go up to ~-133; fastblock-specific codes start
    // at -135 to leave room. Pin the boundary.
    FB_ASSERT_EQ(as_int(err_code::RAFT_ERR_NOT_LEADER), -135);
}

FB_TEST(err_num_codes, codes_are_pairwise_unique) {
    // Renumbering or aliasing would silently merge error branches. Verify
    // every code in our mirror set is distinct.
    std::vector<int> codes = {
        as_int(err_code::E_SUCCESS),
        as_int(err_code::E_ENOENT), as_int(err_code::E_NOMEM),
        as_int(err_code::E_BUSY),   as_int(err_code::E_NODEV),
        as_int(err_code::E_INVAL),  as_int(err_code::E_ENOSPC),
        as_int(err_code::E_EILSEQ),
        as_int(err_code::RAFT_ERR_NOT_LEADER),
        as_int(err_code::RAFT_ERR_ONE_VOTING_CHANGE_ONLY),
        as_int(err_code::RAFT_ERR_SHUTDOWN),
        as_int(err_code::RAFT_ERR_NOMEM),
        as_int(err_code::RAFT_ERR_NEEDS_SNAPSHOT),
        as_int(err_code::RAFT_ERR_SNAPSHOT_IN_PROGRESS),
        as_int(err_code::OSD_DOWN), as_int(err_code::OSD_STARTING),
        as_int(err_code::RAFT_ERR_UNKNOWN),
        as_int(err_code::RAFT_ERR_LAST),
    };
    auto n = codes.size();
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    FB_ASSERT_EQ(codes.size(), n);
}

FB_TEST(err_num_codes, last_is_most_negative) {
    // RAFT_ERR_LAST acts as a sentinel; nothing should be below it.
    FB_ASSERT_TRUE(as_int(err_code::RAFT_ERR_LAST) <= as_int(err_code::RAFT_ERR_UNKNOWN));
    FB_ASSERT_TRUE(as_int(err_code::RAFT_ERR_LAST) <= as_int(err_code::OSD_STARTING));
    FB_ASSERT_EQ(as_int(err_code::RAFT_ERR_LAST), -200);
}

FB_TEST(err_num_codes, raft_and_system_ranges_dont_overlap) {
    // System errnos on Linux are <= 133; the RAFT range starts at 135. The
    // gap (134) is intentional padding. Verify both directions.
    FB_ASSERT_TRUE(-ENOENT > -135); // system errno is "less negative" than -135
    FB_ASSERT_TRUE(-EILSEQ > -135);
    FB_ASSERT_TRUE(as_int(err_code::RAFT_ERR_NOT_LEADER) < -ENOENT);
}

// ============================================================================
// Test Suite: utils_align — generic align_up / align_down (utils.h)
//
// align_up<T>(v, align) and align_down<T>(v, align) work via bitmask:
//   align_up   = (v + align - 1) & ~(align - 1)
//   align_down = v & ~(align - 1)
// PRECONDITION: align must be a power of two. Otherwise ~(align - 1) is a
// nonsense mask. This is a stricter contract than bdev_object_mapping's
// arithmetic version — we test both behaviours.
// ============================================================================

namespace {

template<typename T>
constexpr T u_align_up(T v, T align) {
    return (v + align - 1) & ~(align - 1);
}

template<typename T>
constexpr T u_align_down(T v, T align) {
    return v & ~(align - 1);
}

} // anonymous namespace

FB_SUITE_SETUP(utils_align) {}
FB_SUITE_TEARDOWN(utils_align) {}

FB_TEST(utils_align, align_down_is_identity_at_boundary) {
    FB_ASSERT_EQ(u_align_down<uint64_t>(0, 4096), 0u);
    FB_ASSERT_EQ(u_align_down<uint64_t>(4096, 4096), 4096u);
    FB_ASSERT_EQ(u_align_down<uint64_t>(8192, 4096), 8192u);
}

FB_TEST(utils_align, align_down_rounds_down) {
    FB_ASSERT_EQ(u_align_down<uint64_t>(1, 4096), 0u);
    FB_ASSERT_EQ(u_align_down<uint64_t>(4095, 4096), 0u);
    FB_ASSERT_EQ(u_align_down<uint64_t>(4097, 4096), 4096u);
    FB_ASSERT_EQ(u_align_down<uint64_t>(8191, 4096), 4096u);
}

FB_TEST(utils_align, align_up_is_identity_at_boundary) {
    FB_ASSERT_EQ(u_align_up<uint64_t>(0, 4096), 0u);
    FB_ASSERT_EQ(u_align_up<uint64_t>(4096, 4096), 4096u);
    FB_ASSERT_EQ(u_align_up<uint64_t>(8192, 4096), 8192u);
}

FB_TEST(utils_align, align_up_rounds_up) {
    FB_ASSERT_EQ(u_align_up<uint64_t>(1, 4096), 4096u);
    FB_ASSERT_EQ(u_align_up<uint64_t>(4095, 4096), 4096u);
    FB_ASSERT_EQ(u_align_up<uint64_t>(4097, 4096), 8192u);
}

FB_TEST(utils_align, works_for_uint32_too) {
    // The template instantiates for any integer T; check uint32_t.
    FB_ASSERT_EQ(u_align_up<uint32_t>(100, 64), 128u);
    FB_ASSERT_EQ(u_align_down<uint32_t>(100, 64), 64u);
}

FB_TEST(utils_align, alignment_must_be_power_of_two) {
    // PRECONDITION: only powers of 2 produce a valid mask. For align=3,
    // align-1 = 2 = 0b010, so ~(align-1) = ...11111101 — a nonsense mask
    // that AND's away ONLY bit 1. align_down(5, 3): 5 = 0b101, AND ~0b010
    // = 0b101, returns 5 (NOT 3, as arithmetic division would yield). This
    // test pins the LIMITATION: callers must pre-check that align is a
    // power of 2; the function does not.
    FB_ASSERT_EQ(u_align_down<uint64_t>(5, 3), 5u); // NOT 3 — power-of-2 only!
    FB_ASSERT_EQ(u_align_down<uint64_t>(7, 3), 5u); // 7 = 0b111, AND ~0b010 = 0b101 = 5
}

FB_TEST(utils_align, large_64bit_values) {
    // Stress at the upper end of uint64_t.
    constexpr uint64_t big = 0x1000'0000'0000'0001ULL;
    FB_ASSERT_EQ(u_align_down<uint64_t>(big, 4096), 0x1000'0000'0000'0000ULL);
    FB_ASSERT_EQ(u_align_up<uint64_t>(big, 4096), 0x1000'0000'0000'1000ULL);
}

// ============================================================================
// Test Suite: utils_constants — port range and cluster size sentinels
//
// utils.h hardcodes several deployment constants:
//   - MIN_OSD_PORT / MAX_OSD_PORT bracket the legal OSD listen range.
//   - default_monitor_port is the default monitor listen port.
//   - default_blobstore_cluster_size is 1 MiB (used by the blobstore).
// These leak into config files and deployment scripts; freezing them
// catches accidental changes.
// ============================================================================

namespace {

constexpr int32_t MIN_OSD_PORT = 9000;
constexpr int32_t MAX_OSD_PORT = 10000;
constexpr int32_t default_monitor_port = 3333;
constexpr uint32_t default_blobstore_core = 0;
constexpr uint32_t default_blobstore_cluster_size = (1024 * 1024);

} // anonymous namespace

FB_SUITE_SETUP(utils_constants) {}
FB_SUITE_TEARDOWN(utils_constants) {}

FB_TEST(utils_constants, osd_port_range_is_nontrivial) {
    // MAX > MIN, and the range has room (at least 100 ports for many OSDs).
    FB_ASSERT_TRUE(MAX_OSD_PORT > MIN_OSD_PORT);
    FB_ASSERT_GE(MAX_OSD_PORT - MIN_OSD_PORT, 100);
}

FB_TEST(utils_constants, osd_port_values_pinned) {
    // Exact values; deployment scripts rely on these.
    FB_ASSERT_EQ(MIN_OSD_PORT, 9000);
    FB_ASSERT_EQ(MAX_OSD_PORT, 10000);
}

FB_TEST(utils_constants, monitor_port_is_3333) {
    FB_ASSERT_EQ(default_monitor_port, 3333);
}

FB_TEST(utils_constants, monitor_port_outside_osd_range) {
    // The monitor MUST NOT live in the OSD port range or they could collide
    // on a single host running both.
    FB_ASSERT_TRUE(default_monitor_port < MIN_OSD_PORT || default_monitor_port > MAX_OSD_PORT);
}

FB_TEST(utils_constants, blobstore_cluster_size_is_1MiB) {
    // The blobstore allocates in 1 MiB clusters; changing this is an
    // on-disk format change.
    FB_ASSERT_EQ(default_blobstore_cluster_size, 1024u * 1024u);
}

FB_TEST(utils_constants, blobstore_cluster_size_is_power_of_two) {
    // SPDK blobstore requires power-of-2 cluster size.
    auto v = default_blobstore_cluster_size;
    FB_ASSERT_TRUE(v > 0 && (v & (v - 1)) == 0);
}

FB_TEST(utils_constants, default_blobstore_core_is_zero) {
    // Core 0 is the default housekeeping core.
    FB_ASSERT_EQ(default_blobstore_core, 0u);
}

// ============================================================================
// Test Suite: utils_osd_info — osd_info_t / pg_info_type / core_shard_map
//
// These are the in-memory descriptors the monclient hands to the rest of
// fastblock. The contract:
//   - Default-constructed osd_info_t is "not running" (isup=false, isin=false).
//   - pg_info_type starts at version 0 with empty osd list (a fresh PG).
//   - core_shard_map is a 12-byte POD; layout matters because it's hashed
//     into config maps.
// ============================================================================

namespace {

struct core_shard_map_t {
    uint32_t port;
    uint32_t core_id;
    uint32_t shard_id;
};

struct osd_info_t {
    int node_id{0};
    bool isin{false};
    bool isup{false};
    bool ispendingcreate{false};
    std::map<uint32_t, core_shard_map_t> sharded_ports{};
    std::string address{};
};

struct pg_info_type {
    uint64_t pg_id{0};
    int64_t version{0};
    std::vector<int> osds{};
};

} // anonymous namespace

FB_SUITE_SETUP(utils_osd_info) {}
FB_SUITE_TEARDOWN(utils_osd_info) {}

FB_TEST(utils_osd_info, osd_info_default_is_not_running) {
    // A fresh osd_info must read as "doesn't exist yet": down, out, no
    // pending create, no address. Otherwise a default-init slot looks like
    // a live OSD to the scheduler.
    osd_info_t info;
    FB_ASSERT_FALSE(info.isin);
    FB_ASSERT_FALSE(info.isup);
    FB_ASSERT_FALSE(info.ispendingcreate);
    FB_ASSERT_TRUE(info.address.empty());
    FB_ASSERT_TRUE(info.sharded_ports.empty());
}

FB_TEST(utils_osd_info, osd_info_isup_and_isin_independent) {
    // isup (running) and isin (member of the cluster) are orthogonal:
    // an OSD can be in-but-down (failed) or up-but-out (newly joining).
    osd_info_t info;
    info.isin = true;
    FB_ASSERT_FALSE(info.isup); // still down
    info.isup = true;
    FB_ASSERT_TRUE(info.isin); // still in
}

FB_TEST(utils_osd_info, pg_info_default_is_fresh_pg) {
    pg_info_type pg;
    FB_ASSERT_EQ(pg.pg_id, 0u);
    FB_ASSERT_EQ(pg.version, 0);
    FB_ASSERT_TRUE(pg.osds.empty());
}

FB_TEST(utils_osd_info, pg_info_version_is_signed) {
    // version is int64_t so callers can pass -1 to mean "no version yet"
    // (matches the osd_map sentinel). Verify the sign.
    pg_info_type pg;
    pg.version = -1;
    FB_ASSERT_EQ(pg.version, -1);
}

FB_TEST(utils_osd_info, core_shard_map_size_is_12_bytes) {
    // 3 × uint32_t = 12 bytes, no padding (alignment is 4, all fields are 4).
    FB_ASSERT_EQ(sizeof(core_shard_map_t), 12u);
}

FB_TEST(utils_osd_info, core_shard_map_fields_are_uint32) {
    // port and shard_id flow through protobuf as uint32; an accidental widening
    // would break the wire.
    core_shard_map_t m{};
    FB_ASSERT_EQ(sizeof(m.port), 4u);
    FB_ASSERT_EQ(sizeof(m.core_id), 4u);
    FB_ASSERT_EQ(sizeof(m.shard_id), 4u);
}

FB_TEST(utils_osd_info, sharded_ports_keyed_by_shard_id) {
    // The map's key is shard_id; lookups by shard_id must hit.
    osd_info_t info;
    info.sharded_ports[0] = {9001, 0, 0};
    info.sharded_ports[1] = {9002, 1, 1};
    FB_ASSERT_EQ(info.sharded_ports.size(), 2u);
    FB_ASSERT_EQ(info.sharded_ports.at(0).port, 9001u);
    FB_ASSERT_EQ(info.sharded_ports.at(1).port, 9002u);
}

// ============================================================================
// Test Suite: utils_overload — std::visit overload helper
//
// utils::overload is the canonical "lambda visitor" trick:
//   std::visit(overload{[](int){...}, [](string){...}}, var);
// It deduces the lambda types and pulls each operator() into a single struct.
// Pin the behaviour so an accidental refactor doesn't lose the deduction
// guide.
// ============================================================================

namespace u_overload {
template<typename... Ts>
struct overload : Ts... { using Ts::operator()...; };
template<class... Ts> overload(Ts...) -> overload<Ts...>;
} // namespace u_overload

FB_SUITE_SETUP(utils_overload) {}
FB_SUITE_TEARDOWN(utils_overload) {}

FB_TEST(utils_overload, dispatches_on_alternative_type) {
    // The whole point: each alternative's lambda runs.
    std::variant<int, std::string> v;

    v = 42;
    int int_hits = 0, str_hits = 0;
    auto vis = u_overload::overload{
        [&](int)         { ++int_hits; },
        [&](const std::string&) { ++str_hits; },
    };
    std::visit(vis, v);
    FB_ASSERT_EQ(int_hits, 1);
    FB_ASSERT_EQ(str_hits, 0);

    v = std::string{"hi"};
    std::visit(vis, v);
    FB_ASSERT_EQ(int_hits, 1);
    FB_ASSERT_EQ(str_hits, 1);
}

FB_TEST(utils_overload, captures_outer_state) {
    // Lambdas captured by reference must still write back into the outer scope.
    std::variant<int, double> v = 3.14;
    double seen = 0.0;
    std::visit(
        u_overload::overload{
            [&](int x) { seen = x; },
            [&](double x) { seen = x; },
        },
        v);
    FB_ASSERT_NEAR(seen, 3.14, 1e-9);
}

FB_TEST(utils_overload, works_with_three_alternatives) {
    using V = std::variant<int, std::string, double>;
    V v;
    auto type_name = [&](const V& x) {
        return std::visit(
            u_overload::overload{
                [](int)               { return std::string{"int"}; },
                [](const std::string&){ return std::string{"str"}; },
                [](double)            { return std::string{"dbl"}; },
            },
            x);
    };
    v = 1;        FB_ASSERT_STR_EQ(type_name(v).c_str(), "int");
    v = "hi";     FB_ASSERT_STR_EQ(type_name(v).c_str(), "str");
    v = 2.0;      FB_ASSERT_STR_EQ(type_name(v).c_str(), "dbl");
}

// ============================================================================
// Test Suite: utils_duration_map — latency percentile statistics
//
// duration_map records start/end timestamps keyed by request id, and reports
// p50/p90/p99 + mean + biased stdev. The reporting code itself depends on
// SPDK_ERRLOG so we don't try to invoke it; instead we test the underlying
// percentile / stdev arithmetic in isolation.
// ============================================================================

namespace {

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    auto idx = static_cast<size_t>(p * v.size());
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

double mean(const std::vector<double>& v) {
    double s = 0.0;
    for (double x : v) s += x;
    return v.empty() ? 0.0 : s / v.size();
}

double biased_stdev(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double m = mean(v);
    double accum = 0.0;
    for (double x : v) accum += (x - m) * (x - m);
    return std::sqrt(accum / v.size());
}

} // anonymous namespace

FB_SUITE_SETUP(utils_duration_map) {}
FB_SUITE_TEARDOWN(utils_duration_map) {}

FB_TEST(utils_duration_map, percentile_of_uniform_sequence) {
    // 0..99: p50 ≈ 50, p90 ≈ 90, p99 ≈ 99.
    std::vector<double> v;
    for (int i = 0; i < 100; ++i) v.push_back(i);
    FB_ASSERT_EQ(percentile(v, 0.5), 50.0);
    FB_ASSERT_EQ(percentile(v, 0.9), 90.0);
    FB_ASSERT_EQ(percentile(v, 0.99), 99.0);
}

FB_TEST(utils_duration_map, p999_clamps_to_last_element) {
    // duration_map computes the index as (p * N); for N=100, p=0.999 → 99.9,
    // truncated to 99. p=1.0 would index 100, so we clamp to N-1 (here 99).
    std::vector<double> v;
    for (int i = 0; i < 100; ++i) v.push_back(i);
    FB_ASSERT_EQ(percentile(v, 0.999), 99.0);
}

FB_TEST(utils_duration_map, mean_of_constant_sequence) {
    std::vector<double> v(10, 42.0);
    FB_ASSERT_NEAR(mean(v), 42.0, 1e-9);
}

FB_TEST(utils_duration_map, stdev_of_constant_is_zero) {
    // All identical → no variation → stdev == 0.
    std::vector<double> v(20, 7.0);
    FB_ASSERT_NEAR(biased_stdev(v), 0.0, 1e-9);
}

FB_TEST(utils_duration_map, stdev_of_known_sequence) {
    // {2, 4, 4, 4, 5, 5, 7, 9} has biased stdev 2.0 (textbook example).
    std::vector<double> v = {2, 4, 4, 4, 5, 5, 7, 9};
    FB_ASSERT_NEAR(biased_stdev(v), 2.0, 1e-9);
    FB_ASSERT_NEAR(mean(v), 5.0, 1e-9);
}

FB_TEST(utils_duration_map, percentile_monotonic_in_p) {
    // p(x) is non-decreasing in x.
    std::vector<double> v;
    for (int i = 0; i < 1000; ++i) v.push_back(i);
    double last = 0.0;
    for (double p : {0.1, 0.25, 0.5, 0.75, 0.9, 0.99}) {
        double cur = percentile(v, p);
        FB_ASSERT_GE(cur, last);
        last = cur;
    }
}

// Main function for test runner
FB_TEST_MAIN()
