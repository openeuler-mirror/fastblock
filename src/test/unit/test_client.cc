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
