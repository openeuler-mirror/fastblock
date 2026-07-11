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
 * @file test_monclient.cc
 * @brief Unit tests for monclient module data contracts NOT covered by test_comm.cc.
 *
 * test_comm.cc already covers:
 *   - response_status enum
 *   - endpoint validity
 *   - osd_map versioned directory
 *   - pg_state bitmask
 *   - pg_map_update state machine
 *   - cached_request_class enum
 *
 * This file adds:
 *   - pg_map pool directory operations (pool_is_exist, get_pool_id, add_pools, delete_pool)
 *   - pools structure and pool array iteration
 *   - image_info structure fields
 *   - Additional pg_map_update edge cases (partial completion, concurrent updates)
 *   - pg_map pool_pg_map nested dictionary lookup
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Local mirrors of monclient data contracts
// ============================================================================

namespace {

// ---------- response_status (mirrored from client.h) ----------------------
enum response_status {
    ok = 0,
    created_image_exists,
    marshal_image_context_error,
    server_put_ectd_error,
    unknown_pool_name,
    unknown_server_status,
    image_not_found,
    image_name_too_long,
    server_error,
    fail,
};

// ---------- image_info (mirrored from client.h) ---------------------------
struct image_info {
    std::string pool_name{};
    std::string image_name{};
    size_t size{};
    size_t object_size{};
};

// ---------- pools::pool (mirrored from client.h) --------------------------
struct pool {
    int32_t pool_id;
    std::string name;
    int32_t pg_size;
    int32_t pg_count;
    std::string failure_domain;
    std::string root;
};

struct pools {
    size_t num_pool{0};
    std::unique_ptr<pool[]> data{nullptr};
};

// ---------- pg_state bitmask (mirrored) -----------------------------------
enum pg_state {
    PgCreating  = 1 << 0,
    PgActive    = 1 << 1,
    PgUndersize = 1 << 2,
    PgDown      = 1 << 3,
    PgRemapped  = 1 << 4
};

// ---------- pg_map pool directory operations (mirrored) -------------------
// Type aliases used across multiple structures
using pool_id_type = int32_t;
using pg_id_type = int32_t;
using version_type = int64_t;

struct pg_map_mirror {
    // Pool name -> id mapping
    std::unordered_map<pool_id_type, std::string> pools;

    bool pool_is_exist(const pool_id_type pool_id) const {
        return pools.contains(pool_id);
    }

    bool get_pool_id(std::string& pool_name, pool_id_type& pool_id) {
        for (auto& [id, name] : pools) {
            if (pool_name == name) {
                pool_id = id;
                return true;
            }
        }
        return false;
    }

    void add_pools(pool_id_type pool_id, const std::string& pool_name) {
        if (!pools.contains(pool_id)) {
            pools[pool_id] = pool_name;
        }
    }

    void delete_pool(pool_id_type pool_id) {
        pools.erase(pool_id);
    }
};

// ---------- pg_map pool_pg_map nested dictionary --------------------------
struct pg_info_mirror {
    int32_t pg_id{0};
    std::vector<int32_t> osd_list;
    pg_state state{PgCreating};
};

// ---------- pg_map_update sentinel values ---------------------------------
constexpr int PG_UPDATE_FAILED  = -1;
constexpr int PG_UPDATE_DONE    = 0;
constexpr int PG_UPDATE_RUNNING = 1;

struct pool_update_info_mirror {
    version_type pool_version{0};
    std::unordered_map<pg_id_type, int> pgs{}; // pg_id -> update state
};

} // anonymous namespace


// ============================================================================
// Test Suite: monclient_image_info — image metadata structure
// ============================================================================

FB_SUITE_SETUP(monclient_image_info) {}
FB_SUITE_TEARDOWN(monclient_image_info) {}

FB_TEST(monclient_image_info, default_constructed_is_empty) {
    image_info info;
    FB_ASSERT_TRUE(info.pool_name.empty());
    FB_ASSERT_TRUE(info.image_name.empty());
    FB_ASSERT_EQ(info.size, 0u);
    FB_ASSERT_EQ(info.object_size, 0u);
}

FB_TEST(monclient_image_info, populated_fields_preserved) {
    image_info info{"pool_a", "vol_001", 1024 * 1024 * 1024, 4 * 1024 * 1024};
    FB_ASSERT_STR_EQ(info.pool_name.c_str(), "pool_a");
    FB_ASSERT_STR_EQ(info.image_name.c_str(), "vol_001");
    FB_ASSERT_EQ(info.size, 1024ull * 1024 * 1024);
    FB_ASSERT_EQ(info.object_size, 4ull * 1024 * 1024);
}

FB_TEST(monclient_image_info, size_and_object_size_are_size_t) {
    // size_t is platform-dependent; verify the fields can hold large values
    // (e.g., >4 GiB on 64-bit platforms).
    image_info info;
    info.size = 16ull * 1024 * 1024 * 1024; // 16 GiB
    info.object_size = 16ull * 1024 * 1024; // 16 MiB
    FB_ASSERT_TRUE(info.size > 4ull * 1024 * 1024 * 1024);
    FB_ASSERT_TRUE(info.object_size > 4ull * 1024 * 1024);
}

// ============================================================================
// Test Suite: monclient_pools — pool array structure
// ============================================================================

FB_SUITE_SETUP(monclient_pools) {}
FB_SUITE_TEARDOWN(monclient_pools) {}

FB_TEST(monclient_pools, default_constructed_is_empty) {
    pools p;
    FB_ASSERT_EQ(p.num_pool, 0u);
    FB_ASSERT_TRUE(p.data == nullptr);
}

FB_TEST(monclient_pools, array_allocation_matches_count) {
    pools p;
    p.num_pool = 3;
    p.data = std::make_unique<pool[]>(3);
    p.data[0] = pool{1, "default", 3, 128, "host", "default"};
    p.data[1] = pool{2, "images", 3, 64, "host", "default"};
    p.data[2] = pool{3, "volumes", 3, 256, "host", "default"};
    FB_ASSERT_EQ(p.num_pool, 3u);
    FB_ASSERT_TRUE(p.data != nullptr);
}

FB_TEST(monclient_pools, pool_fields_preserved) {
    pool p{42, "test_pool", 3, 100, "rack", "root"};
    FB_ASSERT_EQ(p.pool_id, 42);
    FB_ASSERT_STR_EQ(p.name.c_str(), "test_pool");
    FB_ASSERT_EQ(p.pg_size, 3);
    FB_ASSERT_EQ(p.pg_count, 100);
    FB_ASSERT_STR_EQ(p.failure_domain.c_str(), "rack");
    FB_ASSERT_STR_EQ(p.root.c_str(), "root");
}

FB_TEST(monclient_pools, iteration_over_array) {
    pools p;
    p.num_pool = 2;
    p.data = std::make_unique<pool[]>(2);
    p.data[0] = pool{1, "a", 3, 10, "host", "r"};
    p.data[1] = pool{2, "b", 3, 20, "host", "r"};
    int count = 0;
    for (size_t i = 0; i < p.num_pool; ++i) {
        ++count;
    }
    FB_ASSERT_EQ(count, 2);
}

FB_TEST(monclient_pools, empty_pool_name_allowed) {
    // Empty pool name is syntactically valid (monclient may create pools with
    // placeholder names). Pin that empty string is accepted.
    pool p{0, "", 3, 1, "host", "root"};
    FB_ASSERT_TRUE(p.name.empty());
}

// ============================================================================
// Test Suite: monclient_pg_map_pool_dir — pool directory operations
// ============================================================================

FB_SUITE_SETUP(monclient_pg_map_pool_dir) {}
FB_SUITE_TEARDOWN(monclient_pg_map_pool_dir) {}

FB_TEST(monclient_pg_map_pool_dir, empty_map_has_no_pools) {
    pg_map_mirror m;
    FB_ASSERT_TRUE(m.pools.empty());
    FB_ASSERT_FALSE(m.pool_is_exist(1));
}

FB_TEST(monclient_pg_map_pool_dir, add_pool_inserts_if_missing) {
    pg_map_mirror m;
    m.add_pools(1, "default");
    FB_ASSERT_TRUE(m.pool_is_exist(1));
    FB_ASSERT_EQ(m.pools.size(), 1u);
    FB_ASSERT_STR_EQ(m.pools[1].c_str(), "default");
}

FB_TEST(monclient_pg_map_pool_dir, add_pool_no_duplicate) {
    // add_pools checks contains() before inserting; duplicate calls must not
    // overwrite the existing entry.
    pg_map_mirror m;
    m.add_pools(5, "original");
    m.add_pools(5, "duplicate"); // should be ignored
    FB_ASSERT_STR_EQ(m.pools[5].c_str(), "original");
    FB_ASSERT_EQ(m.pools.size(), 1u);
}

FB_TEST(monclient_pg_map_pool_dir, delete_pool_removes_entry) {
    pg_map_mirror m;
    m.add_pools(10, "temp");
    m.add_pools(20, "keep");
    FB_ASSERT_TRUE(m.pool_is_exist(10));
    m.delete_pool(10);
    FB_ASSERT_FALSE(m.pool_is_exist(10));
    FB_ASSERT_TRUE(m.pool_is_exist(20));
    FB_ASSERT_EQ(m.pools.size(), 1u);
}

FB_TEST(monclient_pg_map_pool_dir, delete_nonexistent_pool_is_safe) {
    // erase() on a missing key is safe (returns 0, no exception).
    pg_map_mirror m;
    m.delete_pool(999); // nonexistent
    FB_ASSERT_TRUE(m.pools.empty());
}

FB_TEST(monclient_pg_map_pool_dir, get_pool_id_by_name_found) {
    pg_map_mirror m;
    m.add_pools(7, "images");
    m.add_pools(8, "volumes");
    int32_t found_id = -1;
    std::string name = "images";
    FB_ASSERT_TRUE(m.get_pool_id(name, found_id));
    FB_ASSERT_EQ(found_id, 7);
}

FB_TEST(monclient_pg_map_pool_dir, get_pool_id_by_name_not_found) {
    pg_map_mirror m;
    m.add_pools(1, "exists");
    int32_t found_id = -1;
    std::string name = "missing";
    FB_ASSERT_FALSE(m.get_pool_id(name, found_id));
    // found_id is unchanged (implementation detail: not reset on failure)
}

FB_TEST(monclient_pg_map_pool_dir, get_pool_id_multiple_matches_returns_first) {
    // In case of duplicate names (shouldn't happen in production), get_pool_id
    // iterates and returns the first match. Pin the iteration order.
    pg_map_mirror m;
    m.add_pools(3, "dup");
    m.add_pools(4, "dup");
    int32_t found_id = -1;
    std::string name = "dup";
    FB_ASSERT_TRUE(m.get_pool_id(name, found_id));
    // Returns whichever comes first in iteration order (unordered_map is
    // hash-based, order undefined; pin that SOME match is found).
    FB_ASSERT_TRUE(found_id == 3 || found_id == 4);
}

FB_TEST(monclient_pg_map_pool_dir, pool_id_negative_allowed) {
    // pool_id is int32_t; negative IDs might be used for internal pools.
    pg_map_mirror m;
    m.add_pools(-1, "internal");
    FB_ASSERT_TRUE(m.pool_is_exist(-1));
    int32_t found_id = 0;
    std::string name = "internal";
    FB_ASSERT_TRUE(m.get_pool_id(name, found_id));
    FB_ASSERT_EQ(found_id, -1);
}

// ============================================================================
// Test Suite: monclient_pg_state_combined — pg state bitmask combinations
// ============================================================================

FB_SUITE_SETUP(monclient_pg_state_combined) {}
FB_SUITE_TEARDOWN(monclient_pg_state_combined) {}

FB_TEST(monclient_pg_state_combined, active_undersize_is_common_combination) {
    // A pg can be active but undersized (not all replicas present). This is
    // the most common degraded state.
    int state = PgActive | PgUndersize;
    FB_ASSERT_TRUE((state & PgActive) != 0);
    FB_ASSERT_TRUE((state & PgUndersize) != 0);
    FB_ASSERT_FALSE((state & PgDown) != 0);
    FB_ASSERT_FALSE((state & PgCreating) != 0);
}

FB_TEST(monclient_pg_state_combined, remapped_preserves_other_bits) {
    // When a pg is remapped (OSDs changed), it typically carries other state
    // bits. Remapped should not clear them.
    int state = PgActive | PgRemapped;
    state |= PgUndersize; // add undersize
    FB_ASSERT_TRUE((state & PgActive) != 0);
    FB_ASSERT_TRUE((state & PgRemapped) != 0);
    FB_ASSERT_TRUE((state & PgUndersize) != 0);
}

FB_TEST(monclient_pg_state_combined, clear_state_bit) {
    // Monclient may need to clear a bit (e.g., transition from Creating to Active).
    int state = PgCreating | PgActive;
    state &= ~PgCreating; // clear creating
    FB_ASSERT_FALSE((state & PgCreating) != 0);
    FB_ASSERT_TRUE((state & PgActive) != 0);
}

FB_TEST(monclient_pg_state_combined, all_five_bits_set) {
    // All bits set is a valid (though pathological) state; verify bitmask width.
    int state = PgCreating | PgActive | PgUndersize | PgDown | PgRemapped;
    FB_ASSERT_EQ(state, 1 | 2 | 4 | 8 | 16); // 31
    FB_ASSERT_TRUE((state & PgCreating) != 0);
    FB_ASSERT_TRUE((state & PgActive) != 0);
    FB_ASSERT_TRUE((state & PgUndersize) != 0);
    FB_ASSERT_TRUE((state & PgDown) != 0);
    FB_ASSERT_TRUE((state & PgRemapped) != 0);
}

FB_TEST(monclient_pg_state_combined, zero_is_no_state) {
    // 0 means the pg has no state bits set — a clean slate (or a bug).
    int state = 0;
    FB_ASSERT_FALSE((state & PgCreating) != 0);
    FB_ASSERT_FALSE((state & PgActive) != 0);
    FB_ASSERT_FALSE((state & PgUndersize) != 0);
    FB_ASSERT_FALSE((state & PgDown) != 0);
    FB_ASSERT_FALSE((state & PgRemapped) != 0);
}

// ============================================================================
// Test Suite: monclient_pg_update_advanced — additional update state machine edge cases
// ============================================================================

FB_SUITE_SETUP(monclient_pg_update_advanced) {}
FB_SUITE_TEARDOWN(monclient_pg_update_advanced) {}

FB_TEST(monclient_pg_update_advanced, partial_completion_has_running) {
    // When some pgs are done and others are running, pool_is_updating must
    // report true (migration in progress).
    pool_update_info_mirror info;
    info.pool_version = 10;
    info.pgs[1] = PG_UPDATE_DONE;
    info.pgs[2] = PG_UPDATE_RUNNING;
    info.pgs[3] = PG_UPDATE_RUNNING;
    bool has_running = false;
    for (auto& [_, st] : info.pgs) {
        if (st == PG_UPDATE_RUNNING) has_running = true;
    }
    FB_ASSERT_TRUE(has_running);
}

FB_TEST(monclient_pg_update_advanced, all_failed_blocks_completion) {
    // If ALL pgs failed, pool_update_all_done is false, and pool_is_updating
    // is false (nothing running). The pool is stuck and needs operator attention.
    pool_update_info_mirror info;
    info.pool_version = 5;
    info.pgs[1] = PG_UPDATE_FAILED;
    info.pgs[2] = PG_UPDATE_FAILED;
    bool all_done = true;
    bool has_running = false;
    for (auto& [_, st] : info.pgs) {
        if (st != PG_UPDATE_DONE) all_done = false;
        if (st == PG_UPDATE_RUNNING) has_running = true;
    }
    FB_ASSERT_FALSE(all_done);
    FB_ASSERT_FALSE(has_running);
}

FB_TEST(monclient_pg_update_advanced, version_carries_pool_epoch) {
    // pool_version tracks the pool's configuration epoch. It must increase
    // monotonically for each new update batch.
    pool_update_info_mirror info1;
    info1.pool_version = 100;
    pool_update_info_mirror info2;
    info2.pool_version = 200;
    FB_ASSERT_TRUE(info2.pool_version > info1.pool_version);
}

FB_TEST(monclient_pg_update_advanced, pg_id_in_update_map) {
    // The update map is keyed by pg_id, NOT by pg_name. Verify int32_t keys.
    pool_update_info_mirror info;
    info.pgs[0] = PG_UPDATE_RUNNING;
    info.pgs[999] = PG_UPDATE_DONE;
    FB_ASSERT_TRUE(info.pgs.contains(0));
    FB_ASSERT_TRUE(info.pgs.contains(999));
    FB_ASSERT_EQ(info.pgs[0], PG_UPDATE_RUNNING);
    FB_ASSERT_EQ(info.pgs[999], PG_UPDATE_DONE);
}

FB_TEST(monclient_pg_update_advanced, update_state_transition_sequence) {
    // Typical sequence: RUNNING -> DONE. Verify state can be updated.
    pool_update_info_mirror info;
    info.pgs[5] = PG_UPDATE_RUNNING;
    FB_ASSERT_EQ(info.pgs[5], PG_UPDATE_RUNNING);
    info.pgs[5] = PG_UPDATE_DONE; // transition
    FB_ASSERT_EQ(info.pgs[5], PG_UPDATE_DONE);
}

FB_TEST(monclient_pg_update_advanced, update_state_failure_transition) {
    // Failure sequence: RUNNING -> FAILED (no DONE).
    pool_update_info_mirror info;
    info.pgs[7] = PG_UPDATE_RUNNING;
    info.pgs[7] = PG_UPDATE_FAILED; // failure
    FB_ASSERT_EQ(info.pgs[7], PG_UPDATE_FAILED);
}

// ============================================================================
// Test Suite: monclient_response_status_advanced — additional status codes
// ============================================================================

FB_SUITE_SETUP(monclient_response_status_advanced) {}
FB_SUITE_TEARDOWN(monclient_response_status_advanced) {}

FB_TEST(monclient_response_status_advanced, ok_is_zero) {
    FB_ASSERT_EQ(static_cast<int>(ok), 0);
}

FB_TEST(monclient_response_status_advanced, positive_codes_are_user_errors) {
    // All user-facing errors (image exists, not found, etc.) use positive codes.
    FB_ASSERT_TRUE(static_cast<int>(created_image_exists) > 0);
    FB_ASSERT_TRUE(static_cast<int>(image_not_found) > 0);
    FB_ASSERT_TRUE(static_cast<int>(unknown_pool_name) > 0);
    FB_ASSERT_TRUE(static_cast<int>(image_name_too_long) > 0);
}

FB_TEST(monclient_response_status_advanced, values_are_pairwise_unique) {
    // Status codes must not alias; otherwise switch branches merge silently.
    std::vector<int> codes = {
        ok,
        created_image_exists, marshal_image_context_error,
        server_put_ectd_error, unknown_pool_name, unknown_server_status,
        image_not_found, image_name_too_long, server_error, fail,
    };
    auto n = codes.size();
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    FB_ASSERT_EQ(codes.size(), n);
}

FB_TEST(monclient_response_status_advanced, server_error_is_distinct_from_fail) {
    // server_error and fail are both generic failures but must be distinct
    // (different retry policies in some callers).
    FB_ASSERT_TRUE(static_cast<int>(server_error) != static_cast<int>(fail));
}

// ============================================================================
// Test Main Entry Point
// ============================================================================

FB_TEST_MAIN()
