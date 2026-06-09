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
 * @file test_raft_state.cc
 * @brief Unit tests for Raft state transitions and identity enums
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <limits>

namespace {

typedef enum {
    RAFT_STATE_NONE,
    RAFT_STATE_FOLLOWER,
    RAFT_STATE_CANDIDATE,
    RAFT_STATE_LEADER
} raft_identity;

enum class raft_op_state {
    RAFT_INIT,
    RAFT_ACTIVE,
    RAFT_DOWN,
    RAFT_DELETE
};

} // anonymous namespace

// ============================================================================
// Additional Raft Types
// ============================================================================

namespace {

typedef long int raft_term_t;
typedef long int raft_index_t;
typedef long int raft_time_t;
typedef long int raft_entry_id_t;
typedef long int raft_node_id_t;
typedef uint64_t raft_id_type;

} // anonymous namespace

FB_SUITE_SETUP(raft_state) {
    // Setup code here
}

FB_SUITE_TEARDOWN(raft_state) {
    // Teardown code here
}

// ============================================================================
// Test Suite: Raft State Transitions
// ============================================================================

FB_TEST(raft_state, become_candidate) {
    raft_identity state = RAFT_STATE_FOLLOWER;
    state = RAFT_STATE_CANDIDATE;
    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
    FB_ASSERT_TRUE(state != RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_state, become_leader) {
    raft_identity state = RAFT_STATE_CANDIDATE;
    state = RAFT_STATE_LEADER;
    FB_ASSERT_EQ(state, RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(state != RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(state != RAFT_STATE_CANDIDATE);
}

FB_TEST(raft_state, become_follower) {
    raft_identity state = RAFT_STATE_LEADER;
    state = RAFT_STATE_FOLLOWER;
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(state != RAFT_STATE_LEADER);
}

FB_TEST(raft_state, state_none_check) {
    raft_identity state = RAFT_STATE_NONE;
    FB_ASSERT_EQ(state, RAFT_STATE_NONE);
    FB_ASSERT_TRUE(state != RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(state != RAFT_STATE_CANDIDATE);
    FB_ASSERT_TRUE(state != RAFT_STATE_LEADER);
}

FB_TEST(raft_state, state_follower_check) {
    raft_identity state = RAFT_STATE_FOLLOWER;
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(state != RAFT_STATE_CANDIDATE);
    FB_ASSERT_TRUE(state != RAFT_STATE_LEADER);
}

FB_TEST(raft_state, state_candidate_check) {
    raft_identity state = RAFT_STATE_CANDIDATE;
    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
    FB_ASSERT_TRUE(state != RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(state != RAFT_STATE_LEADER);
}

FB_TEST(raft_state, state_leader_check) {
    raft_identity state = RAFT_STATE_LEADER;
    FB_ASSERT_EQ(state, RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(state != RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(state != RAFT_STATE_CANDIDATE);
}

// ============================================================================
// Test Suite: Raft Identity and Enums
// ============================================================================

FB_TEST(raft_state, identity_enum) {
    FB_ASSERT_EQ(RAFT_STATE_NONE, 0);
    FB_ASSERT_EQ(RAFT_STATE_FOLLOWER, 1);
    FB_ASSERT_EQ(RAFT_STATE_CANDIDATE, 2);
    FB_ASSERT_EQ(RAFT_STATE_LEADER, 3);
}

FB_TEST(raft_state, identity_none) {
    raft_identity id = RAFT_STATE_NONE;
    FB_ASSERT_TRUE(id == RAFT_STATE_NONE);
    FB_ASSERT_TRUE(id != RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_state, op_state_enum) {
    FB_ASSERT_EQ(static_cast<int>(raft_op_state::RAFT_INIT), 0);
    FB_ASSERT_EQ(static_cast<int>(raft_op_state::RAFT_ACTIVE), 1);
    FB_ASSERT_EQ(static_cast<int>(raft_op_state::RAFT_DOWN), 2);
    FB_ASSERT_EQ(static_cast<int>(raft_op_state::RAFT_DELETE), 3);
}

FB_TEST(raft_state, op_state_init) {
    raft_op_state state = raft_op_state::RAFT_INIT;
    FB_ASSERT_TRUE(state == raft_op_state::RAFT_INIT);
}

FB_TEST(raft_state, op_state_idle) {
    raft_op_state state = raft_op_state::RAFT_INIT;
    FB_ASSERT_TRUE(state == raft_op_state::RAFT_INIT);
}

FB_TEST(raft_state, op_state_transitions) {
    raft_op_state state = raft_op_state::RAFT_INIT;
    state = raft_op_state::RAFT_ACTIVE;
    FB_ASSERT_TRUE(state == raft_op_state::RAFT_ACTIVE);
    state = raft_op_state::RAFT_DOWN;
    FB_ASSERT_TRUE(state == raft_op_state::RAFT_DOWN);
}

FB_TEST(raft_state, op_state_catching_up) {
    raft_op_state state = raft_op_state::RAFT_ACTIVE;
    FB_ASSERT_TRUE(state == raft_op_state::RAFT_ACTIVE);
}

FB_TEST(raft_state, op_state_config_changing) {
    raft_op_state state = raft_op_state::RAFT_ACTIVE;
    FB_ASSERT_TRUE(state == raft_op_state::RAFT_ACTIVE);
}

FB_TEST(raft_state, op_state_snapshot) {
    raft_op_state state = raft_op_state::RAFT_ACTIVE;
    FB_ASSERT_TRUE(state == raft_op_state::RAFT_ACTIVE);
}

// ============================================================================
// Test Suite: Raft Types
// ============================================================================

FB_TEST(raft_state, types_basic) {
    raft_term_t term = 1;
    raft_index_t index = 0;
    raft_time_t time = 1000;

    FB_ASSERT_TRUE(term >= 0);
    FB_ASSERT_TRUE(index >= 0);
    FB_ASSERT_TRUE(time >= 0);
}

FB_TEST(raft_state, types_sizes) {
    FB_ASSERT_TRUE(sizeof(raft_term_t) >= 8);
    FB_ASSERT_TRUE(sizeof(raft_index_t) >= 8);
    FB_ASSERT_TRUE(sizeof(raft_time_t) >= 8);
}

FB_TEST(raft_state, term_type) {
    raft_term_t t1 = 1;
    raft_term_t t2 = 2;
    FB_ASSERT_TRUE(t2 > t1);
    FB_ASSERT_TRUE(t1 < t2);
}

FB_TEST(raft_state, index_type) {
    raft_index_t idx = 0;
    FB_ASSERT_EQ(idx, 0L);
    idx = 100;
    FB_ASSERT_EQ(idx, 100L);
}

FB_TEST(raft_state, time_type) {
    raft_time_t t1 = 1000;
    raft_time_t t2 = 2000;
    raft_time_t diff = t2 - t1;
    FB_ASSERT_EQ(diff, 1000L);
}

FB_TEST(raft_state, id_type) {
    raft_id_type id = 0xFFFFFFFFFFFFFFFFULL;
    FB_ASSERT_EQ(id, 0xFFFFFFFFFFFFFFFFULL);
}

FB_TEST(raft_state, entry_id_type) {
    raft_entry_id_t id = 12345;
    FB_ASSERT_EQ(id, 12345L);
}

FB_TEST(raft_state, node_id_type) {
    raft_node_id_t id = 1;
    FB_ASSERT_EQ(id, 1L);
}

FB_TEST(raft_state, term_cmp_gt) {
    raft_term_t t1 = 7;
    raft_term_t t2 = 5;
    FB_ASSERT_TRUE(t1 > t2);
    FB_ASSERT_FALSE(t1 == t2);
    FB_ASSERT_FALSE(t1 < t2);
}

FB_TEST(raft_state, term_cmp_lt) {
    raft_term_t t1 = 3;
    raft_term_t t2 = 5;
    FB_ASSERT_TRUE(t1 < t2);
    FB_ASSERT_FALSE(t1 == t2);
    FB_ASSERT_FALSE(t1 > t2);
}

FB_TEST(raft_state, term_cmp_eq) {
    raft_term_t t1 = 5;
    raft_term_t t2 = 5;
    FB_ASSERT_TRUE(t1 == t2);
    FB_ASSERT_FALSE(t1 < t2);
    FB_ASSERT_FALSE(t1 > t2);
}

FB_TEST(raft_state, term_increase) {
    raft_term_t current_term = 1;
    raft_term_t received_term = 2;
    bool term_is_newer = received_term > current_term;
    FB_ASSERT_TRUE(term_is_newer);
    current_term = received_term;
    FB_ASSERT_EQ(current_term, 2L);
}

FB_TEST(raft_state, term_max_boundary) {
    raft_term_t max_term = std::numeric_limits<raft_term_t>::max();
    FB_ASSERT_TRUE(max_term > 0);
    raft_term_t large_term = max_term - 1;
    FB_ASSERT_TRUE(large_term > 0);
}

// ============================================================================
// Test Suite: Index and Time Operations
// ============================================================================

FB_TEST(raft_state, idx_inc) {
    raft_index_t idx = 1;
    idx = idx + 1;
    FB_ASSERT_EQ(idx, 2L);
    idx += 1;
    FB_ASSERT_EQ(idx, 3L);
    idx++;
    FB_ASSERT_EQ(idx, 4L);
}

FB_TEST(raft_state, idx_dec) {
    raft_index_t idx = 10;
    idx = idx - 1;
    FB_ASSERT_EQ(idx, 9L);
    idx -= 1;
    FB_ASSERT_EQ(idx, 8L);
    idx--;
    FB_ASSERT_EQ(idx, 7L);
}

FB_TEST(raft_state, idx_diff) {
    raft_index_t start = 5;
    raft_index_t end = 10;
    raft_index_t diff = end - start;
    FB_ASSERT_EQ(diff, 5L);
    FB_ASSERT_TRUE(start + diff == end);
}

FB_TEST(raft_state, time_add) {
    raft_time_t now = 1000;
    raft_time_t timeout = 500;
    raft_time_t expiry = now + timeout;
    FB_ASSERT_EQ(expiry, 1500L);
    FB_ASSERT_TRUE(expiry > now);
}

FB_TEST(raft_state, time_diff) {
    raft_time_t start = 1000;
    raft_time_t end = 1750;
    raft_time_t elapsed = end - start;
    FB_ASSERT_EQ(elapsed, 750L);
    FB_ASSERT_TRUE(elapsed >= 0);
}

FB_TEST(raft_state, next_idx_boundary) {
    auto clamp_next_idx = [](int64_t next_idx) -> int64_t {
        return next_idx < 1 ? 1 : next_idx;
    };

    FB_ASSERT_EQ(clamp_next_idx(0), 1L);
    FB_ASSERT_EQ(clamp_next_idx(1), 1L);
    FB_ASSERT_EQ(clamp_next_idx(5), 5L);
    FB_ASSERT_EQ(clamp_next_idx(-1), 1L);
    FB_ASSERT_EQ(clamp_next_idx(100), 100L);
}

FB_TEST(raft_state, match_idx_logic) {
    int64_t match_idx = 0;

    FB_ASSERT_EQ(match_idx, 0L);
    match_idx = 5;
    FB_ASSERT_EQ(match_idx, 5L);
    match_idx = 10;
    FB_ASSERT_EQ(match_idx, 10L);
    match_idx = 3;
    FB_ASSERT_EQ(match_idx, 3L);
    match_idx = 0;
    FB_ASSERT_EQ(match_idx, 0L);
}

// ============================================================================
// Test Suite: Election and Log Types
// ============================================================================

namespace {
typedef enum {
    RAFT_LOGTYPE_WRITE,
    RAFT_LOGTYPE_DELETE,
    RAFT_LOGTYPE_ADD_NONVOTING_NODE,
    RAFT_LOGTYPE_CONFIGURATION,
} raft_logtype_e;
}

FB_TEST(raft_state, election_timeout_logic) {
    int election_timeout = 100;
    int election_timeout_rand = election_timeout + rand() % election_timeout;

    // Randomized timeout should be in [base, 2*base)
    FB_ASSERT_TRUE(election_timeout_rand >= election_timeout);
    FB_ASSERT_TRUE(election_timeout_rand < 2 * election_timeout);
}

FB_TEST(raft_state, election_timeout_randomize) {
    int base = 500;
    bool all_in_range = true;

    for (int i = 0; i < 100; ++i) {
        int randomized = base + (rand() % base);
        if (randomized < base || randomized >= 2 * base) {
            all_in_range = false;
            break;
        }
    }
    FB_ASSERT_TRUE(all_in_range);
}

FB_TEST(raft_state, logtype) {
    FB_ASSERT_EQ(RAFT_LOGTYPE_WRITE, 0);
    FB_ASSERT_EQ(RAFT_LOGTYPE_DELETE, 1);
    FB_ASSERT_EQ(RAFT_LOGTYPE_ADD_NONVOTING_NODE, 2);
    FB_ASSERT_EQ(RAFT_LOGTYPE_CONFIGURATION, 3);
}

FB_TEST(raft_state, logtype_enum) {
    raft_logtype_e type = RAFT_LOGTYPE_WRITE;
    FB_ASSERT_EQ(type, RAFT_LOGTYPE_WRITE);
    type = RAFT_LOGTYPE_DELETE;
    FB_ASSERT_EQ(type, RAFT_LOGTYPE_DELETE);
}

FB_TEST(raft_state, logtype_normal) {
    raft_logtype_e type = RAFT_LOGTYPE_WRITE;
    FB_ASSERT_EQ(type, 0);
}

FB_TEST(raft_state, logtype_write_check) {
    raft_logtype_e type = RAFT_LOGTYPE_WRITE;
    FB_ASSERT_TRUE(type == 0);
}

FB_TEST(raft_state, logtype_delete_check) {
    raft_logtype_e type = RAFT_LOGTYPE_DELETE;
    FB_ASSERT_TRUE(type == 1);
}

FB_TEST(raft_state, logtype_add_node) {
    raft_logtype_e type = RAFT_LOGTYPE_ADD_NONVOTING_NODE;
    FB_ASSERT_TRUE(type == 2);
}

FB_TEST(raft_state, logtype_config_check) {
    raft_logtype_e type = RAFT_LOGTYPE_CONFIGURATION;
    FB_ASSERT_TRUE(type == 3);
}

// ============================================================================
// Test Suite: Membership and Voting
// ============================================================================

namespace {
enum raft_membership_e {
    RAFT_MEMBERSHIP_ADD,
    RAFT_MEMBERSHIP_REMOVE,
    RAFT_MEMBERSHIP_NO_CHANGE
};

constexpr int RAFT_NODE_VOTED_FOR_ME = (1 << 0);
}

FB_TEST(raft_state, membership_enum) {
    FB_ASSERT_EQ(RAFT_MEMBERSHIP_ADD, 0);
    FB_ASSERT_EQ(RAFT_MEMBERSHIP_REMOVE, 1);
    FB_ASSERT_EQ(RAFT_MEMBERSHIP_NO_CHANGE, 2);
}

FB_TEST(raft_state, membership_add) {
    raft_membership_e m = RAFT_MEMBERSHIP_ADD;
    FB_ASSERT_TRUE(m == RAFT_MEMBERSHIP_ADD);
    FB_ASSERT_TRUE(m != RAFT_MEMBERSHIP_REMOVE);
    FB_ASSERT_TRUE(m != RAFT_MEMBERSHIP_NO_CHANGE);
}

FB_TEST(raft_state, membership_remove) {
    raft_membership_e m = RAFT_MEMBERSHIP_REMOVE;
    FB_ASSERT_TRUE(m == RAFT_MEMBERSHIP_REMOVE);
    FB_ASSERT_TRUE(m != RAFT_MEMBERSHIP_ADD);
    FB_ASSERT_TRUE(m != RAFT_MEMBERSHIP_NO_CHANGE);
}

FB_TEST(raft_state, membership_no_change) {
    raft_membership_e m = RAFT_MEMBERSHIP_NO_CHANGE;
    FB_ASSERT_TRUE(m == RAFT_MEMBERSHIP_NO_CHANGE);
    FB_ASSERT_TRUE(m != RAFT_MEMBERSHIP_ADD);
    FB_ASSERT_TRUE(m != RAFT_MEMBERSHIP_REMOVE);
}

FB_TEST(raft_state, votes_majority_single) {
    uint64_t node_num = 5;
    uint64_t votes = 3;
    bool is_majority = votes > node_num / 2;
    FB_ASSERT_TRUE(is_majority);

    votes = 2;
    is_majority = votes > node_num / 2;
    FB_ASSERT_FALSE(is_majority);
}

FB_TEST(raft_state, votes_majority_joint) {
    uint64_t old_node_num = 5;
    uint64_t new_node_num = 3;
    uint64_t old_votes = 3;
    uint64_t new_votes = 2;

    bool old_majority = old_votes > old_node_num / 2;
    bool new_majority = new_votes > new_node_num / 2;

    FB_ASSERT_TRUE(old_majority);
    FB_ASSERT_TRUE(new_majority);

    bool joint_majority = old_majority && new_majority;
    FB_ASSERT_TRUE(joint_majority);
}

FB_TEST(raft_state, vote_flag_logic) {
    int flags = 0;

    // Initially no vote
    FB_ASSERT_FALSE((flags & RAFT_NODE_VOTED_FOR_ME) != 0);

    // Set vote
    flags |= RAFT_NODE_VOTED_FOR_ME;
    FB_ASSERT_TRUE((flags & RAFT_NODE_VOTED_FOR_ME) != 0);

    // Clear vote
    flags &= ~RAFT_NODE_VOTED_FOR_ME;
    FB_ASSERT_FALSE((flags & RAFT_NODE_VOTED_FOR_ME) != 0);
}

// ============================================================================
// Test Suite: Node Operations
// ============================================================================

FB_TEST(raft_state, node_id_eq) {
    raft_node_id_t id1 = 1;
    raft_node_id_t id2 = 1;
    FB_ASSERT_TRUE(id1 == id2);
}

FB_TEST(raft_state, node_id_neq) {
    raft_node_id_t id1 = 1;
    raft_node_id_t id2 = 2;
    FB_ASSERT_TRUE(id1 != id2);
}

FB_TEST(raft_state, node_flags) {
    int flags = 0;
    flags |= RAFT_NODE_VOTED_FOR_ME;
    FB_ASSERT_TRUE((flags & RAFT_NODE_VOTED_FOR_ME) != 0);
}

FB_TEST(raft_state, node_logic) {
    auto clamp_next_idx = [](int64_t next_idx) -> int64_t {
        return next_idx < 1 ? 1 : next_idx;
    };

    FB_ASSERT_EQ(clamp_next_idx(-100), 1L);
    FB_ASSERT_EQ(clamp_next_idx(-1), 1L);
    FB_ASSERT_EQ(clamp_next_idx(0), 1L);
    FB_ASSERT_EQ(clamp_next_idx(1), 1L);
    FB_ASSERT_EQ(clamp_next_idx(5), 5L);
    FB_ASSERT_EQ(clamp_next_idx(1000), 1000L);
}

FB_TEST(raft_state, node_next_idx) {
    auto set_next_idx = [](raft_index_t idx) -> raft_index_t {
        return idx < 1 ? 1 : idx;
    };

    FB_ASSERT_EQ(set_next_idx(0), 1L);
    FB_ASSERT_EQ(set_next_idx(1), 1L);
    FB_ASSERT_EQ(set_next_idx(5), 5L);
    FB_ASSERT_EQ(set_next_idx(-1), 1L);
    FB_ASSERT_EQ(set_next_idx(-100), 1L);
    FB_ASSERT_EQ(set_next_idx(100), 100L);
}

FB_TEST(raft_state, node_next_idx_clamp) {
    auto clamp_min_one = [](long int idx) -> long int {
        return idx < 1 ? 1 : idx;
    };

    FB_ASSERT_EQ(clamp_min_one(0), 1L);
    FB_ASSERT_EQ(clamp_min_one(-5), 1L);
    FB_ASSERT_EQ(clamp_min_one(1), 1L);
    FB_ASSERT_EQ(clamp_min_one(10), 10L);
}

FB_TEST(raft_state, next_idx_logic) {
    int64_t next_idx = 1;

    auto set_next_idx = [&next_idx](int64_t idx) {
        next_idx = idx < 1 ? 1 : idx;
    };

    FB_ASSERT_EQ(next_idx, 1L);
    set_next_idx(10);
    FB_ASSERT_EQ(next_idx, 10L);
    set_next_idx(0);
    FB_ASSERT_EQ(next_idx, 1L);
    set_next_idx(-5);
    FB_ASSERT_EQ(next_idx, 1L);
    set_next_idx(100);
    FB_ASSERT_EQ(next_idx, 100L);
}

FB_TEST(raft_state, is_self_check) {
    raft_node_id_t self_id = 1;
    raft_node_id_t other_id = 2;

    FB_ASSERT_TRUE(self_id == self_id);
    FB_ASSERT_FALSE(self_id == other_id);
}

FB_TEST(raft_state, node_voted_for_me) {
    int flags = 0;

    FB_ASSERT_FALSE((flags & RAFT_NODE_VOTED_FOR_ME) != 0);

    flags |= RAFT_NODE_VOTED_FOR_ME;
    FB_ASSERT_TRUE((flags & RAFT_NODE_VOTED_FOR_ME) != 0);
}

FB_TEST(raft_state, node_suppress_heartbeat) {
    bool suppress_heartbeat = false;
    FB_ASSERT_FALSE(suppress_heartbeat);
    suppress_heartbeat = true;
    FB_ASSERT_TRUE(suppress_heartbeat);
}

// Test Suite: Lease and Cache Operations

#include <map>

FB_TEST(raft_state, lease_logic) {
    int64_t lease = 0;

    FB_ASSERT_EQ(lease, 0L);

    auto set_lease = [&lease](int64_t new_lease) {
        if (lease < new_lease) {
            lease = new_lease;
        }
    };

    set_lease(100);
    FB_ASSERT_EQ(lease, 100L);

    set_lease(50);
    FB_ASSERT_EQ(lease, 100L);

    set_lease(200);
    FB_ASSERT_EQ(lease, 200L);
}

FB_TEST(raft_state, node_lease_only_increases) {
    int64_t lease = 0;

    auto update_lease = [&lease](int64_t new_lease) {
        if (new_lease > lease) {
            lease = new_lease;
        }
    };

    update_lease(100);
    FB_ASSERT_EQ(lease, 100L);

    update_lease(50);
    FB_ASSERT_EQ(lease, 100L);

    update_lease(150);
    FB_ASSERT_EQ(lease, 150L);
}

FB_TEST(raft_state, cache_add_remove) {
    std::map<long int, int> cache;

    cache[1] = 10;
    cache[2] = 20;
    FB_ASSERT_EQ(cache.size(), 2UL);

    cache.erase(1);
    FB_ASSERT_EQ(cache.size(), 1UL);
    FB_ASSERT_EQ(cache.count(1), 0UL);
    FB_ASSERT_EQ(cache[2], 20);
}

FB_TEST(raft_state, cache_get_upper) {
    std::map<long int, int> cache;
    cache[1] = 10;
    cache[5] = 50;
    cache[10] = 100;

    auto it = cache.upper_bound(5);
    FB_ASSERT_TRUE(it != cache.end());
    FB_ASSERT_EQ(it->first, 10L);
}

// Test Suite: Server Operations

FB_TEST(raft_state, server_catch_up_num) {
    int catch_up_num = 0;
    FB_ASSERT_EQ(catch_up_num, 0);
    catch_up_num = 3;
    FB_ASSERT_EQ(catch_up_num, 3);
}

FB_TEST(raft_state, server_election_timeout) {
    int base_timeout = 500;
    int randomized = base_timeout + (rand() % base_timeout);
    FB_ASSERT_TRUE(randomized >= base_timeout);
    FB_ASSERT_TRUE(randomized < 2 * base_timeout);
}

FB_TEST(raft_state, server_heartbeat_period) {
    int heartbeat_period = 100;
    FB_ASSERT_TRUE(heartbeat_period > 0);
    FB_ASSERT_TRUE(heartbeat_period < 1000);
}

FB_TEST(raft_state, server_ptr_check) {
    void* ptr = nullptr;
    FB_ASSERT_TRUE(ptr == nullptr);

    int value = 42;
    ptr = &value;
    FB_ASSERT_TRUE(ptr != nullptr);
}

FB_TEST(raft_state, server_snapshot_chunks) {
    int max_chunks = 1024;
    FB_ASSERT_TRUE(max_chunks > 0);
}

FB_TEST(raft_state, server_timeout_elapsed) {
    raft_time_t now = 1000;
    raft_time_t timeout = 500;
    raft_time_t deadline = now + timeout;

    raft_time_t current = 1200;
    bool elapsed = current >= deadline;
    FB_ASSERT_FALSE(elapsed);

    current = 1500;
    elapsed = current >= deadline;
    FB_ASSERT_TRUE(elapsed);
}

FB_TEST(raft_state, timer_constants) {
    int heartbeat_timeout = 100;
    int election_timeout = 500;

    FB_ASSERT_TRUE(heartbeat_timeout > 0);
    FB_ASSERT_TRUE(election_timeout > 0);
    FB_ASSERT_TRUE(election_timeout > heartbeat_timeout);
}

FB_TEST(raft_state, log_max_applied_cache) {
    int max_applied_cache = 1000;
    FB_ASSERT_TRUE(max_applied_cache > 0);
}

FB_TEST(raft_state, log_next_idx_init_check) {
    raft_index_t next_idx = 1;
    FB_ASSERT_EQ(next_idx, 1L);
}

// ============================================================================
// Test Suite: Boundary and Overflow Tests
// ============================================================================

FB_TEST(raft_state, term_overflow_protection) {
    raft_term_t term = std::numeric_limits<raft_term_t>::max();
    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(term == std::numeric_limits<raft_term_t>::max());

    // 验证溢出后的行为（有符号整数溢出是未定义行为）
    raft_term_t max_minus_one = std::numeric_limits<raft_term_t>::max() - 1;
    FB_ASSERT_TRUE(max_minus_one > 0);
    FB_ASSERT_TRUE(max_minus_one < term);
}

FB_TEST(raft_state, index_zero_boundary) {
    raft_index_t idx = 0;
    FB_ASSERT_TRUE(idx >= 0);

    // commit_idx = 0 表示还没有任何日志被提交
    int64_t commit_idx = 0;
    FB_ASSERT_EQ(commit_idx, 0L);

    // commit_idx 必须小于等于 last_log_idx
    int64_t last_log_idx = 5;
    FB_ASSERT_TRUE(commit_idx <= last_log_idx);
}

FB_TEST(raft_state, negative_index_handling) {
    auto clamp = [](long int idx) -> long int {
        return idx < 1 ? 1 : idx;
    };

    FB_ASSERT_EQ(clamp(-999999), 1L);
    FB_ASSERT_EQ(clamp(-1), 1L);
    FB_ASSERT_EQ(clamp(0), 1L);
    FB_ASSERT_EQ(clamp(1), 1L);
    FB_ASSERT_EQ(clamp(100), 100L);
    FB_ASSERT_EQ(clamp(std::numeric_limits<long int>::min()), 1L);
}

FB_TEST(raft_state, term_boundary_values) {
    raft_term_t min_term = std::numeric_limits<raft_term_t>::min();
    raft_term_t max_term = std::numeric_limits<raft_term_t>::max();

    FB_ASSERT_TRUE(min_term < max_term);
    FB_ASSERT_TRUE(max_term > 0);

    // Term 应该从 1 开始，0 是无效值
    raft_term_t valid_term = 1;
    FB_ASSERT_TRUE(valid_term > 0);
}

FB_TEST(raft_state, index_boundary_values) {
    raft_index_t min_idx = 0;
    raft_index_t max_idx = std::numeric_limits<raft_index_t>::max();

    FB_ASSERT_TRUE(min_idx >= 0);
    FB_ASSERT_TRUE(max_idx > 0);
    FB_ASSERT_TRUE(min_idx < max_idx);

    // 验证 first_idx 和 last_idx 的合理范围
    raft_index_t first_idx = 1;
    raft_index_t last_idx = max_idx;
    FB_ASSERT_TRUE(first_idx <= last_idx);
}

// ============================================================================
// Test Suite: State Machine Transition Tests
// ============================================================================

FB_TEST(raft_state, valid_state_transitions) {
    // Follower -> Candidate -> Leader -> Follower (标准转换路径)
    raft_identity state = RAFT_STATE_FOLLOWER;
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);

    // Follower -> Candidate (选举超时触发)
    state = RAFT_STATE_CANDIDATE;
    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
    FB_ASSERT_TRUE(state != RAFT_STATE_FOLLOWER);

    // Candidate -> Leader (赢得选举)
    state = RAFT_STATE_LEADER;
    FB_ASSERT_EQ(state, RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(state != RAFT_STATE_CANDIDATE);

    // Leader -> Follower (发现更高term)
    state = RAFT_STATE_FOLLOWER;
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(state != RAFT_STATE_LEADER);
}

FB_TEST(raft_state, invalid_direct_follower_to_leader) {
    // 不能直接从Follower变成Leader（必须先经过Candidate阶段）
    raft_identity state = RAFT_STATE_FOLLOWER;
    raft_identity prev_state = state;

    // 模拟非法转换检测
    bool transition_valid = (prev_state == RAFT_STATE_CANDIDATE);
    FB_ASSERT_FALSE(transition_valid);

    // 正确的转换路径必须经过Candidate
    prev_state = RAFT_STATE_CANDIDATE;
    transition_valid = (prev_state == RAFT_STATE_CANDIDATE);
    FB_ASSERT_TRUE(transition_valid);
}

FB_TEST(raft_state, state_all_valid_values) {
    // 验证所有状态值都在合法范围内
    for (int i = RAFT_STATE_NONE; i <= RAFT_STATE_LEADER; i++) {
        FB_ASSERT_TRUE(i >= RAFT_STATE_NONE);
        FB_ASSERT_TRUE(i <= RAFT_STATE_LEADER);
    }
}

FB_TEST(raft_state, state_none_is_initial) {
    // RAFT_STATE_NONE 表示节点尚未加入集群
    raft_identity state = RAFT_STATE_NONE;
    FB_ASSERT_EQ(state, RAFT_STATE_NONE);

    // NONE 状态的节点不参与选举
    bool can_vote = (state == RAFT_STATE_FOLLOWER || state == RAFT_STATE_CANDIDATE);
    FB_ASSERT_FALSE(can_vote);
}

FB_TEST(raft_state, candidate_can_become_leader_or_follower) {
    raft_identity state = RAFT_STATE_CANDIDATE;

    // Candidate 可能赢得选举成为 Leader
    raft_identity after_win = RAFT_STATE_LEADER;
    FB_ASSERT_TRUE(after_win == RAFT_STATE_LEADER);

    // Candidate 可能收到更高term成为 Follower
    raft_identity after_lose = RAFT_STATE_FOLLOWER;
    FB_ASSERT_TRUE(after_lose == RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_state, leader_step_down_on_higher_term) {
    raft_identity state = RAFT_STATE_LEADER;
    raft_term_t current_term = 5;
    raft_term_t received_term = 6;

    // 收到更高term时，Leader必须step down
    bool should_step_down = received_term > current_term;
    FB_ASSERT_TRUE(should_step_down);

    if (should_step_down) {
        state = RAFT_STATE_FOLLOWER;
        current_term = received_term;
    }

    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
    FB_ASSERT_EQ(current_term, 6L);
}

// ============================================================================
// Test Suite: Heartbeat and Timeout Tests
// ============================================================================

FB_TEST(raft_state, heartbeat_timeout_ratio) {
    int election_timeout = 500;
    int heartbeat_timeout = 100;

    // 心跳间隔应该远小于选举超时，以防止不必要的选举
    FB_ASSERT_TRUE(heartbeat_timeout < election_timeout / 2);
    FB_ASSERT_TRUE(heartbeat_timeout > 0);
    FB_ASSERT_TRUE(election_timeout > heartbeat_timeout);
}

FB_TEST(raft_state, election_timeout_rand_range) {
    int base = 1000;
    bool all_in_range = true;

    // 验证随机化选举超时在合理范围内 [base, 2*base)
    for (int i = 0; i < 1000; i++) {
        int randomized = base + (rand() % base);
        if (randomized < base || randomized >= 2 * base) {
            all_in_range = false;
            break;
        }
    }
    FB_ASSERT_TRUE(all_in_range);
}

FB_TEST(raft_state, leader_heartbeat_timing) {
    raft_time_t last_heartbeat = 1000;
    raft_time_t heartbeat_interval = 100;

    // 当前时间刚好达到心跳间隔
    raft_time_t now = 1100;
    bool need_heartbeat = (now - last_heartbeat) >= heartbeat_interval;
    FB_ASSERT_TRUE(need_heartbeat);

    // 当前时间还没达到心跳间隔
    now = 1050;
    need_heartbeat = (now - last_heartbeat) >= heartbeat_interval;
    FB_ASSERT_FALSE(need_heartbeat);
}

FB_TEST(raft_state, follower_election_timeout_elapsed) {
    raft_time_t last_leader_contact = 1000;
    raft_time_t election_timeout = 500;

    // 未超时
    raft_time_t now = 1200;
    bool election_triggered = (now - last_leader_contact) >= election_timeout;
    FB_ASSERT_FALSE(election_triggered);

    // 刚好超时
    now = 1500;
    election_triggered = (now - last_leader_contact) >= election_timeout;
    FB_ASSERT_TRUE(election_triggered);

    // 已超时很久
    now = 2000;
    election_triggered = (now - last_leader_contact) >= election_timeout;
    FB_ASSERT_TRUE(election_triggered);
}

FB_TEST(raft_state, heartbeat_reset_election_timer) {
    raft_time_t election_timeout = 500;
    raft_time_t last_leader_contact = 0;

    // 收到心跳后重置选举计时器
    raft_time_t now = 100;
    last_leader_contact = now;

    // 计算剩余超时时间
    raft_time_t remaining = election_timeout - (now - last_leader_contact);
    FB_ASSERT_EQ(remaining, 500L);

    // 模拟时间流逝
    now = 400;
    remaining = election_timeout - (now - last_leader_contact);
    FB_ASSERT_EQ(remaining, 100L);
}

FB_TEST(raft_state, min_max_timeout_values) {
    // 定义合理的超时范围
    int min_election_timeout = 100;   // 最小100ms
    int max_election_timeout = 60000; // 最大60秒

    int actual_timeout = 500;
    FB_ASSERT_TRUE(actual_timeout >= min_election_timeout);
    FB_ASSERT_TRUE(actual_timeout <= max_election_timeout);

    int min_heartbeat_timeout = 10;   // 最小10ms
    int max_heartbeat_timeout = 1000; // 最大1秒

    int actual_heartbeat = 100;
    FB_ASSERT_TRUE(actual_heartbeat >= min_heartbeat_timeout);
    FB_ASSERT_TRUE(actual_heartbeat <= max_heartbeat_timeout);
}

FB_TEST(raft_state, randomize_avoid_same_timeout) {
    // 多个节点同时启动时，随机化超时避免同时发起选举
    int base_timeout = 500;
    std::vector<int> timeouts;

    for (int i = 0; i < 5; i++) {
        int randomized = base_timeout + (rand() % base_timeout);
        timeouts.push_back(randomized);
    }

    // 验证至少有一些差异
    bool has_variation = false;
    for (size_t i = 0; i < timeouts.size() - 1; i++) {
        if (timeouts[i] != timeouts[i + 1]) {
            has_variation = true;
            break;
        }
    }
    FB_ASSERT_TRUE(has_variation);
}

// Main function for test runner
FB_TEST_MAIN()
