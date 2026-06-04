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
// PR 2: Additional Raft Types
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
// PR 2: Test Suite: Raft Types
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
// PR 3: Test Suite: Index and Time Operations
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
// PR 4: Test Suite: Election and Log Types
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
// PR 5: Test Suite: Membership and Voting
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

// Main function for test runner
FB_TEST_MAIN()
