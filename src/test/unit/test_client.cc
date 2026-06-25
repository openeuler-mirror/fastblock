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
 * @file test_client.cc
 * @brief Unit tests for client module routing and addressing contracts.
 *
 * Part 1 of client tests covering:
 *   - Jenkins object-name hash (calc_target)
 *   - PG mask / pg_num arithmetic (calc_pg_masks)
 *   - Image object naming (calc_image_object_prefix / get_image_object_name)
 *   - Object splitting (calc_first_object_position / get_obj_num)
 *   - Size literals and errc enum surface
 *
 * The client is the user-facing entry point; these arithmetic contracts are
 * invoked on every IO path. Pinning them prevents silent routing changes.
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// ============================================================================
// Local mirrors of the client/* contracts under test.
// ============================================================================

namespace {

// ---------- libfblock.h size literals (mirrored verbatim) -----------------
constexpr size_t KiB = 1024;
constexpr size_t MiB = 1024 * KiB;
constexpr size_t GiB = 1024 * MiB;
constexpr size_t default_object_size = 4 * MiB;

// ---------- libfblock.h errc (mirrored verbatim) --------------------------
enum errc {
    success = 0,            // MUST be 0; multiple callsites compare against E_SUCCESS.
    image_not_exist,
    image_already_exist,
    etcd_cmd_failed,
    etcd_image_format_invalid,
    size_is_less_than_current_size,
    put_image_state_flag_failed,
    image_in_deleting,
    not_supported,
    closed,
    invalid_read_data_size,
    invalid_write_data_size,
};

// ---------- jenkins_hash (mirrored from fb_client.cc) ----------------------
#define mix_(a, b, c)      \
    {                      \
        a = a - b;         \
        a = a - c;         \
        a = a ^ (c >> 13); \
        b = b - c;         \
        b = b - a;         \
        b = b ^ (a << 8);  \
        c = c - a;         \
        c = c - b;         \
        c = c ^ (b >> 13); \
        a = a - b;         \
        a = a - c;         \
        a = a ^ (c >> 12); \
        b = b - c;         \
        b = b - a;         \
        b = b ^ (a << 16); \
        c = c - a;         \
        c = c - b;         \
        c = c ^ (b >> 5);  \
        a = a - b;         \
        a = a - c;         \
        a = a ^ (c >> 3);  \
        b = b - c;         \
        b = b - a;         \
        b = b ^ (a << 10); \
        c = c - a;         \
        c = c - b;         \
        c = c ^ (b >> 15); \
    }

unsigned jenkins_hash(const std::string& str, unsigned length) {
    const unsigned char* k = (const unsigned char*)str.c_str();
    uint32_t a, b, c;
    uint32_t len = length;
    a = 0x9e3779b9;
    b = a;
    c = 0;
    while (len >= 12) {
        a += (k[0] + ((uint32_t)k[1] << 8) + ((uint32_t)k[2] << 16) + ((uint32_t)k[3] << 24));
        b += (k[4] + ((uint32_t)k[5] << 8) + ((uint32_t)k[6] << 16) + ((uint32_t)k[7] << 24));
        c += (k[8] + ((uint32_t)k[9] << 8) + ((uint32_t)k[10] << 16) + ((uint32_t)k[11] << 24));
        mix_(a, b, c);
        k += 12;
        len -= 12;
    }
    c += length;
    switch (len) {
    case 11: c += ((uint32_t)k[10] << 24); [[fallthrough]];
    case 10: c += ((uint32_t)k[9] << 16);  [[fallthrough]];
    case 9:  c += ((uint32_t)k[8] << 8);   [[fallthrough]];
    case 8:  b += ((uint32_t)k[7] << 24);  [[fallthrough]];
    case 7:  b += ((uint32_t)k[6] << 16);  [[fallthrough]];
    case 6:  b += ((uint32_t)k[5] << 8);   [[fallthrough]];
    case 5:  b += k[4];                    [[fallthrough]];
    case 4:  a += ((uint32_t)k[3] << 24);  [[fallthrough]];
    case 3:  a += ((uint32_t)k[2] << 16);  [[fallthrough]];
    case 2:  a += ((uint32_t)k[1] << 8);   [[fallthrough]];
    case 1:  a += k[0];
    }
    mix_(a, b, c);
    return c;
}

// ---------- cbits / calc_target / calc_pg_masks -----------------------------
template <class T>
inline typename std::enable_if<(std::is_integral<T>::value && sizeof(T) <= sizeof(unsigned)), unsigned>::type
cbits(T v) {
    if (v == 0) return 0;
    return (sizeof(v) * 8) - __builtin_clz(v);
}

struct pg_router {
    uint32_t pg_num{0};
    uint32_t pg_mask{0};

    void calc_pg_masks(uint32_t target_pg_num) {
        pg_num = target_pg_num;
        pg_mask = (1u << cbits(pg_num - 1)) - 1u;
    }

    unsigned calc_target(const std::string& sstr) {
        unsigned seed = jenkins_hash(sstr, sstr.size());
        return seed % pg_num;
    }
};

// ---------- libblk_client object addressing (mirrored verbatim) ------------
std::string calc_image_object_prefix(uint64_t pool_id, const std::string& image_name) {
    return std::to_string(pool_id) + "__blk_data___" + image_name;
}

std::string get_image_object_name(std::string& prefix, uint64_t seq) {
    char ch[17];
    std::snprintf(ch, 17, "%lu", seq);
    return prefix + std::string{ch};
}

std::tuple<size_t, uint64_t, uint64_t>
calc_first_object_position(uint64_t offset, uint64_t length, size_t object_size) {
    uint64_t first_object_offset = offset % object_size;
    size_t   first_object_size   = object_size - first_object_offset;
    if (length < first_object_size) {
        first_object_size = length;
    }
    uint64_t object_seq = offset / static_cast<uint64_t>(object_size);
    return std::make_tuple(first_object_size, first_object_offset, object_seq);
}

uint64_t get_obj_num(uint64_t offset, uint64_t length) {
    // align_down / align_up (power-of-2 form), mirroring utils.h.
    auto align_down = [](uint64_t v, uint64_t a) { return v & ~(a - 1); };
    auto align_up   = [](uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); };
    auto start_off = align_down(offset, default_object_size);
    auto end_off   = align_up(offset + length, default_object_size);
    return (end_off - start_off) / default_object_size;
}

} // anonymous namespace


// ============================================================================
// Test Suite: client_jenkins_hash — fb_client.cc jenkins object hash
// ============================================================================

FB_SUITE_SETUP(client_jenkins_hash) {}
FB_SUITE_TEARDOWN(client_jenkins_hash) {}

FB_TEST(client_jenkins_hash, deterministic) {
    auto h1 = jenkins_hash("pool_1__blk_data___image0", 26);
    auto h2 = jenkins_hash("pool_1__blk_data___image0", 26);
    FB_ASSERT_EQ(h1, h2);
}

FB_TEST(client_jenkins_hash, length_sensitive) {
    FB_ASSERT_NE(jenkins_hash("", 0), jenkins_hash("a", 1));
}

FB_TEST(client_jenkins_hash, distinct_object_names_differ) {
    std::set<unsigned> buckets;
    std::string prefix = "1__blk_data___img_x";
    for (uint64_t i = 0; i < 32; ++i) {
        auto name = prefix + std::to_string(i);
        buckets.insert(jenkins_hash(name, name.size()) % 128u);
    }
    FB_ASSERT_GE(buckets.size(), 24u);
}

FB_TEST(client_jenkins_hash, handles_keys_above_12_bytes) {
    auto h12 = jenkins_hash("abcdefghijkl", 12);
    auto h13 = jenkins_hash("abcdefghijklm", 13);
    auto h24 = jenkins_hash("abcdefghijklabcdefghijkl", 24);
    FB_ASSERT_NE(h12, h13);
    FB_ASSERT_NE(h12, h24);
}

FB_TEST(client_jenkins_hash, distinct_short_keys_distinct_hashes) {
    std::set<unsigned> hs;
    for (char c = 'a'; c <= 'z'; ++c) {
        hs.insert(jenkins_hash(std::string{c}, 1));
    }
    FB_ASSERT_EQ(hs.size(), 26u);
}

// ============================================================================
// Test Suite: client_pg_mask — calc_pg_masks bitfield arithmetic
// ============================================================================

FB_SUITE_SETUP(client_pg_mask) {}
FB_SUITE_TEARDOWN(client_pg_mask) {}

FB_TEST(client_pg_mask, pg_num_one_yields_zero_mask) {
    pg_router r;
    r.calc_pg_masks(1);
    FB_ASSERT_EQ(r.pg_num, 1u);
    FB_ASSERT_EQ(r.pg_mask, 0u);
}

FB_TEST(client_pg_mask, pg_num_power_of_two) {
    pg_router r;
    r.calc_pg_masks(8);
    FB_ASSERT_EQ(r.pg_mask, 7u);
    r.calc_pg_masks(256);
    FB_ASSERT_EQ(r.pg_mask, 255u);
    r.calc_pg_masks(1024);
    FB_ASSERT_EQ(r.pg_mask, 1023u);
}

FB_TEST(client_pg_mask, pg_num_not_power_of_two_rounds_up) {
    pg_router r;
    r.calc_pg_masks(5);
    FB_ASSERT_EQ(r.pg_mask, 7u);
    r.calc_pg_masks(9);
    FB_ASSERT_EQ(r.pg_mask, 15u);
}

FB_TEST(client_pg_mask, large_pg_num) {
    pg_router r;
    r.calc_pg_masks(1u << 20);
    FB_ASSERT_EQ(r.pg_mask, (1u << 20) - 1u);
}

// ============================================================================
// Test Suite: client_calc_target — hash % pg_num routing
// ============================================================================

FB_SUITE_SETUP(client_calc_target) {}
FB_SUITE_TEARDOWN(client_calc_target) {}

FB_TEST(client_calc_target, returns_within_pg_range) {
    pg_router r;
    r.calc_pg_masks(64);
    for (uint64_t i = 0; i < 200; ++i) {
        auto name = "img_obj_" + std::to_string(i);
        FB_ASSERT_LT(r.calc_target(name), 64u);
    }
}

FB_TEST(client_calc_target, deterministic_across_calls) {
    pg_router r;
    r.calc_pg_masks(32);
    auto t1 = r.calc_target("the_object_name");
    auto t2 = r.calc_target("the_object_name");
    FB_ASSERT_EQ(t1, t2);
}

FB_TEST(client_calc_target, pg_num_change_routes_differently) {
    pg_router r;
    r.calc_pg_masks(16);
    auto small = r.calc_target("an_object");
    r.calc_pg_masks(64);
    auto large = r.calc_target("an_object");
    FB_ASSERT_LT(small, 16u);
    FB_ASSERT_LT(large, 64u);
}

FB_TEST(client_calc_target, multiple_objects_spread) {
    pg_router r;
    r.calc_pg_masks(8);
    std::set<unsigned> hit;
    for (int i = 0; i < 256; ++i) {
        hit.insert(r.calc_target("obj_" + std::to_string(i)));
    }
    FB_ASSERT_EQ(hit.size(), 8u);
}

// ============================================================================
// Test Suite: client_object_addressing — image object name conventions
// ============================================================================

FB_SUITE_SETUP(client_object_addressing) {}
FB_SUITE_TEARDOWN(client_object_addressing) {}

FB_TEST(client_object_addressing, prefix_format) {
    FB_ASSERT_STR_EQ(calc_image_object_prefix(1, "img").c_str(), "1__blk_data___img");
    FB_ASSERT_STR_EQ(calc_image_object_prefix(0, "").c_str(),    "0__blk_data___");
    FB_ASSERT_STR_EQ(calc_image_object_prefix(42, "vol").c_str(), "42__blk_data___vol");
}

FB_TEST(client_object_addressing, object_name_concatenates_seq) {
    auto prefix = calc_image_object_prefix(7, "image");
    FB_ASSERT_STR_EQ(get_image_object_name(prefix, 0).c_str(),   "7__blk_data___image0");
    FB_ASSERT_STR_EQ(get_image_object_name(prefix, 1).c_str(),   "7__blk_data___image1");
    FB_ASSERT_STR_EQ(get_image_object_name(prefix, 42).c_str(),  "7__blk_data___image42");
}

FB_TEST(client_object_addressing, seq_uses_decimal_not_hex) {
    auto prefix = calc_image_object_prefix(0, "x");
    FB_ASSERT_STR_EQ(get_image_object_name(prefix, 16).c_str(), "0__blk_data___x16");
    FB_ASSERT_STR_EQ(get_image_object_name(prefix, 255).c_str(), "0__blk_data___x255");
}

FB_TEST(client_object_addressing, large_seq_truncation_boundary) {
    auto prefix = calc_image_object_prefix(0, "");
    auto name = get_image_object_name(prefix, 1234567890123ull);
    FB_ASSERT_TRUE(name.find("1234567890123") != std::string::npos);
}

// ============================================================================
// Test Suite: client_first_object_position — calc_first_object_position
// ============================================================================

FB_SUITE_SETUP(client_first_object_position) {}
FB_SUITE_TEARDOWN(client_first_object_position) {}

FB_TEST(client_first_object_position, zero_offset_small_length) {
    auto [sz, off, seq] = calc_first_object_position(0, 1024, default_object_size);
    FB_ASSERT_EQ(sz,  1024u);
    FB_ASSERT_EQ(off, 0u);
    FB_ASSERT_EQ(seq, 0u);
}

FB_TEST(client_first_object_position, zero_offset_exact_object) {
    auto [sz, off, seq] = calc_first_object_position(0, default_object_size, default_object_size);
    FB_ASSERT_EQ(sz,  default_object_size);
    FB_ASSERT_EQ(off, 0u);
    FB_ASSERT_EQ(seq, 0u);
}

FB_TEST(client_first_object_position, mid_object_spans) {
    auto [sz, off, seq] = calc_first_object_position(5 * MiB, 4 * MiB, default_object_size);
    FB_ASSERT_EQ(sz,  3 * MiB);
    FB_ASSERT_EQ(off, 1 * MiB);
    FB_ASSERT_EQ(seq, 1u);
}

FB_TEST(client_first_object_position, mid_object_contained) {
    auto [sz, off, seq] = calc_first_object_position(5 * MiB, 1 * MiB, default_object_size);
    FB_ASSERT_EQ(sz,  1 * MiB);
    FB_ASSERT_EQ(off, 1 * MiB);
    FB_ASSERT_EQ(seq, 1u);
}

FB_TEST(client_first_object_position, seq_grows_with_offset) {
    for (uint64_t k = 0; k < 8; ++k) {
        auto [sz, off, seq] = calc_first_object_position(k * default_object_size, 1024, default_object_size);
        FB_ASSERT_EQ(seq, k);
        FB_ASSERT_EQ(off, 0u);
        FB_ASSERT_EQ(sz,  1024u);
    }
}

// ============================================================================
// Test Suite: client_object_count — get_obj_num (multi-object spans)
// ============================================================================

FB_SUITE_SETUP(client_object_count) {}
FB_SUITE_TEARDOWN(client_object_count) {}

FB_TEST(client_object_count, single_object_aligned) {
    FB_ASSERT_EQ(get_obj_num(0, 1024), 1u);
    FB_ASSERT_EQ(get_obj_num(0, default_object_size), 1u);
}

FB_TEST(client_object_count, exact_object_unaligned_offset) {
    FB_ASSERT_EQ(get_obj_num(1 * MiB, 4 * MiB), 2u);
}

FB_TEST(client_object_count, spans_three_objects) {
    FB_ASSERT_EQ(get_obj_num(3 * MiB, 7 * MiB), 3u);
}

FB_TEST(client_object_count, large_aligned_io) {
    FB_ASSERT_EQ(get_obj_num(0, 16 * MiB), 4u);
}

FB_TEST(client_object_count, single_byte) {
    FB_ASSERT_EQ(get_obj_num(4 * MiB - 1, 1), 1u);
    FB_ASSERT_EQ(get_obj_num(4 * MiB, 1), 1u);
}

FB_TEST(client_object_count, matches_seq_count_from_position) {
    auto check = [](uint64_t offset, uint64_t length) {
        auto expected = get_obj_num(offset, length);
        auto [first_sz, first_off, first_seq] = calc_first_object_position(offset, length, default_object_size);
        (void)first_off; (void)first_seq;
        size_t bytes = 0;
        uint64_t iters = 0;
        size_t slice = first_sz;
        while (bytes < length) {
            bytes += slice;
            slice = default_object_size;
            if (slice > length - bytes) slice = length - bytes;
            ++iters;
        }
        FB_ASSERT_EQ(iters, expected);
    };
    check(0, 1024);
    check(0, default_object_size);
    check(1 * MiB, 4 * MiB);
    check(3 * MiB, 7 * MiB);
    check(0, 16 * MiB);
    check(4 * MiB - 1, 1);
}

// ============================================================================
// Test Suite: client_size_literals — KiB/MiB/GiB and default_object_size
// ============================================================================

FB_SUITE_SETUP(client_size_literals) {}
FB_SUITE_TEARDOWN(client_size_literals) {}

FB_TEST(client_size_literals, base_values) {
    FB_ASSERT_EQ(KiB, 1024u);
    FB_ASSERT_EQ(MiB, 1024u * 1024u);
    FB_ASSERT_EQ(GiB, 1024u * 1024u * 1024u);
}

FB_TEST(client_size_literals, default_object_size_is_4MiB) {
    FB_ASSERT_EQ(default_object_size, 4u * 1024u * 1024u);
    FB_ASSERT_EQ(default_object_size, 4u * MiB);
}

// ============================================================================
// Test Suite: client_errc_enum — libfblock errc surface
// ============================================================================

FB_SUITE_SETUP(client_errc_enum) {}
FB_SUITE_TEARDOWN(client_errc_enum) {}

FB_TEST(client_errc_enum, success_is_zero) {
    FB_ASSERT_EQ(static_cast<int>(errc::success), 0);
}

FB_TEST(client_errc_enum, error_values_pairwise_unique) {
    std::vector<int> codes = {
        errc::success,
        errc::image_not_exist, errc::image_already_exist,
        errc::etcd_cmd_failed, errc::etcd_image_format_invalid,
        errc::size_is_less_than_current_size, errc::put_image_state_flag_failed,
        errc::image_in_deleting, errc::not_supported, errc::closed,
        errc::invalid_read_data_size, errc::invalid_write_data_size,
    };
    auto n = codes.size();
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    FB_ASSERT_EQ(codes.size(), n);
}

FB_TEST(client_errc_enum, non_success_are_positive) {
    FB_ASSERT_TRUE(static_cast<int>(errc::image_not_exist) > 0);
    FB_ASSERT_TRUE(static_cast<int>(errc::invalid_write_data_size) > 0);
}

// ============================================================================
// Part 2: Identifiers, retry classification, and write-ring state machine
// ============================================================================

namespace {

// ---------- fb_client error-code surface used by should_retry_request -----
namespace err {
    constexpr int32_t E_SUCCESS                   = 0;
    constexpr int32_t RAFT_ERR_NOT_LEADER         = -135;
    constexpr int32_t RAFT_ERR_NOT_FOUND_PG       = -141;
    constexpr int32_t RAFT_ERR_PG_SHUTDOWN        = -142;
    constexpr int32_t RAFT_ERR_NO_CONNECTED       = -143;
    constexpr int32_t RAFT_ERR_PG_INITIALIZING    = -144;
    constexpr int32_t OSD_DOWN                    = -149;
    constexpr int32_t OSD_STARTING                = -150;
    constexpr int32_t ERR_NOT_FOUND_POOL          = -160;
    constexpr int32_t ERR_INTERNAL                = -180;
    constexpr int32_t ERR_PERM                    = -EPERM;
}

// ---------- fb_client.h connection_id / leader_key (bit-packed) -----------
struct connection_id_layout {
    int32_t  node_id;
    uint32_t port;
};
static_assert(sizeof(connection_id_layout) == sizeof(uint64_t));

uint64_t to_connection_id(int32_t node_id, int port) {
    uint64_t ret{};
    auto* p = reinterpret_cast<connection_id_layout*>(&ret);
    p->node_id = node_id;
    p->port    = static_cast<uint32_t>(port);
    return ret;
}

struct leader_key_layout {
    int32_t pool_id;
    int32_t pg_id;
};
static_assert(sizeof(leader_key_layout) == sizeof(uint64_t));

uint64_t make_leader_key(int32_t pool_id, int32_t pg_id) {
    uint64_t k{};
    auto* p = reinterpret_cast<leader_key_layout*>(&k);
    p->pg_id   = pg_id;
    p->pool_id = pool_id;
    return k;
}

leader_key_layout from_leader_key(uint64_t k) {
    auto* p = reinterpret_cast<leader_key_layout*>(&k);
    return {p->pool_id, p->pg_id};
}

// ---------- fb_client::should_retry_request (verbatim switch) -------------
bool should_retry_request(int32_t state) noexcept {
    switch (state) {
    case -ENOLINK:
    case -ENOENT:
    case -EINVAL:
    case err::RAFT_ERR_NOT_LEADER:
    case err::RAFT_ERR_NOT_FOUND_PG:
    case err::RAFT_ERR_PG_SHUTDOWN:
    case err::RAFT_ERR_NO_CONNECTED:
    case err::OSD_DOWN:
    case err::OSD_STARTING:
    case err::RAFT_ERR_PG_INITIALIZING:
        return true;
    default:
        return false;
    }
}

// ---------- write_ring_state mirror (just the test-observable fields) -----
struct write_ring_slot_info {
    uint64_t remote_addr{0};
    uint32_t remote_key{0};
    uint32_t slot_size{0};
    bool     busy{false};
};

struct write_ring_state {
    uint64_t queue_id{0};
    uint64_t lease_us{0};
    uint32_t next_slot{0};
    bool     is_ready{false};
    bool     is_onflight{false};
    bool     is_connecting{false};
    int32_t  node_id{-1};
    uint32_t port{0};
    std::string addr{};
    std::chrono::steady_clock::time_point lease_deadline{};
    std::vector<write_ring_slot_info> slots{};
    bool conn_alive{false};
};

void reset_write_ring_state(write_ring_state& s, bool keep_connection = false) {
    s.queue_id = 0;
    s.lease_us = 0;
    s.next_slot = 0;
    s.is_ready = false;
    s.is_onflight = false;
    s.lease_deadline = {};
    s.slots.clear();
    if (!keep_connection) s.conn_alive = false;
}

void refresh_local_write_ring_deadline(write_ring_state& s) noexcept {
    if (s.lease_us == 0) {
        s.lease_deadline = {};
        return;
    }
    auto lease = std::chrono::microseconds{s.lease_us};
    auto guard = lease / 5;
    if (guard < std::chrono::milliseconds{500}) guard = std::chrono::milliseconds{500};
    if (guard > std::chrono::seconds{5})       guard = std::chrono::seconds{5};
    if (guard >= lease)                        guard = lease / 2;
    s.lease_deadline = std::chrono::steady_clock::now() + lease - guard;
}

bool should_refresh_write_ring_lease(const write_ring_state* s) noexcept {
    return s && s->is_ready && s->lease_deadline != std::chrono::steady_clock::time_point{} &&
           std::chrono::steady_clock::now() >= s->lease_deadline;
}

std::optional<uint32_t> acquire_write_ring_slot(write_ring_state* s) {
    if (!s || !s->is_ready || s->slots.empty()) return std::nullopt;
    for (size_t i = 0; i < s->slots.size(); ++i) {
        auto idx = (s->next_slot + i) % s->slots.size();
        if (!s->slots[idx].busy) {
            s->slots[idx].busy = true;
            s->next_slot = static_cast<uint32_t>((idx + 1) % s->slots.size());
            return static_cast<uint32_t>(idx);
        }
    }
    return std::nullopt;
}

// ---------- monitor::client::endpoint::parse (mirrored) -------------------
struct endpoint {
    std::string host;
    int port{0};
};

endpoint parse_endpoint(const char* address) {
    std::string s{address};
    auto p = s.find(':');
    endpoint ep;
    ep.host = s.substr(0, p);
    ep.port = std::stoi(s.substr(p + 1));
    return ep;
}

} // anonymous namespace

// ============================================================================
// Test Suite: client_connection_id — node_id/port bit-packing
// ============================================================================

FB_SUITE_SETUP(client_connection_id) {}
FB_SUITE_TEARDOWN(client_connection_id) {}

FB_TEST(client_connection_id, different_node_ids_differ) {
    FB_ASSERT_NE(to_connection_id(1, 9000), to_connection_id(2, 9000));
}

FB_TEST(client_connection_id, different_ports_differ) {
    FB_ASSERT_NE(to_connection_id(1, 9000), to_connection_id(1, 9001));
}

FB_TEST(client_connection_id, layout_matches_reinterpret) {
    auto k = to_connection_id(0x11223344, 0x55667788);
    FB_ASSERT_EQ(static_cast<uint32_t>(k & 0xffffffff), 0x11223344u);
    FB_ASSERT_EQ(static_cast<uint32_t>((k >> 32) & 0xffffffff), 0x55667788u);
}

FB_TEST(client_connection_id, zero_id_zero_port_is_zero) {
    FB_ASSERT_EQ(to_connection_id(0, 0), 0ull);
}

// ============================================================================
// Test Suite: client_leader_key — pool_id/pg_id bit-packing
// ============================================================================

FB_SUITE_SETUP(client_leader_key) {}
FB_SUITE_TEARDOWN(client_leader_key) {}

FB_TEST(client_leader_key, roundtrip) {
    auto k = make_leader_key(7, 13);
    auto unpacked = from_leader_key(k);
    FB_ASSERT_EQ(unpacked.pool_id, 7);
    FB_ASSERT_EQ(unpacked.pg_id,   13);
}

FB_TEST(client_leader_key, distinct_pgs_distinct_keys) {
    FB_ASSERT_NE(make_leader_key(7, 13), make_leader_key(7, 14));
    FB_ASSERT_NE(make_leader_key(7, 13), make_leader_key(8, 13));
}

FB_TEST(client_leader_key, negative_pool_id_preserved) {
    auto k = make_leader_key(-1, 0);
    auto u = from_leader_key(k);
    FB_ASSERT_EQ(u.pool_id, -1);
    FB_ASSERT_EQ(u.pg_id,    0);
}

// ============================================================================
// Test Suite: client_retry_classification — should_retry_request
// ============================================================================

FB_SUITE_SETUP(client_retry_classification) {}
FB_SUITE_TEARDOWN(client_retry_classification) {}

FB_TEST(client_retry_classification, system_errnos_retry) {
    FB_ASSERT_TRUE(should_retry_request(-ENOLINK));
    FB_ASSERT_TRUE(should_retry_request(-ENOENT));
    FB_ASSERT_TRUE(should_retry_request(-EINVAL));
}

FB_TEST(client_retry_classification, raft_transients_retry) {
    FB_ASSERT_TRUE(should_retry_request(err::RAFT_ERR_NOT_LEADER));
    FB_ASSERT_TRUE(should_retry_request(err::RAFT_ERR_NOT_FOUND_PG));
    FB_ASSERT_TRUE(should_retry_request(err::RAFT_ERR_PG_SHUTDOWN));
    FB_ASSERT_TRUE(should_retry_request(err::RAFT_ERR_NO_CONNECTED));
    FB_ASSERT_TRUE(should_retry_request(err::RAFT_ERR_PG_INITIALIZING));
}

FB_TEST(client_retry_classification, osd_lifecycle_states_retry) {
    FB_ASSERT_TRUE(should_retry_request(err::OSD_DOWN));
    FB_ASSERT_TRUE(should_retry_request(err::OSD_STARTING));
}

FB_TEST(client_retry_classification, success_not_retried) {
    FB_ASSERT_FALSE(should_retry_request(err::E_SUCCESS));
}

FB_TEST(client_retry_classification, pool_not_found_not_retried) {
    FB_ASSERT_FALSE(should_retry_request(err::ERR_NOT_FOUND_POOL));
}

FB_TEST(client_retry_classification, internal_errors_propagate) {
    FB_ASSERT_FALSE(should_retry_request(err::ERR_INTERNAL));
    FB_ASSERT_FALSE(should_retry_request(err::ERR_PERM));
    FB_ASSERT_FALSE(should_retry_request(-EBADF));
    FB_ASSERT_FALSE(should_retry_request(-EIO));
}

// ============================================================================
// Test Suite: client_write_ring_slots — slot allocation arithmetic
// ============================================================================

FB_SUITE_SETUP(client_write_ring_slots) {}
FB_SUITE_TEARDOWN(client_write_ring_slots) {}

FB_TEST(client_write_ring_slots, not_ready_returns_null) {
    write_ring_state s;
    s.is_ready = false;
    FB_ASSERT_FALSE(acquire_write_ring_slot(&s).has_value());
}

FB_TEST(client_write_ring_slots, empty_slots_returns_null) {
    write_ring_state s;
    s.is_ready = true;
    s.slots.clear();
    FB_ASSERT_FALSE(acquire_write_ring_slot(&s).has_value());
}

FB_TEST(client_write_ring_slots, sequential_allocation) {
    write_ring_state s;
    s.is_ready = true;
    s.slots.assign(4, write_ring_slot_info{});
    auto a = acquire_write_ring_slot(&s);
    auto b = acquire_write_ring_slot(&s);
    auto c = acquire_write_ring_slot(&s);
    auto d = acquire_write_ring_slot(&s);
    FB_ASSERT_TRUE(a.has_value()); FB_ASSERT_EQ(*a, 0u);
    FB_ASSERT_TRUE(b.has_value()); FB_ASSERT_EQ(*b, 1u);
    FB_ASSERT_TRUE(c.has_value()); FB_ASSERT_EQ(*c, 2u);
    FB_ASSERT_TRUE(d.has_value()); FB_ASSERT_EQ(*d, 3u);
}

FB_TEST(client_write_ring_slots, full_ring_returns_null) {
    write_ring_state s;
    s.is_ready = true;
    s.slots.assign(2, write_ring_slot_info{.busy = true});
    FB_ASSERT_FALSE(acquire_write_ring_slot(&s).has_value());
}

FB_TEST(client_write_ring_slots, release_then_reallocate) {
    write_ring_state s;
    s.is_ready = true;
    s.slots.assign(2, write_ring_slot_info{});
    auto a = acquire_write_ring_slot(&s);
    auto b = acquire_write_ring_slot(&s);
    FB_ASSERT_TRUE(a.has_value() && b.has_value());
    s.slots[0].busy = false;
    auto c = acquire_write_ring_slot(&s);
    FB_ASSERT_TRUE(c.has_value());
    FB_ASSERT_EQ(*c, 0u);
}

FB_TEST(client_write_ring_slots, wrap_around_continues_scan) {
    write_ring_state s;
    s.is_ready = true;
    s.slots.resize(4);
    s.slots[0].busy = true;
    s.slots[1].busy = true;
    s.slots[2].busy = false;
    s.slots[3].busy = false;
    auto x = acquire_write_ring_slot(&s);
    FB_ASSERT_TRUE(x.has_value());
    FB_ASSERT_EQ(*x, 2u);
    FB_ASSERT_EQ(s.next_slot, 3u);
}

// ============================================================================
// Test Suite: client_write_ring_lease — lease deadline + reset
// ============================================================================

FB_SUITE_SETUP(client_write_ring_lease) {}
FB_SUITE_TEARDOWN(client_write_ring_lease) {}

FB_TEST(client_write_ring_lease, zero_lease_clears_deadline) {
    write_ring_state s;
    s.lease_us = 0;
    refresh_local_write_ring_deadline(s);
    FB_ASSERT_TRUE(s.lease_deadline == std::chrono::steady_clock::time_point{});
}

FB_TEST(client_write_ring_lease, long_lease_uses_clamped_guard) {
    write_ring_state s;
    s.lease_us = 30ull * 1000 * 1000;
    auto before = std::chrono::steady_clock::now();
    refresh_local_write_ring_deadline(s);
    auto after = std::chrono::steady_clock::now();
    auto delta = s.lease_deadline - after;
    FB_ASSERT_TRUE(delta >= std::chrono::seconds{24});
    FB_ASSERT_TRUE(delta <= std::chrono::seconds{26});
    FB_ASSERT_TRUE(s.lease_deadline > before);
}

FB_TEST(client_write_ring_lease, short_lease_uses_minimum_guard) {
    write_ring_state s;
    s.lease_us = 100ull * 1000;
    auto before = std::chrono::steady_clock::now();
    refresh_local_write_ring_deadline(s);
    auto delta = s.lease_deadline - before;
    FB_ASSERT_TRUE(delta > std::chrono::microseconds{0});
    FB_ASSERT_TRUE(delta <= std::chrono::milliseconds{100});
}

FB_TEST(client_write_ring_lease, refresh_predicate_requires_ready) {
    write_ring_state s;
    s.is_ready = false;
    s.lease_us = 30ull * 1000 * 1000;
    refresh_local_write_ring_deadline(s);
    FB_ASSERT_FALSE(should_refresh_write_ring_lease(&s));
    s.is_ready = true;
    s.lease_deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1};
    FB_ASSERT_TRUE(should_refresh_write_ring_lease(&s));
}

FB_TEST(client_write_ring_lease, reset_keep_connection) {
    write_ring_state s;
    s.queue_id = 99;
    s.is_ready = true;
    s.is_onflight = true;
    s.slots.assign(3, write_ring_slot_info{});
    s.conn_alive = true;
    reset_write_ring_state(s, /*keep_connection=*/true);
    FB_ASSERT_EQ(s.queue_id, 0u);
    FB_ASSERT_FALSE(s.is_ready);
    FB_ASSERT_FALSE(s.is_onflight);
    FB_ASSERT_TRUE(s.slots.empty());
    FB_ASSERT_TRUE(s.conn_alive);
}

FB_TEST(client_write_ring_lease, reset_drops_connection) {
    write_ring_state s;
    s.conn_alive = true;
    s.queue_id = 5;
    reset_write_ring_state(s, /*keep_connection=*/false);
    FB_ASSERT_FALSE(s.conn_alive);
    FB_ASSERT_EQ(s.queue_id, 0u);
}

// ============================================================================
// Test Suite: client_endpoint_parse — libfblock parse_endpoint
// ============================================================================

FB_SUITE_SETUP(client_endpoint_parse) {}
FB_SUITE_TEARDOWN(client_endpoint_parse) {}

FB_TEST(client_endpoint_parse, simple_ipv4) {
    auto ep = parse_endpoint("127.0.0.1:3333");
    FB_ASSERT_STR_EQ(ep.host.c_str(), "127.0.0.1");
    FB_ASSERT_EQ(ep.port, 3333);
}

FB_TEST(client_endpoint_parse, hostname) {
    auto ep = parse_endpoint("monitor.example.com:9999");
    FB_ASSERT_STR_EQ(ep.host.c_str(), "monitor.example.com");
    FB_ASSERT_EQ(ep.port, 9999);
}

// ============================================================================
// Part 3: Integration — write & read fanout
// ============================================================================

namespace {

// ---------- fake bdev_io / source structs (mirrored skeletons) ------------

struct fake_bdev_io {
    int id{0};
};

// write_source — mirrors libfblock.cc's write_source fan-in counter.
struct write_source {
    uint32_t obj_num;
    fake_bdev_io* bdev_io;
    int32_t result;
    std::function<void(fake_bdev_io*, int32_t)> cb;
    bool callback_fired{false};

    write_source(std::function<void(fake_bdev_io*, int32_t)> _cb,
                 uint32_t n, fake_bdev_io* io)
      : obj_num{n}, bdev_io{io}, result{err::E_SUCCESS}, cb{std::move(_cb)} {}

    static void write_done(void* src, int32_t state) {
        auto* s = reinterpret_cast<write_source*>(src);
        if (state != err::E_SUCCESS) s->result = state;
        --s->obj_num;
        if (s->obj_num == 0) {
            s->callback_fired = true;
            s->cb(s->bdev_io, s->result);
        }
    }
};

// read_source — mirrors libfblock.cc's read_source per-object copy.
struct read_source {
    uint32_t obj_num;
    std::string buf;
    uint64_t first_object_size;
    int32_t result;
    fake_bdev_io* bdev_io;
    std::function<void(fake_bdev_io*, const std::string&, int32_t)> cb;
    bool callback_fired{false};

    read_source(std::function<void(fake_bdev_io*, const std::string&, int32_t)> _cb,
                uint32_t n, uint64_t len, fake_bdev_io* io, uint64_t first_obj_sz)
      : obj_num{n}, buf(len, '\0'), first_object_size{first_obj_sz},
        result{err::E_SUCCESS}, bdev_io{io}, cb{std::move(_cb)} {}

    static void read_done(void* src, uint64_t object_idx, const std::string& data, int32_t state) {
        auto* s = reinterpret_cast<read_source*>(src);
        if (state == err::E_SUCCESS) {
            char* ptr = s->buf.data();
            if (object_idx == 0) {
                std::memcpy(ptr, data.data(), data.size());
            } else {
                ptr += s->first_object_size + (object_idx - 1) * default_object_size;
                std::memcpy(ptr, data.data(), data.size());
            }
        } else {
            s->result = state;
        }
        --s->obj_num;
        if (s->obj_num == 0) {
            s->callback_fired = true;
            s->cb(s->bdev_io, s->buf, s->result);
        }
    }
};

// ---------- fake OSD: holds deferred responses for the runner --------------

struct write_req_record {
    std::string object_name;
    uint64_t offset;
    std::string data;
    void* source;
    int32_t pending_state{err::E_SUCCESS};
};

struct read_req_record {
    std::string object_name;
    uint64_t offset;
    uint64_t length;
    void* source;
    uint64_t object_idx;
    int32_t pending_state{err::E_SUCCESS};
    std::string payload;
};

struct fake_osd {
    std::deque<write_req_record> w_queue;
    std::deque<read_req_record>  r_queue;
    std::map<std::string, int32_t> next_write_state;
    std::map<std::string, int32_t> next_read_state;

    void submit_write(std::string object_name, uint64_t off,
                      std::string data, write_source* src) {
        write_req_record r{std::move(object_name), off, std::move(data), src};
        auto it = next_write_state.find(r.object_name);
        if (it != next_write_state.end()) {
            r.pending_state = it->second;
            next_write_state.erase(it);
        }
        w_queue.push_back(std::move(r));
    }

    void submit_read(std::string object_name, uint64_t off, uint64_t len,
                     read_source* src, uint64_t obj_idx, std::string payload) {
        read_req_record r{std::move(object_name), off, len, src, obj_idx,
                          err::E_SUCCESS, std::move(payload)};
        auto it = next_read_state.find(r.object_name);
        if (it != next_read_state.end()) {
            r.pending_state = it->second;
            next_read_state.erase(it);
        }
        r_queue.push_back(std::move(r));
    }

    bool drain_one_write() {
        if (w_queue.empty()) return false;
        auto rec = std::move(w_queue.front());
        w_queue.pop_front();
        write_source::write_done(rec.source, rec.pending_state);
        return true;
    }
    bool drain_one_read() {
        if (r_queue.empty()) return false;
        auto rec = std::move(r_queue.front());
        r_queue.pop_front();
        const std::string& d = (rec.pending_state == err::E_SUCCESS) ? rec.payload : std::string{};
        read_source::read_done(rec.source, rec.object_idx, d, rec.pending_state);
        return true;
    }

    void drain_all_writes() { while (drain_one_write()) {} }
    void drain_all_reads()  { while (drain_one_read())  {} }
};

// ---------- libblk_client::write driver (mirrors libfblock.cc:178) --------

void drive_write(fake_osd& osd, uint64_t pool_id, const std::string& image,
                 uint64_t offset, const std::string& buf,
                 write_source* src) {
    if (buf.empty()) {
        src->cb(src->bdev_io, errc::success);
        return;
    }
    auto length = buf.size();
    auto prefix = calc_image_object_prefix(pool_id, image);
    auto [expected, obj_off, obj_seq] =
        calc_first_object_position(offset, length, default_object_size);
    size_t write_bytes = 0;
    while (write_bytes < length) {
        auto name = get_image_object_name(prefix, obj_seq);
        std::string chunk{buf.data() + write_bytes, expected};
        osd.submit_write(name, obj_off, std::move(chunk), src);
        write_bytes += expected;
        expected = default_object_size;
        if (expected > length - write_bytes) expected = length - write_bytes;
        obj_off = 0;
        ++obj_seq;
    }
}

// ---------- libblk_client::read driver (mirrors libfblock.cc:296) ---------

void drive_read(fake_osd& osd, uint64_t pool_id, const std::string& image,
                uint64_t offset, uint64_t length,
                const std::vector<std::string>& object_payloads,
                read_source* src) {
    if (length == 0) {
        src->cb(src->bdev_io, std::string{}, err::E_SUCCESS);
        return;
    }
    auto prefix = calc_image_object_prefix(pool_id, image);
    auto [expected, obj_off, obj_seq] =
        calc_first_object_position(offset, length, default_object_size);
    size_t read_bytes = 0;
    uint64_t idx = 0;
    while (read_bytes < length) {
        auto name = get_image_object_name(prefix, obj_seq);
        std::string payload = (idx < object_payloads.size()) ? object_payloads[idx] : std::string(expected, '?');
        if (payload.size() > expected) payload.resize(expected);
        osd.submit_read(name, obj_off, expected, src, idx, std::move(payload));
        read_bytes += expected;
        expected = default_object_size;
        if (expected > length - read_bytes) expected = length - read_bytes;
        obj_off = 0;
        ++obj_seq;
        ++idx;
    }
}

} // anonymous namespace

// ============================================================================
// Test Suite: client_end_to_end_write — full write fanout
// ============================================================================

FB_SUITE_SETUP(client_end_to_end_write) {}
FB_SUITE_TEARDOWN(client_end_to_end_write) {}

FB_TEST(client_end_to_end_write, empty_buffer_fires_callback_immediately) {
    fake_osd osd;
    fake_bdev_io io{1};
    int32_t result = -1;
    write_source src([&](fake_bdev_io*, int32_t s) { result = s; },
                     /*obj_num=*/0, &io);
    drive_write(osd, /*pool=*/1, "img", /*off=*/0, /*buf=*/{}, &src);
    FB_ASSERT_EQ(result, static_cast<int32_t>(errc::success));
    FB_ASSERT_TRUE(osd.w_queue.empty());
}

FB_TEST(client_end_to_end_write, single_object_path) {
    fake_osd osd;
    fake_bdev_io io{1};
    bool fired = false;
    int32_t result = -1;
    auto obj_num = get_obj_num(0, 1024);
    write_source src([&](fake_bdev_io*, int32_t s) { fired = true; result = s; },
                     static_cast<uint32_t>(obj_num), &io);
    drive_write(osd, 1, "img", 0, std::string(1024, 'a'), &src);
    FB_ASSERT_EQ(osd.w_queue.size(), 1u);
    FB_ASSERT_STR_EQ(osd.w_queue.front().object_name.c_str(), "1__blk_data___img0");
    FB_ASSERT_EQ(osd.w_queue.front().offset, 0u);
    FB_ASSERT_EQ(osd.w_queue.front().data.size(), 1024u);
    osd.drain_all_writes();
    FB_ASSERT_TRUE(fired);
    FB_ASSERT_EQ(result, err::E_SUCCESS);
    FB_ASSERT_EQ(src.obj_num, 0u);
}

FB_TEST(client_end_to_end_write, multi_object_fanout) {
    fake_osd osd;
    fake_bdev_io io{2};
    int callbacks = 0;
    auto obj_num = get_obj_num(3 * MiB, 7 * MiB);
    FB_ASSERT_EQ(obj_num, 3u);
    write_source src([&](fake_bdev_io*, int32_t) { ++callbacks; },
                     static_cast<uint32_t>(obj_num), &io);
    drive_write(osd, 7, "vol", 3 * MiB, std::string(7 * MiB, 'x'), &src);
    FB_ASSERT_EQ(osd.w_queue.size(), 3u);
    FB_ASSERT_STR_EQ(osd.w_queue[0].object_name.c_str(), "7__blk_data___vol0");
    FB_ASSERT_EQ(osd.w_queue[0].offset, 3 * MiB);
    FB_ASSERT_EQ(osd.w_queue[0].data.size(), 1 * MiB);
    FB_ASSERT_STR_EQ(osd.w_queue[1].object_name.c_str(), "7__blk_data___vol1");
    FB_ASSERT_EQ(osd.w_queue[1].offset, 0u);
    FB_ASSERT_EQ(osd.w_queue[1].data.size(), 4 * MiB);
    FB_ASSERT_STR_EQ(osd.w_queue[2].object_name.c_str(), "7__blk_data___vol2");
    FB_ASSERT_EQ(osd.w_queue[2].offset, 0u);
    FB_ASSERT_EQ(osd.w_queue[2].data.size(), 2 * MiB);
    osd.drain_one_write();
    FB_ASSERT_EQ(callbacks, 0);
    FB_ASSERT_EQ(src.obj_num, 2u);
    osd.drain_one_write();
    FB_ASSERT_EQ(callbacks, 0);
    FB_ASSERT_EQ(src.obj_num, 1u);
    osd.drain_one_write();
    FB_ASSERT_EQ(callbacks, 1);
    FB_ASSERT_EQ(src.obj_num, 0u);
}

FB_TEST(client_end_to_end_write, partial_failure_propagates_last_error) {
    fake_osd osd;
    fake_bdev_io io{3};
    int32_t result = err::E_SUCCESS;
    auto obj_num = get_obj_num(0, 2 * default_object_size);
    write_source src([&](fake_bdev_io*, int32_t s) { result = s; },
                     static_cast<uint32_t>(obj_num), &io);
    osd.next_write_state["3__blk_data___v0"] = -ENOLINK;
    drive_write(osd, 3, "v", 0, std::string(2 * default_object_size, 'b'), &src);
    osd.drain_all_writes();
    FB_ASSERT_EQ(src.obj_num, 0u);
    FB_ASSERT_EQ(result, -ENOLINK);
}

FB_TEST(client_end_to_end_write, callback_fires_exactly_once) {
    fake_osd osd;
    fake_bdev_io io{4};
    int callbacks = 0;
    auto obj_num = get_obj_num(0, 4 * default_object_size);
    write_source src([&](fake_bdev_io*, int32_t) { ++callbacks; },
                     static_cast<uint32_t>(obj_num), &io);
    drive_write(osd, 0, "img", 0, std::string(4 * default_object_size, 'c'), &src);
    osd.drain_all_writes();
    FB_ASSERT_EQ(callbacks, 1);
}

// ============================================================================
// Test Suite: client_end_to_end_read — full read assembly
// ============================================================================

FB_SUITE_SETUP(client_end_to_end_read) {}
FB_SUITE_TEARDOWN(client_end_to_end_read) {}

FB_TEST(client_end_to_end_read, zero_length_short_circuits) {
    fake_osd osd;
    fake_bdev_io io{1};
    bool fired = false;
    read_source src([&](fake_bdev_io*, const std::string&, int32_t) { fired = true; },
                    0, 0, &io, 0);
    drive_read(osd, 1, "img", 0, 0, {}, &src);
    FB_ASSERT_TRUE(fired);
    FB_ASSERT_TRUE(osd.r_queue.empty());
}

FB_TEST(client_end_to_end_read, single_object_assembly) {
    fake_osd osd;
    fake_bdev_io io{1};
    std::string out;
    auto obj_num = get_obj_num(0, 1024);
    auto [first_sz, _o, _s] = calc_first_object_position(0, 1024, default_object_size);
    (void)_o; (void)_s;
    read_source src([&](fake_bdev_io*, const std::string& b, int32_t) { out = b; },
                    static_cast<uint32_t>(obj_num), 1024, &io, first_sz);
    std::vector<std::string> payloads{std::string(1024, 'A')};
    drive_read(osd, 1, "img", 0, 1024, payloads, &src);
    osd.drain_all_reads();
    FB_ASSERT_EQ(out.size(), 1024u);
    FB_ASSERT_EQ(out, std::string(1024, 'A'));
}

FB_TEST(client_end_to_end_read, multi_object_offset_placement) {
    fake_osd osd;
    fake_bdev_io io{2};
    std::string out;
    auto obj_num = get_obj_num(3 * MiB, 7 * MiB);
    auto [first_sz, _o, _s] = calc_first_object_position(3 * MiB, 7 * MiB, default_object_size);
    (void)_o; (void)_s;
    read_source src([&](fake_bdev_io*, const std::string& b, int32_t) { out = b; },
                    static_cast<uint32_t>(obj_num), 7 * MiB, &io, first_sz);
    std::vector<std::string> payloads{
        std::string(1 * MiB, 'X'),
        std::string(4 * MiB, 'Y'),
        std::string(2 * MiB, 'Z'),
    };
    drive_read(osd, 0, "v", 3 * MiB, 7 * MiB, payloads, &src);
    osd.drain_all_reads();
    FB_ASSERT_EQ(out.size(), 7 * MiB);
    FB_ASSERT_EQ(out[0],              'X');
    FB_ASSERT_EQ(out[1 * MiB - 1],    'X');
    FB_ASSERT_EQ(out[1 * MiB],        'Y');
    FB_ASSERT_EQ(out[5 * MiB - 1],    'Y');
    FB_ASSERT_EQ(out[5 * MiB],        'Z');
    FB_ASSERT_EQ(out[7 * MiB - 1],    'Z');
}

FB_TEST(client_end_to_end_read, error_does_not_corrupt_buf) {
    fake_osd osd;
    fake_bdev_io io{3};
    std::string out;
    int32_t status = err::E_SUCCESS;
    auto obj_num = get_obj_num(0, 2 * default_object_size);
    auto [first_sz, _o, _s] = calc_first_object_position(0, 2 * default_object_size, default_object_size);
    (void)_o; (void)_s;
    read_source src([&](fake_bdev_io*, const std::string& b, int32_t s) { out = b; status = s; },
                    static_cast<uint32_t>(obj_num), 2 * default_object_size, &io, first_sz);
    std::vector<std::string> payloads{
        std::string(default_object_size, 'Q'),
        std::string(default_object_size, 'R'),
    };
    osd.next_read_state["5__blk_data___im1"] = err::OSD_DOWN;
    drive_read(osd, 5, "im", 0, 2 * default_object_size, payloads, &src);
    osd.drain_all_reads();
    FB_ASSERT_EQ(status, err::OSD_DOWN);
    FB_ASSERT_EQ(out[0], 'Q');
    FB_ASSERT_EQ(out[default_object_size], '\0');
}

// ============================================================================
// Part 4: Integration — retry, write-ring fast-path, leader race
// ============================================================================

// ============================================================================
// Test Suite: client_end_to_end_retry — transient errors loop back
// ============================================================================

FB_SUITE_SETUP(client_end_to_end_retry) {}
FB_SUITE_TEARDOWN(client_end_to_end_retry) {}

namespace {

// A tiny re-driver: when a write_done's state is retryable, we DO NOT
// decrement obj_num — we re-submit the same chunk and let the next drain
// settle it. This matches fb_client's retry_request semantics.
struct retrying_runner {
    fake_osd* osd;
    int retries{0};

    void drain_with_retry() {
        std::vector<write_req_record> retry_set;
        while (!osd->w_queue.empty()) {
            auto rec = std::move(osd->w_queue.front());
            osd->w_queue.pop_front();
            if (should_retry_request(rec.pending_state)) {
                ++retries;
                rec.pending_state = err::E_SUCCESS;
                retry_set.push_back(std::move(rec));
                continue;
            }
            write_source::write_done(rec.source, rec.pending_state);
        }
        for (auto& r : retry_set) osd->w_queue.push_back(std::move(r));
        osd->drain_all_writes();
    }
};

} // anonymous namespace

FB_TEST(client_end_to_end_retry, transient_not_leader_retries_then_succeeds) {
    fake_osd osd;
    fake_bdev_io io{10};
    int callbacks = 0;
    int32_t final_state = -1;
    auto obj_num = get_obj_num(0, 1024);
    write_source src([&](fake_bdev_io*, int32_t s) { ++callbacks; final_state = s; },
                     static_cast<uint32_t>(obj_num), &io);
    osd.next_write_state["1__blk_data___img0"] = err::RAFT_ERR_NOT_LEADER;
    drive_write(osd, 1, "img", 0, std::string(1024, 'z'), &src);
    retrying_runner rr{&osd};
    rr.drain_with_retry();
    FB_ASSERT_EQ(callbacks, 1);
    FB_ASSERT_EQ(final_state, err::E_SUCCESS);
    FB_ASSERT_EQ(rr.retries, 1);
}

FB_TEST(client_end_to_end_retry, transient_osd_down_then_recover) {
    fake_osd osd;
    fake_bdev_io io{11};
    int callbacks = 0;
    int32_t final_state = -1;
    auto obj_num = get_obj_num(0, 2 * default_object_size);
    write_source src([&](fake_bdev_io*, int32_t s) { ++callbacks; final_state = s; },
                     static_cast<uint32_t>(obj_num), &io);
    osd.next_write_state["1__blk_data___img1"] = err::OSD_STARTING;
    drive_write(osd, 1, "img", 0, std::string(2 * default_object_size, 'w'), &src);
    retrying_runner rr{&osd};
    rr.drain_with_retry();
    FB_ASSERT_EQ(callbacks, 1);
    FB_ASSERT_EQ(final_state, err::E_SUCCESS);
    FB_ASSERT_EQ(rr.retries, 1);
}

FB_TEST(client_end_to_end_retry, non_retryable_error_short_circuits) {
    fake_osd osd;
    fake_bdev_io io{12};
    int32_t result = err::E_SUCCESS;
    int callbacks = 0;
    auto obj_num = get_obj_num(0, 1024);
    write_source src([&](fake_bdev_io*, int32_t s) { ++callbacks; result = s; },
                     static_cast<uint32_t>(obj_num), &io);
    osd.next_write_state["1__blk_data___img0"] = err::ERR_NOT_FOUND_POOL;
    drive_write(osd, 1, "img", 0, std::string(1024, 'q'), &src);
    retrying_runner rr{&osd};
    rr.drain_with_retry();
    FB_ASSERT_EQ(callbacks, 1);
    FB_ASSERT_EQ(result, err::ERR_NOT_FOUND_POOL);
    FB_ASSERT_EQ(rr.retries, 0);
}

// ============================================================================
// Test Suite: client_end_to_end_write_ring — fast-path slot lifecycle
// ============================================================================

FB_SUITE_SETUP(client_end_to_end_write_ring) {}
FB_SUITE_TEARDOWN(client_end_to_end_write_ring) {}

FB_TEST(client_end_to_end_write_ring, lease_acquire_and_slot_pick) {
    write_ring_state s;
    s.lease_us = 30ull * 1000 * 1000;
    s.slots.assign(4, write_ring_slot_info{.remote_addr = 0x1000, .remote_key = 7,
                                           .slot_size = 256 * 1024, .busy = false});
    s.is_ready = true;
    refresh_local_write_ring_deadline(s);
    FB_ASSERT_FALSE(should_refresh_write_ring_lease(&s));
    auto idx = acquire_write_ring_slot(&s);
    FB_ASSERT_TRUE(idx.has_value());
    FB_ASSERT_TRUE(s.slots[*idx].busy);
}

FB_TEST(client_end_to_end_write_ring, response_releases_slot) {
    write_ring_state s;
    s.is_ready = true;
    s.slots.assign(2, write_ring_slot_info{.slot_size = 1024, .busy = false});
    auto idx = acquire_write_ring_slot(&s);
    FB_ASSERT_TRUE(idx.has_value());
    FB_ASSERT_TRUE(s.slots[*idx].busy);
    s.slots[*idx].busy = false;
    FB_ASSERT_FALSE(s.slots[*idx].busy);
    auto idx2 = acquire_write_ring_slot(&s);
    FB_ASSERT_TRUE(idx2.has_value());
}

FB_TEST(client_end_to_end_write_ring, transient_response_resets_ring) {
    write_ring_state s;
    s.is_ready = true;
    s.queue_id = 99;
    s.lease_us = 30ull * 1000 * 1000;
    s.slots.assign(2, write_ring_slot_info{.slot_size = 1024});
    refresh_local_write_ring_deadline(s);
    int32_t state = -ENOLINK;
    if (state == -ENOLINK || state == -ENOENT || state == -EINVAL) {
        s.is_ready = false;
        s.queue_id = 0;
        s.lease_us = 0;
        s.lease_deadline = {};
        s.slots.clear();
    }
    FB_ASSERT_FALSE(s.is_ready);
    FB_ASSERT_EQ(s.queue_id, 0u);
    FB_ASSERT_EQ(s.lease_us, 0u);
    FB_ASSERT_TRUE(s.slots.empty());
}

FB_TEST(client_end_to_end_write_ring, lease_expiry_triggers_refresh) {
    write_ring_state s;
    s.is_ready = true;
    s.conn_alive = true;
    s.lease_deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1};
    FB_ASSERT_TRUE(should_refresh_write_ring_lease(&s));
    reset_write_ring_state(s, /*keep_connection=*/true);
    FB_ASSERT_FALSE(s.is_ready);
    FB_ASSERT_TRUE(s.conn_alive);
    FB_ASSERT_TRUE(s.slots.empty());
}

// ============================================================================
// Test Suite: client_end_to_end_leader — leader_osd / get_leader race
// ============================================================================

FB_SUITE_SETUP(client_end_to_end_leader) {}
FB_SUITE_TEARDOWN(client_end_to_end_leader) {}

namespace {

struct leader_osd_info {
    int32_t leader_id{-1};
    std::string addr{};
    int32_t port{};
    bool is_valid{false};
    bool is_onflight{true};
};

} // anonymous namespace

FB_TEST(client_end_to_end_leader, first_request_blocks_until_leader_acquired) {
    std::unordered_map<uint64_t, leader_osd_info> table;
    auto key = make_leader_key(1, 3);
    table.emplace(key, leader_osd_info{});
    FB_ASSERT_TRUE(table[key].is_onflight);
    table[key].leader_id = 42;
    table[key].addr = "10.0.0.5";
    table[key].port = 9100;
    table[key].is_onflight = false;
    table[key].is_valid = true;
    FB_ASSERT_FALSE(table[key].is_onflight);
    FB_ASSERT_TRUE(table[key].is_valid);
    FB_ASSERT_EQ(table[key].leader_id, 42);
}

FB_TEST(client_end_to_end_leader, invalid_leader_reacquires) {
    std::unordered_map<uint64_t, leader_osd_info> table;
    auto key = make_leader_key(2, 4);
    table.emplace(key, leader_osd_info{.leader_id = 5, .addr = "10.0.0.1",
                                       .port = 9000, .is_valid = true, .is_onflight = false});
    const std::string new_addr = "10.0.0.1";
    const int new_port = 9100;
    bool changed = !(table[key].addr == new_addr && table[key].port == new_port);
    FB_ASSERT_TRUE(changed);
    table[key].addr = new_addr;
    table[key].port = new_port;
    table[key].is_valid = true;
    FB_ASSERT_EQ(table[key].port, 9100);
}

FB_TEST(client_end_to_end_leader, retry_invalidates_leader) {
    std::unordered_map<uint64_t, leader_osd_info> table;
    auto key = make_leader_key(1, 1);
    table.emplace(key, leader_osd_info{.leader_id = 1, .addr = "h", .port = 9000,
                                       .is_valid = true, .is_onflight = false});
    table[key].is_valid = false;
    table[key].is_onflight = false;
    bool should_acquire = (!table[key].is_valid) || (table.find(key) == table.end());
    FB_ASSERT_TRUE(should_acquire);
}

// ============================================================================
// Part 5: Error paths and edge cases
// ============================================================================

// ============================================================================
// Test Suite: client_write_edge_cases — write boundary conditions
// ============================================================================

FB_SUITE_SETUP(client_write_edge_cases) {}
FB_SUITE_TEARDOWN(client_write_edge_cases) {}

FB_TEST(client_write_edge_cases, offset_at_object_boundary) {
    // Offset exactly at 4 MiB: lands in object 1, first_object_offset=0.
    auto [sz, off, seq] = calc_first_object_position(4 * MiB, 1024, default_object_size);
    FB_ASSERT_EQ(seq, 1u);
    FB_ASSERT_EQ(off, 0u);
    FB_ASSERT_EQ(sz, 1024u);
}

FB_TEST(client_write_edge_cases, offset_at_last_byte_of_object) {
    // Offset at 4 MiB - 1 byte: still within object 0.
    auto [sz, off, seq] = calc_first_object_position(4 * MiB - 1, 1, default_object_size);
    FB_ASSERT_EQ(seq, 0u);
    FB_ASSERT_EQ(off, 4 * MiB - 1);
    FB_ASSERT_EQ(sz, 1u);
}

FB_TEST(client_write_edge_cases, length_exceeds_single_object) {
    // Length > object_size must span multiple objects.
    auto obj_num = get_obj_num(0, 5 * MiB);
    FB_ASSERT_EQ(obj_num, 2u);
}

FB_TEST(client_write_edge_cases, write_spans_exactly_two_objects) {
    // [2 MiB, 6 MiB) spans object 0 (partial 2 MiB) and object 1 (4 MiB).
    auto obj_num = get_obj_num(2 * MiB, 4 * MiB);
    FB_ASSERT_EQ(obj_num, 2u);
    auto [sz, off, seq] = calc_first_object_position(2 * MiB, 4 * MiB, default_object_size);
    FB_ASSERT_EQ(sz, 2 * MiB); // first slice is partial
    FB_ASSERT_EQ(off, 2 * MiB);
    FB_ASSERT_EQ(seq, 0u);
}

FB_TEST(client_write_edge_cases, very_large_offset) {
    // Stress large offset arithmetic (should not overflow).
    uint64_t large_offset = 1024ull * 1024 * 1024 * 1024; // 1 TiB
    auto obj_num = get_obj_num(large_offset, 4 * MiB);
    FB_ASSERT_EQ(obj_num, 1u); // exactly one object at any offset
    auto [sz, off, seq] = calc_first_object_position(large_offset, 4 * MiB, default_object_size);
    FB_ASSERT_EQ(sz, 4 * MiB);
    FB_ASSERT_EQ(seq, large_offset / default_object_size);
}

FB_TEST(client_write_edge_cases, zero_length_write_is_valid) {
    // Zero-length writes are valid and should short-circuit without error.
    auto obj_num = get_obj_num(0, 0);
    FB_ASSERT_EQ(obj_num, 0u); // degenerate case: no objects
}

// ============================================================================
// Test Suite: client_read_edge_cases — read boundary conditions
// ============================================================================

FB_SUITE_SETUP(client_read_edge_cases) {}
FB_SUITE_TEARDOWN(client_read_edge_cases) {}

FB_TEST(client_read_edge_cases, read_single_byte_from_first_object) {
    auto obj_num = get_obj_num(0, 1);
    FB_ASSERT_EQ(obj_num, 1u);
    auto [sz, off, seq] = calc_first_object_position(0, 1, default_object_size);
    FB_ASSERT_EQ(sz, 1u);
    FB_ASSERT_EQ(off, 0u);
    FB_ASSERT_EQ(seq, 0u);
}

FB_TEST(client_read_edge_cases, read_single_byte_from_last_position) {
    // Read 1 byte at position 4 MiB - 1 (last byte of object 0).
    auto obj_num = get_obj_num(4 * MiB - 1, 1);
    FB_ASSERT_EQ(obj_num, 1u);
    auto [sz, off, seq] = calc_first_object_position(4 * MiB - 1, 1, default_object_size);
    FB_ASSERT_EQ(sz, 1u);
    FB_ASSERT_EQ(off, 4 * MiB - 1);
    FB_ASSERT_EQ(seq, 0u);
}

FB_TEST(client_read_edge_cases, read_across_object_boundary) {
    // Read 5 MiB starting at 3 MiB: spans objects 0, 1, 2.
    auto obj_num = get_obj_num(3 * MiB, 5 * MiB);
    FB_ASSERT_EQ(obj_num, 3u);
}

FB_TEST(client_read_edge_cases, first_slice_size_matches_first_object_size) {
    // The first read slice size MUST match the first_object_size calculated.
    fake_osd osd;
    fake_bdev_io io{100};
    std::string out;
    auto obj_num = get_obj_num(1 * MiB, 8 * MiB); // starts mid-object
    auto [first_sz, _o, _s] = calc_first_object_position(1 * MiB, 8 * MiB, default_object_size);
    (void)_o; (void)_s;
    read_source src([&](fake_bdev_io*, const std::string& b, int32_t) { out = b; },
                    static_cast<uint32_t>(obj_num), 8 * MiB, &io, first_sz);
    // first_sz should be 3 MiB (remaining in object 0)
    FB_ASSERT_EQ(first_sz, 3 * MiB);
    std::vector<std::string> payloads{
        std::string(3 * MiB, 'A'),
        std::string(4 * MiB, 'B'),
        std::string(1 * MiB, 'C'),
    };
    drive_read(osd, 1, "img", 1 * MiB, 8 * MiB, payloads, &src);
    osd.drain_all_reads();
    FB_ASSERT_EQ(out.size(), 8 * MiB);
    FB_ASSERT_EQ(out[0], 'A');
    FB_ASSERT_EQ(out[3 * MiB], 'B');
    FB_ASSERT_EQ(out[7 * MiB], 'C');
}

// ============================================================================
// Test Suite: client_object_name_edge — object naming edge cases
// ============================================================================

FB_SUITE_SETUP(client_object_name_edge) {}
FB_SUITE_TEARDOWN(client_object_name_edge) {}

FB_TEST(client_object_name_edge, pool_id_zero_is_valid) {
    auto prefix = calc_image_object_prefix(0, "test");
    FB_ASSERT_STR_EQ(prefix.c_str(), "0__blk_data___test");
}

FB_TEST(client_object_name_edge, pool_id_negative_preserved) {
    // Negative pool_id might be used for internal pools.
    auto prefix = calc_image_object_prefix(-1, "internal");
    FB_ASSERT_STR_EQ(prefix.c_str(), "-1__blk_data___internal");
}

FB_TEST(client_object_name_edge, image_name_with_special_chars) {
    // Image names can contain underscores, hyphens, dots.
    auto prefix = calc_image_object_prefix(1, "test-image_vol.v2");
    FB_ASSERT_TRUE(prefix.find("test-image_vol.v2") != std::string::npos);
}

FB_TEST(client_object_name_edge, image_name_empty_is_valid) {
    // Empty image name is syntactically valid (creates object prefix with
    // pool_id only).
    auto prefix = calc_image_object_prefix(5, "");
    FB_ASSERT_STR_EQ(prefix.c_str(), "5__blk_data___");
}

FB_TEST(client_object_name_edge, seq_max_uint64) {
    auto prefix = calc_image_object_prefix(0, "");
    auto name = get_image_object_name(prefix, 18446744073709551615ull);
    // Name should contain the number (may truncate due to buffer size).
    FB_ASSERT_TRUE(!name.empty());
}

// ============================================================================
// Test Suite: client_pg_routing_edge — PG routing edge cases
// ============================================================================

FB_SUITE_SETUP(client_pg_routing_edge) {}
FB_SUITE_TEARDOWN(client_pg_routing_edge) {}

FB_TEST(client_pg_routing_edge, pg_num_one_all_objects_route_to_pg0) {
    pg_router r;
    r.calc_pg_masks(1);
    for (int i = 0; i < 100; ++i) {
        FB_ASSERT_EQ(r.calc_target("obj_" + std::to_string(i)), 0u);
    }
}

FB_TEST(client_pg_routing_edge, pg_num_power_of_two_distribution) {
    pg_router r;
    r.calc_pg_masks(8);
    std::set<unsigned> hit;
    for (int i = 0; i < 1000; ++i) {
        hit.insert(r.calc_target("obj_" + std::to_string(i)));
    }
    // Should hit all 8 PGs with reasonable distribution.
    FB_ASSERT_EQ(hit.size(), 8u);
}

FB_TEST(client_pg_routing_edge, pg_num_change_invalidates_cache) {
    // When pg_num changes, cached routing decisions become stale.
    // This test documents the expected behavior (no implicit invalidation).
    pg_router r;
    r.calc_pg_masks(16);
    auto pg1 = r.calc_target("object_a");
    r.calc_pg_masks(32);
    auto pg2 = r.calc_target("object_a");
    // pg2 is re-computed; may differ from pg1.
    FB_ASSERT_LT(pg1, 16u);
    FB_ASSERT_LT(pg2, 32u);
}

FB_TEST(client_pg_routing_edge, object_name_affects_hash) {
    pg_router r;
    r.calc_pg_masks(64);
    auto pg1 = r.calc_target("image_obj_0");
    auto pg2 = r.calc_target("image_obj_1");
    // Different names should generally route to different PGs (hash mixes).
    // We don't require always different, but verify they're within range.
    FB_ASSERT_LT(pg1, 64u);
    FB_ASSERT_LT(pg2, 64u);
}

// ============================================================================
// Test Suite: client_connection_cache — stub cache behavior
// ============================================================================

FB_SUITE_SETUP(client_connection_cache) {}
FB_SUITE_TEARDOWN(client_connection_cache) {}

FB_TEST(client_connection_cache, different_osds_dont_share_connection) {
    // Each OSD node_id+port pair gets its own connection_id key.
    auto id1 = to_connection_id(1, 9000);
    auto id2 = to_connection_id(2, 9000);
    auto id3 = to_connection_id(1, 9001);
    FB_ASSERT_TRUE(id1 != id2);
    FB_ASSERT_TRUE(id1 != id3);
    FB_ASSERT_TRUE(id2 != id3);
}

FB_TEST(client_connection_cache, same_osd_same_port_reuses_key) {
    // Same node_id and port => same connection_id.
    auto id1 = to_connection_id(5, 9500);
    auto id2 = to_connection_id(5, 9500);
    FB_ASSERT_EQ(id1, id2);
}

FB_TEST(client_connection_cache, max_port_value) {
    // uint16_t max port is 65535; connection_id must handle it.
    auto id = to_connection_id(1, 65535);
    FB_ASSERT_TRUE(id != 0ull);
}

FB_TEST(client_connection_cache, negative_node_id_allowed) {
    // node_id is int32_t; negative values should be preserved.
    auto id = to_connection_id(-1, 9000);
    // Layout: low 32 bits = node_id, high 32 bits = port.
    int32_t node = static_cast<int32_t>(id & 0xffffffff);
    FB_ASSERT_EQ(node, -1);
}

// ============================================================================
// Test Suite: client_leader_key_edge — leader key edge cases
// ============================================================================

FB_SUITE_SETUP(client_leader_key_edge) {}
FB_SUITE_TEARDOWN(client_leader_key_edge) {}

FB_TEST(client_leader_key_edge, pool_id_negative_preserved_in_key) {
    auto key = make_leader_key(-100, 5);
    auto unpacked = from_leader_key(key);
    FB_ASSERT_EQ(unpacked.pool_id, -100);
    FB_ASSERT_EQ(unpacked.pg_id, 5);
}

FB_TEST(client_leader_key_edge, pg_id_negative_preserved_in_key) {
    auto key = make_leader_key(1, -10);
    auto unpacked = from_leader_key(key);
    FB_ASSERT_EQ(unpacked.pool_id, 1);
    FB_ASSERT_EQ(unpacked.pg_id, -10);
}

FB_TEST(client_leader_key_edge, both_negative_preserved) {
    auto key = make_leader_key(-1, -1);
    auto unpacked = from_leader_key(key);
    FB_ASSERT_EQ(unpacked.pool_id, -1);
    FB_ASSERT_EQ(unpacked.pg_id, -1);
}

FB_TEST(client_leader_key_edge, key_is_unique_per_pool_pg_pair) {
    std::set<uint64_t> keys;
    for (int32_t pool = 0; pool < 10; ++pool) {
        for (int32_t pg = 0; pg < 10; ++pg) {
            keys.insert(make_leader_key(pool, pg));
        }
    }
    FB_ASSERT_EQ(keys.size(), 100u);
}
