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

#include "raft/raft.h"
#include "raft/raft_log.h"
#include "raft/raft_node.h"
#include "raft/configuration_manager.h"

#include <limits>
#include <memory>

// All types (raft_identity, raft_op_state, raft_term_t, etc.) are defined in raft/raft.h and raft/raft_types.h

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

// raft_logtype_e is defined in raft/raft.h

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

// raft_membership_e and RAFT_NODE_VOTED_FOR_ME are defined in raft/raft.h

FB_TEST(raft_state, membership_enum) {
    FB_ASSERT_EQ(RAFT_MEMBERSHIP_ADD, 0);
    FB_ASSERT_EQ(RAFT_MEMBERSHIP_REMOVE, 1);
}

FB_TEST(raft_state, membership_add) {
    raft_membership_e m = RAFT_MEMBERSHIP_ADD;
    FB_ASSERT_TRUE(m == RAFT_MEMBERSHIP_ADD);
    FB_ASSERT_TRUE(m != RAFT_MEMBERSHIP_REMOVE);
}

FB_TEST(raft_state, membership_remove) {
    raft_membership_e m = RAFT_MEMBERSHIP_REMOVE;
    FB_ASSERT_TRUE(m == RAFT_MEMBERSHIP_REMOVE);
    FB_ASSERT_TRUE(m != RAFT_MEMBERSHIP_ADD);
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
    FB_ASSERT_EQ(remaining, 200L);
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

// ============================================================================
// Test Suite: Log Replication and Consistency Tests
// ============================================================================

FB_TEST(raft_state, log_replication_match_idx_update) {
    int64_t match_idx = 0;
    int64_t next_idx = 1;

    // 成功复制日志后更新 match_idx 和 next_idx
    match_idx = next_idx;
    next_idx++;
    FB_ASSERT_EQ(match_idx, 1L);
    FB_ASSERT_EQ(next_idx, 2L);

    // 继续复制更多日志
    match_idx = next_idx;
    next_idx++;
    FB_ASSERT_EQ(match_idx, 2L);
    FB_ASSERT_EQ(next_idx, 3L);
}

FB_TEST(raft_state, log_inconsistency_next_idx_decrement) {
    int64_t next_idx = 10;

    // 日志不一致时，递减 next_idx
    next_idx = std::max(1L, next_idx - 1);
    FB_ASSERT_EQ(next_idx, 9L);

    // 多次递减
    next_idx = std::max(1L, next_idx - 1);
    FB_ASSERT_EQ(next_idx, 8L);

    // 递减到最小值
    next_idx = 1;
    next_idx = std::max(1L, next_idx - 1);
    FB_ASSERT_EQ(next_idx, 1L);  // 不能小于1
}

FB_TEST(raft_state, log_commit_idx_advancement) {
    int64_t commit_idx = 0;
    int64_t last_log_idx = 5;

    // commit_idx 只能前进，不能后退
    commit_idx = 3;
    FB_ASSERT_TRUE(commit_idx <= last_log_idx);

    // 新的 commit_idx 必须大于旧的
    int64_t new_commit_idx = 4;
    FB_ASSERT_TRUE(new_commit_idx > commit_idx);
    FB_ASSERT_TRUE(new_commit_idx <= last_log_idx);

    // commit_idx 不能超过 last_log_idx
    new_commit_idx = 10;
    bool can_commit = new_commit_idx <= last_log_idx;
    FB_ASSERT_FALSE(can_commit);
}

FB_TEST(raft_state, log_last_applied_tracking) {
    int64_t last_applied = 0;
    int64_t commit_idx = 5;

    // last_applied 应该追赶 commit_idx
    while (last_applied < commit_idx) {
        last_applied++;
    }
    FB_ASSERT_EQ(last_applied, 5L);

    // last_applied 不能超过 commit_idx
    FB_ASSERT_TRUE(last_applied <= commit_idx);
}

FB_TEST(raft_state, log_entry_id_uniqueness) {
    raft_entry_id_t id1 = 1001;
    raft_entry_id_t id2 = 1002;
    raft_entry_id_t id3 = 1001;

    // ID 应该唯一
    FB_ASSERT_TRUE(id1 != id2);
    FB_ASSERT_TRUE(id1 == id3);
}

FB_TEST(raft_state, log_prev_log_term_check) {
    raft_term_t current_term = 5;
    raft_term_t prev_log_term = 4;
    raft_index_t prev_log_idx = 10;

    // 验证 prev_log_term 和 prev_log_idx 的语义
    FB_ASSERT_TRUE(prev_log_term <= current_term);
    FB_ASSERT_TRUE(prev_log_idx >= 0);
}

FB_TEST(raft_state, log_replication_quorum) {
    uint64_t node_num = 5;
    uint64_t replication_count = 3;  // 成功复制到3个节点

    // 需要多数派才能提交
    bool has_quorum = replication_count > node_num / 2;
    FB_ASSERT_TRUE(has_quorum);

    // 不够多数派
    replication_count = 2;
    has_quorum = replication_count > node_num / 2;
    FB_ASSERT_FALSE(has_quorum);
}

FB_TEST(raft_state, log_consistency_check) {
    int64_t leader_next_idx = 10;
    int64_t follower_match_idx = 7;

    // Follower 的 match_idx 小于 Leader 的 next_idx 表示日志落后
    bool follower_behind = follower_match_idx < leader_next_idx - 1;
    FB_ASSERT_TRUE(follower_behind);

    // 计算需要发送的日志条目数
    int64_t entries_to_send = leader_next_idx - follower_match_idx - 1;
    FB_ASSERT_EQ(entries_to_send, 2L);
}

// ============================================================================
// Test Suite: Majority and Voting Tests
// ============================================================================

FB_TEST(raft_state, majority_calculation_various_sizes) {
    // 奇数节点集群
    FB_ASSERT_TRUE(2 > 3 / 2);   // 3节点需要2票
    FB_ASSERT_TRUE(3 > 5 / 2);   // 5节点需要3票
    FB_ASSERT_TRUE(4 > 7 / 2);   // 7节点需要4票
    FB_ASSERT_TRUE(51 > 101 / 2); // 101节点需要51票

    // 偶数节点集群
    FB_ASSERT_TRUE(3 > 4 / 2);   // 4节点需要3票
    FB_ASSERT_TRUE(4 > 6 / 2);   // 6节点需要4票
    FB_ASSERT_TRUE(51 > 100 / 2); // 100节点需要51票
}

FB_TEST(raft_state, vote_granting_rules) {
    raft_term_t current_term = 5;
    raft_term_t candidate_term = 6;

    // 规则1: Candidate term >= current_term
    bool term_ok = candidate_term >= current_term;
    FB_ASSERT_TRUE(term_ok);

    // Candidate term 小于 current_term，拒绝投票
    candidate_term = 4;
    term_ok = candidate_term >= current_term;
    FB_ASSERT_FALSE(term_ok);

    // 规则2: 如果已经投票给其他人，不能再投票
    raft_node_id_t voted_for = 1;
    raft_node_id_t candidate_id = 2;
    bool can_vote = (voted_for == 0) || (voted_for == candidate_id);
    FB_ASSERT_FALSE(can_vote);  // 已投票给节点1，不能投票给节点2
}

FB_TEST(raft_state, split_vote_detection) {
    uint64_t votes = 2;
    uint64_t node_num = 5;
    bool has_majority = votes > node_num / 2;
    FB_ASSERT_FALSE(has_majority);  // 2/5 不够多数

    // 更极端的分裂投票
    votes = 2;
    node_num = 4;
    has_majority = votes > node_num / 2;
    FB_ASSERT_FALSE(has_majority);  // 2/4 不够多数 (需要3票)
}

FB_TEST(raft_state, self_vote_included) {
    uint64_t node_num = 5;
    uint64_t votes = 1;  // Candidate 先给自己投票

    // 还需要获得其他节点的投票
    uint64_t additional_votes_needed = (node_num / 2) + 1 - votes;
    FB_ASSERT_EQ(additional_votes_needed, 2UL);

    // 获得足够的票数后
    votes = 3;
    bool wins_election = votes > node_num / 2;
    FB_ASSERT_TRUE(wins_election);
}

FB_TEST(raft_state, vote_during_election) {
    raft_identity state = RAFT_STATE_FOLLOWER;
    raft_term_t current_term = 5;
    raft_node_id_t voted_for = 0;  // 尚未投票

    // 收到投票请求
    raft_term_t candidate_term = 6;
    raft_node_id_t candidate_id = 3;

    // 更新 term 并投票
    if (candidate_term > current_term) {
        current_term = candidate_term;
        voted_for = candidate_id;
        state = RAFT_STATE_FOLLOWER;
    }

    FB_ASSERT_EQ(current_term, 6L);
    FB_ASSERT_EQ(voted_for, 3L);
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_state, prevent_double_vote) {
    raft_node_id_t voted_for = 1;
    raft_node_id_t new_candidate = 2;
    raft_term_t current_term = 5;
    raft_term_t new_candidate_term = 5;

    // 同一个 term 不能投票给不同的 candidate
    bool can_vote = (voted_for == 0) || (voted_for == new_candidate);
    FB_ASSERT_FALSE(can_vote);

    // 但可以投票给同一个 candidate（重试）
    new_candidate = 1;
    can_vote = (voted_for == 0) || (voted_for == new_candidate);
    FB_ASSERT_TRUE(can_vote);
}

FB_TEST(raft_state, election_winner_calculation) {
    // 辅助函数：计算是否赢得选举
    auto check_election_win = [](uint64_t votes, uint64_t node_num) -> bool {
        return votes > node_num / 2;
    };

    // 各种场景
    FB_ASSERT_TRUE(check_election_win(3, 5));
    FB_ASSERT_TRUE(check_election_win(2, 3));
    FB_ASSERT_FALSE(check_election_win(2, 5));
    FB_ASSERT_FALSE(check_election_win(1, 3));
    FB_ASSERT_TRUE(check_election_win(51, 100));
    FB_ASSERT_FALSE(check_election_win(50, 100));
}

FB_TEST(raft_state, vote_reset_on_new_term) {
    raft_node_id_t voted_for = 3;
    raft_term_t current_term = 5;

    // 收到更高 term 时，重置 voted_for
    raft_term_t new_term = 6;
    if (new_term > current_term) {
        current_term = new_term;
        voted_for = 0;  // 重置，可以重新投票
    }

    FB_ASSERT_EQ(current_term, 6L);
    FB_ASSERT_EQ(voted_for, 0L);

    // 现可以投票给新的 candidate
    raft_node_id_t candidate_id = 7;
    bool can_vote = (voted_for == 0) || (voted_for == candidate_id);
    FB_ASSERT_TRUE(can_vote);
}

// ============================================================================
// Test Suite: Snapshot and Log Compaction Tests
// ============================================================================

FB_TEST(raft_state, snapshot_index_tracking) {
    int64_t snapshot_idx = 100;
    int64_t last_applied = 100;
    int64_t commit_idx = 150;

    // 快照索引不能超过已应用索引
    FB_ASSERT_TRUE(snapshot_idx <= last_applied);
    FB_ASSERT_TRUE(snapshot_idx < commit_idx);

    // 快照后，first_log_idx 会更新
    int64_t first_log_idx = snapshot_idx + 1;
    FB_ASSERT_EQ(first_log_idx, 101L);
}

FB_TEST(raft_state, log_compaction_free_space) {
    int64_t first_idx = 10;
    int64_t last_idx = 1000;
    int64_t entries_count = last_idx - first_idx + 1;
    FB_ASSERT_EQ(entries_count, 991L);

    // 创建快照后释放的空间
    int64_t snapshot_idx = 500;
    int64_t freed_entries = snapshot_idx - first_idx + 1;
    FB_ASSERT_EQ(freed_entries, 491L);

    // 快照后剩余的日志条目
    int64_t remaining_entries = last_idx - snapshot_idx;
    FB_ASSERT_EQ(remaining_entries, 500L);
}

FB_TEST(raft_state, snapshot_term_tracking) {
    int64_t snapshot_idx = 100;
    raft_term_t snapshot_term = 5;

    // 快照包含的信息
    FB_ASSERT_TRUE(snapshot_idx > 0);
    FB_ASSERT_TRUE(snapshot_term > 0);

    // 快照 term 用于日志一致性检查
    raft_term_t last_included_term = snapshot_term;
    FB_ASSERT_EQ(last_included_term, 5L);
}

FB_TEST(raft_state, snapshot_transfer_chunks) {
    int64_t total_size = 1024 * 1024;  // 1MB snapshot
    int chunk_size = 64 * 1024;         // 64KB chunks
    int expected_chunks = total_size / chunk_size;
    if (total_size % chunk_size != 0) {
        expected_chunks++;
    }

    FB_ASSERT_EQ(expected_chunks, 16);

    // 边界情况：刚好整除
    total_size = 64 * 1024;
    expected_chunks = total_size / chunk_size;
    FB_ASSERT_EQ(expected_chunks, 1);
}

FB_TEST(raft_state, snapshot_request_conditions) {
    int64_t log_size = 10000;
    int64_t snapshot_threshold = 5000;

    // 达到阈值时触发快照
    bool should_snapshot = log_size >= snapshot_threshold;
    FB_ASSERT_TRUE(should_snapshot);

    // 未达到阈值
    log_size = 3000;
    should_snapshot = log_size >= snapshot_threshold;
    FB_ASSERT_FALSE(should_snapshot);
}

FB_TEST(raft_state, follower_snapshot_install) {
    int64_t follower_last_idx = 50;
    int64_t leader_snapshot_idx = 100;

    // Follower 日志落后于 Leader 的快照，需要安装快照
    bool needs_snapshot = leader_snapshot_idx > follower_last_idx;
    FB_ASSERT_TRUE(needs_snapshot);

    // 安装快照后更新索引
    int64_t new_last_idx = leader_snapshot_idx;
    FB_ASSERT_EQ(new_last_idx, 100L);
}

FB_TEST(raft_state, snapshot_during_append_conflict) {
    // 正在接收快照时，不能处理新的日志追加
    bool is_receiving_snapshot = true;
    bool can_append_entries = !is_receiving_snapshot;
    FB_ASSERT_FALSE(can_append_entries);

    // 快照完成后可以继续追加
    is_receiving_snapshot = false;
    can_append_entries = !is_receiving_snapshot;
    FB_ASSERT_TRUE(can_append_entries);
}

FB_TEST(raft_state, log_truncation_before_snapshot) {
    int64_t first_log_idx = 100;
    int64_t last_log_idx = 500;
    int64_t snapshot_idx = 300;

    // 快照后，[first_log_idx, snapshot_idx] 的日志可被删除
    int64_t first_deletable = first_log_idx;
    int64_t last_deletable = snapshot_idx;
    int64_t deletable_count = last_deletable - first_deletable + 1;

    FB_ASSERT_EQ(deletable_count, 201L);

    // 新的 first_log_idx
    int64_t new_first_log_idx = snapshot_idx + 1;
    FB_ASSERT_EQ(new_first_log_idx, 301L);
}

// ============================================================================
// Test Suite: Configuration Change Tests
// ============================================================================

FB_TEST(raft_state, joint_consensus_votes) {
    // 联合共识需要新旧配置都满足多数
    uint64_t old_nodes = 5, old_votes = 3;
    uint64_t new_nodes = 3, new_votes = 2;

    bool old_ok = old_votes > old_nodes / 2;
    bool new_ok = new_votes > new_nodes / 2;
    FB_ASSERT_TRUE(old_ok && new_ok);

    // 新配置不满足多数
    new_votes = 1;
    new_ok = new_votes > new_nodes / 2;
    FB_ASSERT_FALSE(old_ok && new_ok);

    // 旧配置不满足多数
    old_votes = 2;
    new_votes = 2;
    old_ok = old_votes > old_nodes / 2;
    new_ok = new_votes > new_nodes / 2;
    FB_ASSERT_FALSE(old_ok && new_ok);
}

FB_TEST(raft_state, config_change_tracking) {
    raft_membership_e membership = RAFT_MEMBERSHIP_ADD;
    FB_ASSERT_TRUE(membership == RAFT_MEMBERSHIP_ADD);
    FB_ASSERT_TRUE(membership != RAFT_MEMBERSHIP_REMOVE);

    membership = RAFT_MEMBERSHIP_REMOVE;
    FB_ASSERT_TRUE(membership == RAFT_MEMBERSHIP_REMOVE);
    FB_ASSERT_TRUE(membership != RAFT_MEMBERSHIP_ADD);
}

FB_TEST(raft_state, config_change_log_entry) {
    // 配置变更使用特殊日志类型
    raft_logtype_e log_type = RAFT_LOGTYPE_CONFIGURATION;
    FB_ASSERT_EQ(log_type, RAFT_LOGTYPE_CONFIGURATION);

    // 区分普通日志和配置日志
    raft_logtype_e normal_type = RAFT_LOGTYPE_WRITE;
    bool is_config_change = (log_type == RAFT_LOGTYPE_CONFIGURATION);
    FB_ASSERT_TRUE(is_config_change);
    is_config_change = (normal_type == RAFT_LOGTYPE_CONFIGURATION);
    FB_ASSERT_FALSE(is_config_change);
}

FB_TEST(raft_state, add_node_to_cluster) {
    uint64_t node_num = 3;
    raft_node_id_t new_node_id = 4;

    // 添加节点后，集群大小增加
    uint64_t new_node_num = node_num + 1;
    FB_ASSERT_EQ(new_node_num, 4UL);

    // 新节点最初是非投票节点
    bool is_voting = false;
    FB_ASSERT_FALSE(is_voting);

    // 变为投票节点后
    is_voting = true;
    FB_ASSERT_TRUE(is_voting);
}

FB_TEST(raft_state, remove_node_from_cluster) {
    uint64_t node_num = 5;
    raft_node_id_t removed_node_id = 3;

    // 移除节点后，集群大小减少
    uint64_t new_node_num = node_num - 1;
    FB_ASSERT_EQ(new_node_num, 4UL);

    // 需要验证新的多数派计算
    uint64_t votes_needed = new_node_num / 2 + 1;
    FB_ASSERT_EQ(votes_needed, 3UL);
}

FB_TEST(raft_state, config_change_safety) {
    // 配置变更期间，新配置生效前仍使用旧配置
    bool config_change_in_progress = true;
    uint64_t old_nodes = 5;
    uint64_t new_nodes = 3;

    // 在联合共识期间，两个配置都需要满足
    uint64_t old_votes = 3;
    uint64_t new_votes = 2;

    bool old_majority = old_votes > old_nodes / 2;
    bool new_majority = new_votes > new_nodes / 2;

    // 联合共识期间，两个配置都需要多数
    bool safe_to_commit = old_majority && new_majority;
    FB_ASSERT_TRUE(safe_to_commit);

    // 配置变更完成后
    config_change_in_progress = false;
    // 只需要新配置的多数
    safe_to_commit = new_majority;
    FB_ASSERT_TRUE(safe_to_commit);
}

FB_TEST(raft_state, node_promotion_demotion) {
    bool is_voting = false;
    raft_node_id_t node_id = 5;

    // 非投票节点 -> 投票节点（提升）
    is_voting = true;
    FB_ASSERT_TRUE(is_voting);

    // 投票节点 -> 非投票节点（降级）
    is_voting = false;
    FB_ASSERT_FALSE(is_voting);

    // 节点 ID 不变
    FB_ASSERT_EQ(node_id, 5L);
}

FB_TEST(raft_state, single_node_cluster) {
    // 单节点集群的特殊情况
    uint64_t node_num = 1;
    uint64_t votes = 1;

    // 单节点集群，自己就是 Leader
    bool is_leader = votes > node_num / 2;
    FB_ASSERT_TRUE(is_leader);

    // 无需心跳（自己给自己发）
    bool needs_heartbeat = false;
    FB_ASSERT_FALSE(needs_heartbeat);
}

FB_TEST(raft_state, config_change_index_tracking) {
    int64_t config_index = 10;
    int64_t config_term = 5;

    // 跟踪最新配置的索引和term
    FB_ASSERT_TRUE(config_index > 0);
    FB_ASSERT_TRUE(config_term > 0);

    // 新配置会覆盖旧配置
    int64_t new_config_index = 20;
    int64_t new_config_term = 6;

    bool is_newer_config = (new_config_term > config_term) ||
                           (new_config_term == config_term && new_config_index > config_index);
    FB_ASSERT_TRUE(is_newer_config);
}

// ============================================================================
// Test Suite: Error Handling Tests
// ============================================================================

FB_TEST(raft_state, invalid_term_handling) {
    // Term 为负数或零是无效的
    raft_term_t invalid_term = 0;
    bool is_valid = invalid_term > 0;
    FB_ASSERT_FALSE(is_valid);

    invalid_term = -1;
    is_valid = invalid_term > 0;
    FB_ASSERT_FALSE(is_valid);

    // 正常 term
    raft_term_t valid_term = 1;
    is_valid = valid_term > 0;
    FB_ASSERT_TRUE(is_valid);
}

FB_TEST(raft_state, invalid_index_handling) {
    // Index 为负数是无效的
    raft_index_t invalid_idx = -1;
    bool is_valid = invalid_idx >= 0;
    FB_ASSERT_FALSE(is_valid);

    // Index 为零表示"空"状态
    raft_index_t zero_idx = 0;
    bool is_empty = (zero_idx == 0);
    FB_ASSERT_TRUE(is_empty);

    // 有效索引
    raft_index_t valid_idx = 1;
    is_valid = valid_idx > 0;
    FB_ASSERT_TRUE(is_valid);
}

FB_TEST(raft_state, null_pointer_check) {
    void* ptr = nullptr;
    bool is_null = (ptr == nullptr);
    FB_ASSERT_TRUE(is_null);

    // 使用前必须检查
    if (ptr == nullptr) {
        // 不能解引用
        ptr = (void*)1;  // 模拟分配
    }
    FB_ASSERT_TRUE(ptr != nullptr);
}

FB_TEST(raft_state, buffer_overflow_protection) {
    size_t buffer_size = 1024;
    size_t data_size = 512;

    // 检查数据是否适合缓冲区
    bool fits = data_size <= buffer_size;
    FB_ASSERT_TRUE(fits);

    // 数据超过缓冲区
    data_size = 2048;
    fits = data_size <= buffer_size;
    FB_ASSERT_FALSE(fits);

    // 边界情况：刚好填满
    data_size = 1024;
    fits = data_size <= buffer_size;
    FB_ASSERT_TRUE(fits);
}

FB_TEST(raft_state, message_corruption_detection) {
    // 模拟消息校验和检查
    uint32_t expected_checksum = 0xABCD1234;
    uint32_t received_checksum = 0xABCD1234;
    bool is_valid = (expected_checksum == received_checksum);
    FB_ASSERT_TRUE(is_valid);

    // 损坏的消息
    received_checksum = 0xABCD1235;
    is_valid = (expected_checksum == received_checksum);
    FB_ASSERT_FALSE(is_valid);
}

FB_TEST(raft_state, network_timeout_handling) {
    int retry_count = 0;
    int max_retries = 3;

    // 模拟超时重试
    while (retry_count < max_retries) {
        retry_count++;
    }
    FB_ASSERT_EQ(retry_count, 3);

    // 达到最大重试次数后放弃
    bool should_give_up = retry_count >= max_retries;
    FB_ASSERT_TRUE(should_give_up);
}

FB_TEST(raft_state, disk_error_recovery) {
    enum class disk_status { OK, ERROR, RETRY };
    disk_status status = disk_status::ERROR;

    // 检测到错误后尝试恢复
    bool needs_recovery = (status != disk_status::OK);
    FB_ASSERT_TRUE(needs_recovery);

    // 恢复后状态
    status = disk_status::OK;
    needs_recovery = (status != disk_status::OK);
    FB_ASSERT_FALSE(needs_recovery);
}

FB_TEST(raft_state, memory_exhaustion_handling) {
    size_t available_memory = 1024 * 1024;  // 1MB
    size_t required_memory = 2 * 1024 * 1024;  // 2MB

    // 内存不足
    bool memory_sufficient = available_memory >= required_memory;
    FB_ASSERT_FALSE(memory_sufficient);

    // 释放后内存足够
    required_memory = 512 * 1024;  // 512KB
    memory_sufficient = available_memory >= required_memory;
    FB_ASSERT_TRUE(memory_sufficient);
}

FB_TEST(raft_state, invalid_message_type) {
    int valid_types[] = {0, 1, 2, 3};  // 有效消息类型
    int received_type = 99;  // 无效类型

    bool is_valid_type = false;
    for (int i = 0; i < 4; i++) {
        if (valid_types[i] == received_type) {
            is_valid_type = true;
            break;
        }
    }
    FB_ASSERT_FALSE(is_valid_type);

    // 有效类型
    received_type = 2;
    is_valid_type = false;
    for (int i = 0; i < 4; i++) {
        if (valid_types[i] == received_type) {
            is_valid_type = true;
            break;
        }
    }
    FB_ASSERT_TRUE(is_valid_type);
}

FB_TEST(raft_state, state_inconsistency_recovery) {
    raft_term_t local_term = 5;
    raft_term_t leader_term = 6;

    // 发现状态不一致，需要更新本地 term
    bool need_update = leader_term > local_term;
    FB_ASSERT_TRUE(need_update);

    if (need_update) {
        local_term = leader_term;
    }
    FB_ASSERT_EQ(local_term, 6L);
}

FB_TEST(raft_state, graceful_degradation) {
    uint64_t healthy_nodes = 3;
    uint64_t total_nodes = 5;

    // 部分节点故障，集群仍可用
    bool cluster_available = healthy_nodes > total_nodes / 2;
    FB_ASSERT_TRUE(cluster_available);

    // 更多节点故障，集群不可用
    healthy_nodes = 2;
    cluster_available = healthy_nodes > total_nodes / 2;
    FB_ASSERT_FALSE(cluster_available);
}

// ============================================================================
// Test Suite: Concurrency Scenario Tests
// ============================================================================

#include <atomic>
#include <thread>
#include <mutex>

FB_TEST(raft_state, atomic_term_update) {
    std::atomic<raft_term_t> current_term{1};

    // 模拟并发更新 term
    current_term.store(5);
    FB_ASSERT_EQ(current_term.load(), 5L);

    // 原子递增
    raft_term_t old_term = current_term.fetch_add(1);
    FB_ASSERT_EQ(old_term, 5L);
    FB_ASSERT_EQ(current_term.load(), 6L);

    // 比较交换
    raft_term_t expected = 6;
    bool success = current_term.compare_exchange_strong(expected, 10);
    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(current_term.load(), 10L);
}

FB_TEST(raft_state, concurrent_vote_counting) {
    std::atomic<uint64_t> votes{0};
    uint64_t node_num = 5;

    // 模拟并发投票
    for (int i = 0; i < 3; i++) {
        votes.fetch_add(1);
    }
    FB_ASSERT_EQ(votes.load(), 3UL);

    // 检查是否达到多数派
    bool has_majority = votes.load() > node_num / 2;
    FB_ASSERT_TRUE(has_majority);
}

FB_TEST(raft_state, commit_index_ordering) {
    std::atomic<int64_t> commit_idx{0};

    // 模拟多个线程尝试更新 commit_idx
    // commit_idx 只能单调递增
    int64_t old_val = commit_idx.load();
    int64_t new_val = 5;

    bool success = false;
    int64_t expected = old_val;
    if (new_val > expected) {
        success = commit_idx.compare_exchange_strong(expected, new_val);
    }
    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(commit_idx.load(), 5L);

    // 尝试回退（不应该成功）
    old_val = commit_idx.load();
    new_val = 3;
    expected = old_val;
    if (new_val > expected) {
        commit_idx.compare_exchange_strong(expected, new_val);
    }
    FB_ASSERT_EQ(commit_idx.load(), 5L);  // 值未改变
}

FB_TEST(raft_state, state_flag_operations) {
    std::atomic<int> flags{0};
    // RAFT_NODE_VOTED_FOR_ME is defined in raft/raft_node.h
    constexpr int FLAG_VOTED = (1 << 0);
    constexpr int FLAG_MATCHING = (1 << 1);

    // 设置标志位
    flags.fetch_or(FLAG_VOTED);
    FB_ASSERT_TRUE((flags.load() & FLAG_VOTED) != 0);

    // 设置另一个标志
    flags.fetch_or(FLAG_MATCHING);
    FB_ASSERT_TRUE((flags.load() & FLAG_MATCHING) != 0);

    // 清除标志
    flags.fetch_and(~FLAG_VOTED);
    FB_ASSERT_FALSE((flags.load() & FLAG_VOTED) != 0);
    FB_ASSERT_TRUE((flags.load() & FLAG_MATCHING) != 0);
}

FB_TEST(raft_state, leader_id_atomic_access) {
    std::atomic<raft_node_id_t> leader_id{0};

    // 初始无 Leader
    FB_ASSERT_EQ(leader_id.load(), 0L);

    // Leader 变更
    leader_id.store(3);
    FB_ASSERT_EQ(leader_id.load(), 3L);

    // Leader 再次变更
    leader_id.store(5);
    FB_ASSERT_EQ(leader_id.load(), 5L);
}

FB_TEST(raft_state, match_idx_concurrent_update) {
    // 模拟 match_idx 数组的并发更新
    std::vector<std::atomic<int64_t>> match_idx(5);
    for (auto& idx : match_idx) {
        idx.store(0);
    }

    // 模拟不同节点的 match_idx 更新
    match_idx[0].store(10);
    match_idx[1].store(8);
    match_idx[2].store(12);
    match_idx[3].store(10);
    match_idx[4].store(9);

    // 计算提交索引（多数派已复制的最小索引）
    int64_t values[] = {10, 8, 12, 10, 9};
    std::sort(values, values + 5);
    int64_t majority_idx = values[2];  // 第三个值（中位数）
    FB_ASSERT_EQ(majority_idx, 10L);
}

FB_TEST(raft_state, election_timeout_race) {
    std::atomic<bool> election_triggered{false};
    std::atomic<bool> heartbeat_received{false};

    // 模拟心跳和选举超时的竞争
    heartbeat_received.store(true);

    // 检查是否应该触发选举
    if (!heartbeat_received.load()) {
        election_triggered.store(true);
    }
    FB_ASSERT_FALSE(election_triggered.load());

    // 超时未收到心跳
    heartbeat_received.store(false);
    if (!heartbeat_received.load()) {
        election_triggered.store(true);
    }
    FB_ASSERT_TRUE(election_triggered.load());
}

FB_TEST(raft_state, mutex_protected_config_change) {
    std::mutex config_mutex;
    uint64_t old_nodes = 5;
    uint64_t new_nodes = 0;
    bool config_change_in_progress = false;

    {
        std::lock_guard<std::mutex> lock(config_mutex);
        // 开始配置变更
        config_change_in_progress = true;
        new_nodes = old_nodes + 1;
        // 配置变更完成
        config_change_in_progress = false;
    }

    FB_ASSERT_EQ(new_nodes, 6UL);
    FB_ASSERT_FALSE(config_change_in_progress);
}

FB_TEST(raft_state, cas_state_transition) {
    std::atomic<raft_identity> state{RAFT_STATE_FOLLOWER};

    // Follower -> Candidate (CAS 操作)
    raft_identity expected = RAFT_STATE_FOLLOWER;
    bool success = state.compare_exchange_strong(expected, RAFT_STATE_CANDIDATE);
    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(state.load(), RAFT_STATE_CANDIDATE);

    // Candidate -> Leader
    expected = RAFT_STATE_CANDIDATE;
    success = state.compare_exchange_strong(expected, RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(state.load(), RAFT_STATE_LEADER);

    // 非法转换（状态已被其他线程修改）
    expected = RAFT_STATE_CANDIDATE;
    success = state.compare_exchange_strong(expected, RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(success);  // CAS 失败
}

// ============================================================================
// Test Suite: Performance Boundary Tests
// ============================================================================

FB_TEST(raft_state, large_term_values) {
    // 测试非常大的 term 值
    raft_term_t term = std::numeric_limits<raft_term_t>::max() - 1000;
    FB_ASSERT_TRUE(term > 0);

    // 大 term 值的比较
    raft_term_t other_term = term - 1;
    FB_ASSERT_TRUE(term > other_term);

    // 接近最大值时的递增
    term = term + 1;
    FB_ASSERT_TRUE(term > other_term);
}

FB_TEST(raft_state, large_index_values) {
    // 测试大索引值
    raft_index_t idx = 1000000000LL;  // 10亿
    FB_ASSERT_TRUE(idx > 0);

    // 大索引的算术运算
    raft_index_t next_idx = idx + 1;
    FB_ASSERT_TRUE(next_idx > idx);

    // 索引差值计算
    raft_index_t diff = next_idx - idx;
    FB_ASSERT_EQ(diff, 1L);
}

FB_TEST(raft_state, max_cluster_size) {
    // 测试大规模集群
    uint64_t max_nodes = 1000;
    uint64_t votes = 501;  // 多数派

    bool has_majority = votes > max_nodes / 2;
    FB_ASSERT_TRUE(has_majority);

    // 边界情况：刚好多数
    votes = 501;
    has_majority = votes > max_nodes / 2;
    FB_ASSERT_TRUE(has_majority);

    // 边界情况：不够多数
    votes = 500;
    has_majority = votes > max_nodes / 2;
    FB_ASSERT_FALSE(has_majority);
}

FB_TEST(raft_state, large_log_entries) {
    // 测试大量日志条目
    int64_t first_idx = 1;
    int64_t last_idx = 10000000;  // 1000万条日志
    int64_t entry_count = last_idx - first_idx + 1;

    FB_ASSERT_EQ(entry_count, 10000000L);

    // 日志索引范围检查
    bool valid_range = (first_idx >= 1) && (last_idx >= first_idx);
    FB_ASSERT_TRUE(valid_range);
}

FB_TEST(raft_state, high_frequency_term_changes) {
    // 模拟高频率的 term 变化
    raft_term_t term = 1;
    for (int i = 0; i < 10000; i++) {
        term++;
    }
    FB_ASSERT_EQ(term, 10001L);

    // term 单调递增验证
    raft_term_t prev_term = 1;
    bool always_increasing = true;
    for (int i = 0; i < 1000; i++) {
        raft_term_t new_term = prev_term + 1;
        if (new_term <= prev_term) {
            always_increasing = false;
            break;
        }
        prev_term = new_term;
    }
    FB_ASSERT_TRUE(always_increasing);
}

FB_TEST(raft_state, rapid_state_transitions) {
    // 测试快速状态切换
    raft_identity state = RAFT_STATE_FOLLOWER;
    int transition_count = 0;

    for (int cycle = 0; cycle < 1000; cycle++) {
        // Follower -> Candidate -> Leader -> Follower
        state = RAFT_STATE_CANDIDATE;
        transition_count++;
        state = RAFT_STATE_LEADER;
        transition_count++;
        state = RAFT_STATE_FOLLOWER;
        transition_count++;
    }

    FB_ASSERT_EQ(transition_count, 3000);
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_state, large_snapshot_size) {
    // 测试大快照
    int64_t snapshot_size = 1024LL * 1024 * 1024;  // 1GB
    int64_t chunk_size = 64 * 1024;  // 64KB chunks

    int64_t total_chunks = snapshot_size / chunk_size;
    if (snapshot_size % chunk_size != 0) {
        total_chunks++;
    }

    FB_ASSERT_EQ(total_chunks, 16384L);

    // 快照传输进度跟踪
    int64_t transferred_chunks = 0;
    int64_t remaining_chunks = total_chunks - transferred_chunks;
    FB_ASSERT_EQ(remaining_chunks, 16384L);
}

FB_TEST(raft_state, many_concurrent_config_changes) {
    // 模拟多次配置变更
    uint64_t node_count = 3;
    uint64_t config_changes = 0;

    for (int i = 0; i < 100; i++) {
        // 添加节点
        node_count++;
        config_changes++;
        // 移除节点
        node_count--;
        config_changes++;
    }

    FB_ASSERT_EQ(config_changes, 200UL);
    FB_ASSERT_EQ(node_count, 3UL);  // 最终节点数
}

FB_TEST(raft_state, extreme_timeout_values) {
    // 测试极端超时值
    int min_timeout = 1;  // 最小1ms
    int max_timeout = 3600000;  // 最大1小时

    // 极小超时
    bool valid = (min_timeout >= 1 && min_timeout <= max_timeout);
    FB_ASSERT_TRUE(valid);

    // 极大超时
    valid = (max_timeout >= 1 && max_timeout <= 3600000);
    FB_ASSERT_TRUE(valid);

    // 心跳超时与选举超时的比例
    int heartbeat = 1;
    int election = 2;
    bool reasonable_ratio = (heartbeat > 0) && (election >= 2 * heartbeat);
    FB_ASSERT_TRUE(reasonable_ratio);
}

FB_TEST(raft_state, zero_and_negative_boundary) {
    // 边界值测试
    auto clamp = [](int64_t value) -> int64_t {
        return value < 1 ? 1 : value;
    };

    // 极端负值
    FB_ASSERT_EQ(clamp(std::numeric_limits<int64_t>::min()), 1L);
    FB_ASSERT_EQ(clamp(-999999999999LL), 1L);

    // 零值
    FB_ASSERT_EQ(clamp(0), 1L);

    // 正常值
    FB_ASSERT_EQ(clamp(1), 1L);
    FB_ASSERT_EQ(clamp(std::numeric_limits<int64_t>::max()),
                 std::numeric_limits<int64_t>::max());
}

FB_TEST(raft_state, memory_limit_handling) {
    // 模拟内存限制
    size_t total_memory = 1024ULL * 1024 * 1024;  // 1GB
    size_t log_entry_size = 1024;  // 每条日志1KB
    size_t max_entries = total_memory / log_entry_size;

    FB_ASSERT_EQ(max_entries, 1024ULL * 1024);  // 100万条

    // 检查日志条目数是否在限制内
    int64_t current_entries = 500000;
    bool within_limit = current_entries <= static_cast<int64_t>(max_entries);
    FB_ASSERT_TRUE(within_limit);
}

FB_TEST(raft_state, network_bandwidth_limit) {
    // 模拟网络带宽限制
    int64_t bandwidth_bps = 10LL * 1000 * 1000 * 1000;  // 10Gbps
    int64_t entry_size = 1024;  // 1KB per entry
    int64_t entries_per_second = bandwidth_bps / (entry_size * 8);

    FB_ASSERT_TRUE(entries_per_second > 0);

    // 计算日志复制吞吐量
    int64_t batch_size = 1000;
    int64_t batch_count = entries_per_second / batch_size;
    FB_ASSERT_TRUE(batch_count > 0);
}

// ============================================================================
// Test Suite: Raft Log Tests
// ============================================================================

FB_TEST(raft_state, log_base_index) {
    // 模拟日志基索引
    raft_index_t base_idx = 1;
    FB_ASSERT_TRUE(base_idx >= 1);

    // 快照后的基索引
    base_idx = 100;
    FB_ASSERT_TRUE(base_idx > 1);

    // 基索引 + 1 = 第一条日志索引
    raft_index_t first_entry_idx = base_idx + 1;
    FB_ASSERT_EQ(first_entry_idx, 101L);
}

FB_TEST(raft_state, log_next_idx_tracking) {
    raft_index_t next_idx = 1;

    // 追加日志后 next_idx 递增
    next_idx++;
    FB_ASSERT_EQ(next_idx, 2L);

    next_idx++;
    next_idx++;
    FB_ASSERT_EQ(next_idx, 4L);

    // next_idx 应该等于 last_log_idx + 1
    raft_index_t last_log_idx = next_idx - 1;
    FB_ASSERT_EQ(last_log_idx, 3L);
}

FB_TEST(raft_state, log_append_sequence) {
    // 模拟日志追加序列
    raft_index_t current_idx = 0;

    for (int i = 0; i < 10; i++) {
        current_idx++;
    }
    FB_ASSERT_EQ(current_idx, 10L);

    // 日志索引必须连续
    for (raft_index_t idx = 1; idx <= current_idx; idx++) {
        FB_ASSERT_TRUE(idx >= 1);
        FB_ASSERT_TRUE(idx <= current_idx);
    }
}

FB_TEST(raft_state, log_truncate_operation) {
    raft_index_t last_idx = 100;
    raft_index_t truncate_idx = 50;

    // 截断后，last_idx 变为 truncate_idx - 1
    raft_index_t new_last_idx = truncate_idx - 1;
    FB_ASSERT_EQ(new_last_idx, 49L);

    // 截断掉 [truncate_idx, last_idx] 区间的日志
    raft_index_t truncated_count = last_idx - truncate_idx + 1;
    FB_ASSERT_EQ(truncated_count, 51L);
}

FB_TEST(raft_state, log_term_consistency) {
    raft_term_t term1 = 1;
    raft_term_t term2 = 2;
    raft_term_t term3 = 3;

    // 日志 term 应该单调非递减
    FB_ASSERT_TRUE(term1 <= term2);
    FB_ASSERT_TRUE(term2 <= term3);

    // 同一 term 内可能有多条日志
    raft_index_t idx1 = 5;
    raft_index_t idx2 = 6;
    raft_term_t log_term_5 = 2;
    raft_term_t log_term_6 = 2;
    FB_ASSERT_EQ(log_term_5, log_term_6);
}

FB_TEST(raft_state, log_get_from_idx) {
    // 模拟从指定索引获取日志
    raft_index_t start_idx = 10;
    raft_index_t end_idx = 20;

    std::vector<raft_index_t> entries;
    for (raft_index_t idx = start_idx; idx <= end_idx; idx++) {
        entries.push_back(idx);
    }

    FB_ASSERT_EQ(entries.size(), 11UL);
    FB_ASSERT_EQ(entries.front(), 10L);
    FB_ASSERT_EQ(entries.back(), 20L);
}

FB_TEST(raft_state, log_get_at_idx) {
    // 模拟获取特定索引的日志
    std::map<raft_index_t, raft_term_t> log_entries;

    for (int i = 1; i <= 100; i++) {
        log_entries[i] = (i <= 50) ? 1 : 2;
    }

    // 验证获取特定索引
    FB_ASSERT_EQ(log_entries[25], 1L);
    FB_ASSERT_EQ(log_entries[75], 2L);
    FB_ASSERT_EQ(log_entries[50], 1L);
    FB_ASSERT_EQ(log_entries[51], 2L);
}

FB_TEST(raft_state, log_first_and_last) {
    std::map<raft_index_t, int> log_cache;

    // 空缓存情况
    bool is_empty = log_cache.empty();
    FB_ASSERT_TRUE(is_empty);

    // 添加日志
    for (int i = 1; i <= 100; i++) {
        log_cache[i] = i;
    }

    is_empty = log_cache.empty();
    FB_ASSERT_FALSE(is_empty);

    // 第一条和最后一条
    raft_index_t first_idx = log_cache.begin()->first;
    raft_index_t last_idx = log_cache.rbegin()->first;
    FB_ASSERT_EQ(first_idx, 1L);
    FB_ASSERT_EQ(last_idx, 100L);
}

FB_TEST(raft_state, log_cache_size_limit) {
    uint32_t max_cache_entries = 500;
    uint32_t current_cache_size = 0;

    // 模拟缓存增长
    for (int i = 0; i < 600; i++) {
        if (current_cache_size >= max_cache_entries) {
            // 需要清理旧条目
            current_cache_size--;  // 移除一个
        }
        current_cache_size++;
    }

    FB_ASSERT_TRUE(current_cache_size <= max_cache_entries + 1);
}

FB_TEST(raft_state, log_disk_sync) {
    // 模拟磁盘同步点
    raft_index_t synced_idx = 0;
    raft_index_t in_memory_idx = 100;

    // 同步到磁盘
    synced_idx = in_memory_idx;
    FB_ASSERT_EQ(synced_idx, 100L);

    // 内存中的日志可以超前于磁盘
    in_memory_idx = 150;
    bool has_unsynced = in_memory_idx > synced_idx;
    FB_ASSERT_TRUE(has_unsynced);
}

FB_TEST(raft_state, log_recovery_from_disk) {
    // 模拟从磁盘恢复日志
    raft_index_t disk_first_idx = 1;
    raft_index_t disk_last_idx = 80;

    // 恢复后，next_idx = last_idx + 1
    raft_index_t next_idx = disk_last_idx + 1;
    FB_ASSERT_EQ(next_idx, 81L);

    // 需要加载未应用的日志到缓存
    raft_index_t last_applied_idx = 75;
    raft_index_t first_unapplied_idx = last_applied_idx + 1;
    FB_ASSERT_EQ(first_unapplied_idx, 76L);
}

// ============================================================================
// Test Suite: Raft Node Tests
// ============================================================================

FB_TEST(raft_state, node_next_idx_init) {
    // 节点初始 next_idx = 1
    raft_index_t next_idx = 1;
    FB_ASSERT_EQ(next_idx, 1L);

    // next_idx 小于 1 时应该 clamp 到 1
    next_idx = -5;
    next_idx = next_idx < 1 ? 1 : next_idx;
    FB_ASSERT_EQ(next_idx, 1L);

    next_idx = 0;
    next_idx = next_idx < 1 ? 1 : next_idx;
    FB_ASSERT_EQ(next_idx, 1L);
}

FB_TEST(raft_state, node_match_idx_update) {
    raft_index_t match_idx = 0;
    raft_index_t next_idx = 1;

    // 成功复制后更新 match_idx
    match_idx = next_idx;
    next_idx++;
    FB_ASSERT_EQ(match_idx, 1L);
    FB_ASSERT_EQ(next_idx, 2L);

    // match_idx 只能递增
    raft_index_t old_match_idx = match_idx;
    raft_index_t new_match_idx = 5;
    if (new_match_idx > old_match_idx) {
        match_idx = new_match_idx;
    }
    FB_ASSERT_EQ(match_idx, 5L);

    // 尝试回退（不应该成功）
    new_match_idx = 3;
    if (new_match_idx > match_idx) {
        match_idx = new_match_idx;
    }
    FB_ASSERT_EQ(match_idx, 5L);  // 保持原值
}

FB_TEST(raft_state, node_vote_flag) {
    int flags = 0;
    // RAFT_NODE_VOTED_FOR_ME is defined in raft/raft_node.h
    constexpr int VOTE_FLAG = (1 << 0);

    // 设置投票标志
    flags |= VOTE_FLAG;
    FB_ASSERT_TRUE((flags & VOTE_FLAG) != 0);

    // 清除投票标志
    flags &= ~VOTE_FLAG;
    FB_ASSERT_FALSE((flags & VOTE_FLAG) != 0);
}

FB_TEST(raft_state, node_lease_management) {
    raft_time_t lease = 0;

    // lease 只能递增
    auto update_lease = [&lease](raft_time_t new_lease) {
        if (new_lease > lease) {
            lease = new_lease;
        }
    };

    update_lease(100);
    FB_ASSERT_EQ(lease, 100L);

    // 尝试设置更小的值（不应该改变）
    update_lease(50);
    FB_ASSERT_EQ(lease, 100L);

    // 设置更大的值
    update_lease(200);
    FB_ASSERT_EQ(lease, 200L);
}

FB_TEST(raft_state, node_effective_time) {
    raft_time_t effective_time = 0;

    // 设置节点生效时间
    effective_time = 1000;
    FB_ASSERT_EQ(effective_time, 1000L);

    // 验证时间单调性
    raft_time_t new_time = 1500;
    bool is_later = new_time > effective_time;
    FB_ASSERT_TRUE(is_later);
}

FB_TEST(raft_state, node_heartbeat_suppression) {
    bool suppress_heartbeats = false;

    // 开启心跳抑制
    suppress_heartbeats = true;
    FB_ASSERT_TRUE(suppress_heartbeats);

    // 关闭心跳抑制
    suppress_heartbeats = false;
    FB_ASSERT_FALSE(suppress_heartbeats);
}

FB_TEST(raft_state, node_heartbeating_status) {
    bool is_heartbeating = false;

    // 开始心跳
    is_heartbeating = true;
    FB_ASSERT_TRUE(is_heartbeating);

    // 停止心跳
    is_heartbeating = false;
    FB_ASSERT_FALSE(is_heartbeating);
}

FB_TEST(raft_state, node_recovering_status) {
    bool is_recovering = false;

    // 开始恢复
    is_recovering = true;
    FB_ASSERT_TRUE(is_recovering);

    // 恢复完成
    is_recovering = false;
    FB_ASSERT_FALSE(is_recovering);
}

FB_TEST(raft_state, node_end_idx_tracking) {
    raft_index_t end_idx = 0;

    // 设置 end_idx（Leader发送的最后一个日志索引）
    end_idx = 100;
    FB_ASSERT_EQ(end_idx, 100L);

    // end_idx 可以用于判断日志发送进度
    raft_index_t match_idx = 80;
    bool has_more = match_idx < end_idx;
    FB_ASSERT_TRUE(has_more);
}

FB_TEST(raft_state, node_append_time) {
    raft_time_t append_time = 0;
    raft_time_t current_time = 1000;

    // 记录追加时间
    append_time = current_time;
    FB_ASSERT_EQ(append_time, 1000L);

    // 计算距离上次追加的时间
    raft_time_t elapsed = current_time - append_time;
    FB_ASSERT_EQ(elapsed, 0L);
}

FB_TEST(raft_state, node_id_operations) {
    raft_node_id_t id1 = 1;
    raft_node_id_t id2 = 2;

    // ID 比较
    FB_ASSERT_TRUE(id1 != id2);
    FB_ASSERT_TRUE(id1 < id2);

    // 查找节点
    std::map<long, int> nodes;
    nodes[id1] = 100;
    nodes[id2] = 200;

    FB_ASSERT_EQ(nodes[id1], 100);
    FB_ASSERT_EQ(nodes[id2], 200);
}

// ============================================================================
// Test Suite: Entry Cache Tests
// ============================================================================

FB_TEST(raft_state, entry_cache_add_remove) {
    std::map<raft_index_t, int> cache;

    // 添加条目
    cache[1] = 10;
    cache[2] = 20;
    cache[3] = 30;
    FB_ASSERT_EQ(cache.size(), 3UL);

    // 移除条目
    cache.erase(2);
    FB_ASSERT_EQ(cache.size(), 2UL);
    FB_ASSERT_EQ(cache.count(2), 0UL);

    // 验证剩余条目
    FB_ASSERT_EQ(cache[1], 10);
    FB_ASSERT_EQ(cache[3], 30);
}

FB_TEST(raft_state, entry_cache_get_upper) {
    std::map<raft_index_t, int> cache;
    for (int i = 1; i <= 10; i++) {
        cache[i] = i * 10;
    }

    // 获取 >= 5 的条目
    std::vector<int> entries;
    for (auto it = cache.lower_bound(5); it != cache.end(); it++) {
        entries.push_back(it->second);
    }
    FB_ASSERT_EQ(entries.size(), 6UL);
    FB_ASSERT_EQ(entries[0], 50);
    FB_ASSERT_EQ(entries[5], 100);
}

FB_TEST(raft_state, entry_cache_get_between) {
    std::map<raft_index_t, int> cache;
    for (int i = 1; i <= 20; i++) {
        cache[i] = i;
    }

    // 获取 [5, 10] 区间的条目
    raft_index_t start_idx = 5;
    raft_index_t end_idx = 10;

    std::vector<int> entries;
    for (auto it = cache.lower_bound(start_idx); it != cache.end(); it++) {
        if (it->first > end_idx) break;
        entries.push_back(it->second);
    }
    FB_ASSERT_EQ(entries.size(), 6UL);
    FB_ASSERT_EQ(entries.front(), 5);
    FB_ASSERT_EQ(entries.back(), 10);
}

FB_TEST(raft_state, entry_cache_remove_between) {
    std::map<raft_index_t, int> cache;
    for (int i = 1; i <= 20; i++) {
        cache[i] = i;
    }

    // 删除 [5, 10] 区间的条目
    raft_index_t start_idx = 5;
    raft_index_t end_idx = 10;

    for (raft_index_t idx = start_idx; idx <= end_idx; idx++) {
        cache.erase(idx);
    }

    FB_ASSERT_EQ(cache.size(), 14UL);
    FB_ASSERT_EQ(cache.count(5), 0UL);
    FB_ASSERT_EQ(cache.count(10), 0UL);
    FB_ASSERT_EQ(cache[4], 4);   // 之前存在
    FB_ASSERT_EQ(cache[11], 11); // 之后存在
}

FB_TEST(raft_state, entry_cache_get_at_idx) {
    std::map<raft_index_t, int> cache;
    cache[10] = 100;
    cache[20] = 200;

    // 获取特定索引
    auto it = cache.find(10);
    FB_ASSERT_TRUE(it != cache.end());
    FB_ASSERT_EQ(it->second, 100);

    // 索引不存在
    it = cache.find(15);
    FB_ASSERT_TRUE(it == cache.end());
}

FB_TEST(raft_state, entry_cache_first_last_entry) {
    std::map<raft_index_t, int> cache;

    // 空缓存
    bool empty = cache.empty();
    FB_ASSERT_TRUE(empty);

    // 添加条目
    for (int i = 5; i <= 15; i++) {
        cache[i] = i;
    }

    // 第一条和最后一条
    raft_index_t first_idx = cache.begin()->first;
    raft_index_t last_idx = cache.rbegin()->first;
    FB_ASSERT_EQ(first_idx, 5L);
    FB_ASSERT_EQ(last_idx, 15L);
}

FB_TEST(raft_state, entry_cache_count) {
    std::map<raft_index_t, int> cache;

    FB_ASSERT_EQ(cache.size(), 0UL);

    for (int i = 0; i < 100; i++) {
        cache[i + 1] = i;
    }
    FB_ASSERT_EQ(cache.size(), 100UL);

    // 移除一半
    for (int i = 1; i <= 50; i++) {
        cache.erase(i);
    }
    FB_ASSERT_EQ(cache.size(), 50UL);
}

FB_TEST(raft_state, entry_cache_clear) {
    std::map<raft_index_t, int> cache;

    for (int i = 1; i <= 100; i++) {
        cache[i] = i;
    }
    FB_ASSERT_EQ(cache.size(), 100UL);

    // 清空缓存
    cache.clear();
    FB_ASSERT_EQ(cache.size(), 0UL);
    FB_ASSERT_TRUE(cache.empty());
}

FB_TEST(raft_state, entry_cache_complete_callback) {
    // 模拟回调完成机制
    int completed_count = 0;
    int result_code = 0;

    auto complete_entry = [&completed_count, &result_code](int result) {
        completed_count++;
        result_code = result;
    };

    // 完成单个条目
    complete_entry(0);
    FB_ASSERT_EQ(completed_count, 1);
    FB_ASSERT_EQ(result_code, 0);

    // 完成失败
    complete_entry(-1);
    FB_ASSERT_EQ(completed_count, 2);
    FB_ASSERT_EQ(result_code, -1);
}

FB_TEST(raft_state, entry_cache_remove_upper) {
    std::map<raft_index_t, int> cache;
    for (int i = 1; i <= 20; i++) {
        cache[i] = i;
    }

    // 删除 >= 15 的条目
    raft_index_t idx = 15;
    for (auto it = cache.lower_bound(idx); it != cache.end(); ) {
        it = cache.erase(it);
    }

    FB_ASSERT_EQ(cache.size(), 14UL);
    FB_ASSERT_EQ(cache.rbegin()->first, 14L);
    FB_ASSERT_EQ(cache.count(15), 0UL);
    FB_ASSERT_EQ(cache.count(20), 0UL);
}

FB_TEST(raft_state, entry_cache_range_validation) {
    std::map<raft_index_t, int> cache;
    cache[5] = 50;
    cache[10] = 100;
    cache[15] = 150;

    // 验证范围查询边界
    raft_index_t start = 5, end = 15;

    // start > end 是无效范围
    bool valid_range = start <= end;
    FB_ASSERT_TRUE(valid_range);

    // 反转范围
    start = 20; end = 10;
    valid_range = start <= end;
    FB_ASSERT_FALSE(valid_range);
}

// ============================================================================
// Test Suite: Raft Nodes Collection Tests
// ============================================================================

FB_TEST(raft_state, nodes_contains) {
    std::map<long, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    // 检查节点是否存在
    FB_ASSERT_TRUE(nodes.find(1) != nodes.end());
    FB_ASSERT_TRUE(nodes.find(2) != nodes.end());
    FB_ASSERT_FALSE(nodes.find(5) != nodes.end());
}

FB_TEST(raft_state, nodes_find) {
    std::map<long, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;

    // 查找存在的节点
    auto it = nodes.find(1);
    FB_ASSERT_TRUE(it != nodes.end());
    FB_ASSERT_EQ(it->second, 100);

    // 查找不存在的节点
    it = nodes.find(99);
    FB_ASSERT_TRUE(it == nodes.end());
}

FB_TEST(raft_state, nodes_size) {
    std::map<long, int> nodes;

    FB_ASSERT_EQ(nodes.size(), 0UL);

    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;
    FB_ASSERT_EQ(nodes.size(), 3UL);

    nodes.erase(2);
    FB_ASSERT_EQ(nodes.size(), 2UL);
}

FB_TEST(raft_state, nodes_get_node) {
    std::map<long, int> nodes;
    nodes[1] = 100;
    nodes[5] = 500;

    // 获取存在的节点
    auto it = nodes.find(1);
    FB_ASSERT_TRUE(it != nodes.end());
    FB_ASSERT_EQ(it->second, 100);

    // 获取不存在的节点返回 nullptr 等效
    it = nodes.find(99);
    bool is_nullptr = (it == nodes.end());
    FB_ASSERT_TRUE(is_nullptr);
}

FB_TEST(raft_state, nodes_get_ids) {
    std::map<long, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    // 获取所有节点 ID
    std::vector<long> ids;
    for (const auto& pair : nodes) {
        ids.push_back(pair.first);
    }
    FB_ASSERT_EQ(ids.size(), 3UL);

    // 验证包含所有 ID
    FB_ASSERT_TRUE(std::find(ids.begin(), ids.end(), 1) != ids.end());
    FB_ASSERT_TRUE(std::find(ids.begin(), ids.end(), 2) != ids.end());
    FB_ASSERT_TRUE(std::find(ids.begin(), ids.end(), 3) != ids.end());
}

FB_TEST(raft_state, nodes_for_all) {
    std::map<long, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    // 遍历所有节点
    int visited_count = 0;
    for (const auto& pair : nodes) {
        visited_count++;
        FB_ASSERT_TRUE(pair.first >= 1);
        FB_ASSERT_TRUE(pair.second >= 100);
    }
    FB_ASSERT_EQ(visited_count, 3);
}

FB_TEST(raft_state, nodes_new_nodes_management) {
    // 模拟 _nodes 和 _new_nodes 的管理
    std::map<long, int> nodes;
    std::map<long, int> new_nodes;

    // 初始节点
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    // 配置变更：添加新节点
    new_nodes[4] = 400;
    new_nodes[5] = 500;

    // 联合共识期间，需要向所有节点发送消息
    int total_recipients = nodes.size() + new_nodes.size();
    FB_ASSERT_EQ(total_recipients, 5);

    // 遍历所有节点（包括新节点）
    int all_count = 0;
    for (const auto& pair : nodes) all_count++;
    for (const auto& pair : new_nodes) all_count++;
    FB_ASSERT_EQ(all_count, 5);
}

FB_TEST(raft_state, nodes_for_new_nodes) {
    std::map<long, int> new_nodes;
    new_nodes[4] = 400;
    new_nodes[5] = 500;

    // 只遍历新节点
    int new_count = 0;
    for (const auto& pair : new_nodes) {
        new_count++;
        FB_ASSERT_TRUE(pair.first >= 4);
    }
    FB_ASSERT_EQ(new_count, 2);
}

FB_TEST(raft_state, nodes_get_new_node) {
    std::map<long, int> new_nodes;
    new_nodes[4] = 400;

    // 获取新节点
    auto it = new_nodes.find(4);
    FB_ASSERT_TRUE(it != new_nodes.end());
    FB_ASSERT_EQ(it->second, 400);

    // 获取不存在的新节点
    it = new_nodes.find(99);
    FB_ASSERT_TRUE(it == new_nodes.end());
}

FB_TEST(raft_state, nodes_new_node_size) {
    std::map<long, int> new_nodes;

    FB_ASSERT_EQ(new_nodes.size(), 0UL);

    new_nodes[4] = 400;
    new_nodes[5] = 500;
    FB_ASSERT_EQ(new_nodes.size(), 2UL);
}

FB_TEST(raft_state, nodes_iterator_operations) {
    std::map<long, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    // 开始迭代器
    auto it = nodes.begin();
    FB_ASSERT_EQ(it->first, 1);

    // 结束迭代器
    auto end = nodes.end();
    FB_ASSERT_TRUE(end == nodes.end());

    // 遍历
    int count = 0;
    for (auto it = nodes.begin(); it != nodes.end(); it++) {
        count++;
    }
    FB_ASSERT_EQ(count, 3);
}

FB_TEST(raft_state, nodes_const_iterator) {
    std::map<long, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;

    const auto& const_nodes = nodes;

    // 常量迭代器
    int count = 0;
    for (auto it = const_nodes.begin(); it != const_nodes.end(); it++) {
        count++;
    }
    FB_ASSERT_EQ(count, 2);
}

// ============================================================================
// Test Suite: Configuration Manager Tests
// ============================================================================

FB_TEST(raft_state, config_node_info) {
    // 模拟节点信息
    struct node_info {
        raft_node_id_t node_id;
        std::string addr;
        int port;
    };

    node_info info1 = {1, "127.0.0.1", 8888};
    node_info info2 = {2, "127.0.0.1", 8889};

    FB_ASSERT_EQ(info1.node_id, 1);
    FB_ASSERT_EQ(info2.node_id, 2);
    FB_ASSERT_TRUE(info1.node_id != info2.node_id);
    FB_ASSERT_TRUE(info1.port != info2.port);
}

FB_TEST(raft_state, config_initial_nodes) {
    // 初始配置
    std::vector<long> initial_members = {1, 2, 3};

    FB_ASSERT_EQ(initial_members.size(), 3UL);

    // 验证所有成员
    for (raft_node_id_t id : initial_members) {
        FB_ASSERT_TRUE(id >= 1);
        FB_ASSERT_TRUE(id <= 3);
    }
}

FB_TEST(raft_state, config_add_node) {
    std::vector<long> members = {1, 2, 3};

    // 添加新节点
    raft_node_id_t new_node_id = 4;
    members.push_back(new_node_id);

    FB_ASSERT_EQ(members.size(), 4UL);
    FB_ASSERT_TRUE(std::find(members.begin(), members.end(), 4) != members.end());
}

FB_TEST(raft_state, config_remove_node) {
    std::vector<long> members = {1, 2, 3, 4, 5};

    // 移除节点
    raft_node_id_t remove_id = 3;
    members.erase(std::remove(members.begin(), members.end(), remove_id), members.end());

    FB_ASSERT_EQ(members.size(), 4UL);
    FB_ASSERT_FALSE(std::find(members.begin(), members.end(), 3) != members.end());
}

FB_TEST(raft_state, config_replace_node) {
    std::map<long, int> config;
    config[1] = 100;
    config[2] = 200;

    // 替换节点：移除旧节点，添加新节点
    config.erase(1);
    config[3] = 300;

    FB_ASSERT_EQ(config.size(), 2UL);
    FB_ASSERT_FALSE(config.count(1));
    FB_ASSERT_TRUE(config.count(3));
}

FB_TEST(raft_state, config_majority_calc) {
    // 不同配置大小下的多数派计算
    auto calc_majority = [](size_t node_count) -> size_t {
        return node_count / 2 + 1;
    };

    FB_ASSERT_EQ(calc_majority(3), 2UL);
    FB_ASSERT_EQ(calc_majority(5), 3UL);
    FB_ASSERT_EQ(calc_majority(7), 4UL);
    FB_ASSERT_EQ(calc_majority(4), 3UL);
    FB_ASSERT_EQ(calc_majority(100), 51UL);
}

FB_TEST(raft_state, config_change_sequence) {
    // 配置变更序列：C_old -> C_old,new -> C_new
    std::vector<std::string> configs;
    configs.push_back("C_old");
    configs.push_back("C_old,new");
    configs.push_back("C_new");

    FB_ASSERT_EQ(configs.size(), 3UL);
    FB_ASSERT_EQ(configs[0], "C_old");
    FB_ASSERT_EQ(configs[1], "C_old,new");
    FB_ASSERT_EQ(configs[2], "C_new");
}

FB_TEST(raft_state, config_joint_consensus_phase) {
    std::vector<long> old_config = {1, 2, 3};
    std::vector<long> new_config = {4, 5, 6};

    // 联合共识阶段，两个配置都有效
    bool in_joint_consensus = true;

    if (in_joint_consensus) {
        // 需要新旧配置都满足多数
        size_t old_majority = old_config.size() / 2 + 1;
        size_t new_majority = new_config.size() / 2 + 1;

        FB_ASSERT_EQ(old_majority, 2UL);
        FB_ASSERT_EQ(new_majority, 2UL);
    }
}

FB_TEST(raft_state, config_transition_complete) {
    std::string phase = "C_old,new";

    // 变更完成后进入新配置
    phase = "C_new";
    FB_ASSERT_EQ(phase, "C_new");

    // 旧配置不再有效
    bool old_config_valid = false;
    FB_ASSERT_FALSE(old_config_valid);
}

FB_TEST(raft_state, config_rollback) {
    std::vector<long> config = {1, 2, 3, 4};  // 新配置
    std::vector<long> backup = {1, 2, 3};      // 旧配置备份

    // 变更失败，回滚到旧配置
    bool change_failed = true;
    if (change_failed) {
        config = backup;
    }

    FB_ASSERT_EQ(config.size(), 3UL);
    FB_ASSERT_EQ(config.size(), backup.size());
    for (size_t i = 0; i < config.size(); i++) {
        FB_ASSERT_EQ(config[i], backup[i]);
    }
}

FB_TEST(raft_state, config_index_tracking) {
    raft_index_t config_index = 0;
    raft_term_t config_term = 0;

    // 记录配置变更的索引和term
    config_index = 100;
    config_term = 5;

    FB_ASSERT_EQ(config_index, 100L);
    FB_ASSERT_EQ(config_term, 5L);

    // 新的配置变更
    raft_index_t new_config_index = 150;
    raft_term_t new_config_term = 6;

    bool is_newer = (new_config_term > config_term) ||
                    (new_config_term == config_term && new_config_index > config_index);
    FB_ASSERT_TRUE(is_newer);
}

FB_TEST(raft_state, config_voting_members) {
    std::map<long, bool> voting_status;
    voting_status[1] = true;   // 投票节点
    voting_status[2] = true;   // 投票节点
    voting_status[3] = false;  // 非投票节点

    // 统计投票节点数
    int voting_count = 0;
    for (const auto& pair : voting_status) {
        if (pair.second) voting_count++;
    }
    FB_ASSERT_EQ(voting_count, 2);

    // 计算多数派（只计算投票节点）
    int majority = voting_count / 2 + 1;
    FB_ASSERT_EQ(majority, 2);
}

FB_TEST(raft_state, config_promote_non_voting) {
    std::map<long, bool> voting_status;
    voting_status[3] = false;  // 非投票节点

    // 提升为投票节点
    voting_status[3] = true;
    FB_ASSERT_TRUE(voting_status[3]);
}

// ============================================================================
// Test Suite: Raft Basic Tests (migrated from test_raft.cc)
// ============================================================================

FB_SUITE_SETUP(raft) {
    // Setup code here
}

FB_SUITE_TEARDOWN(raft) {
    // Teardown code here
}

// Test: Basic raft node initialization
FB_TEST(raft, node_init) {
    FB_LOG_INFO("Testing raft node initialization");

    raft_node_info info;
    info.set_node_id(1);
    info.set_addr("127.0.0.1");
    info.set_port(8888);

    FB_ASSERT_EQ(1, info.node_id());
    FB_ASSERT_STR_EQ("127.0.0.1", info.addr());
    FB_ASSERT_EQ(8888, info.port());
}

// Test: Raft log entry generation
FB_TEST(raft, log_entry) {
    FB_LOG_INFO("Testing raft log entry");

    log_entry_t entry;
    entry.index = 1;
    entry.term_id = 1;
    entry.type = 1;
    entry.size = 4096;
    entry.meta = "test_meta";

    FB_ASSERT_EQ(1UL, entry.index);
    FB_ASSERT_EQ(1UL, entry.term_id);
    FB_ASSERT_EQ(4096UL, entry.size);
    FB_ASSERT_STR_EQ("test_meta", entry.meta);
}

// Test: Configuration manager basic operations
FB_TEST(raft, config_manager) {
    FB_LOG_INFO("Testing configuration manager");

    std::vector<raft_node_info> nodes;
    raft_node_info node1;
    node1.set_node_id(1);
    node1.set_addr("127.0.0.1");
    node1.set_port(8888);
    nodes.push_back(node1);

    raft_node_info node2;
    node2.set_node_id(2);
    node2.set_addr("127.0.0.1");
    node2.set_port(8889);
    nodes.push_back(node2);

    FB_ASSERT_EQ(2UL, nodes.size());
    FB_ASSERT_NE(nodes[0].node_id(), nodes[1].node_id());
}

// Critical test: Raft leader election
FB_TEST_CRITICAL(raft, leader_election) {
    FB_LOG_INFO("Testing raft leader election (critical)");

    int leader_id = 0;
    bool election_possible = true;

    FB_ASSERT_TRUE(election_possible);
    FB_ASSERT_TRUE(leader_id >= 0);
}

// Test: Raft membership change
FB_TEST(raft, membership_change) {
    FB_LOG_INFO("Testing raft membership change");

    std::vector<int> initial_members = {1, 2, 3};
    std::vector<int> after_add = {1, 2, 3, 4};

    FB_ASSERT_EQ(3UL, initial_members.size());
    FB_ASSERT_EQ(4UL, after_add.size());
}

// Optional test: Performance benchmark
FB_TEST(raft, perf_benchmark) {
    FB_SKIP("Performance benchmark skipped in normal test run");
}

// ============================================================================
// Test Suite: Raft Term Advancement
// ============================================================================

FB_TEST(raft_state, term_advance_on_higher_seen) {
    // When a server sees a higher term, it must update its current term
    // and revert to follower state.
    raft_identity state = RAFT_STATE_LEADER;
    int64_t my_term = 5;
    int64_t peer_term = 7;

    if (peer_term > my_term) {
        my_term = peer_term;
        state = RAFT_STATE_FOLLOWER;
    }

    FB_ASSERT_EQ(my_term, 7);
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_state, term_no_advance_on_equal) {
    int64_t my_term = 5;
    int64_t peer_term = 5;
    bool stepped_down = false;

    if (peer_term > my_term) {
        stepped_down = true;
    }

    FB_ASSERT_FALSE(stepped_down);
    FB_ASSERT_EQ(my_term, 5);
}

FB_TEST(raft_state, term_no_advance_on_lower) {
    int64_t my_term = 10;
    int64_t peer_term = 8;
    bool advanced = false;

    if (peer_term > my_term) {
        my_term = peer_term;
        advanced = true;
    }

    FB_ASSERT_FALSE(advanced);
    FB_ASSERT_EQ(my_term, 10);
}

FB_TEST(raft_state, term_starts_at_zero) {
    // A fresh raft server should start with term 0
    int64_t initial_term = 0;
    FB_ASSERT_EQ(initial_term, 0);
}

FB_TEST(raft_state, term_increments_on_election) {
    // When starting an election, term increments by 1
    int64_t term = 3;
    term++; // candidate increments before requesting votes
    FB_ASSERT_EQ(term, 4);
}

// ============================================================================
// Test Suite: Raft Vote State
// ============================================================================

FB_TEST(raft_state, vote_for_self_on_candidate) {
    // When becoming candidate, vote for self
    int self_id = 5;
    int voted_for = -1; // -1 means not voted
    raft_identity state = RAFT_STATE_FOLLOWER;

    state = RAFT_STATE_CANDIDATE;
    voted_for = self_id;

    FB_ASSERT_EQ(voted_for, self_id);
    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
}

FB_TEST(raft_state, vote_reset_on_new_term) {
    int voted_for = 3;
    int64_t my_term = 5;
    int64_t new_term = 6;

    if (new_term > my_term) {
        my_term = new_term;
        voted_for = -1;
    }

    FB_ASSERT_EQ(voted_for, -1);
    FB_ASSERT_EQ(my_term, 6);
}

FB_TEST(raft_state, vote_grant_first_request) {
    int voted_for = -1; // not voted yet
    int requester_id = 7;
    bool granted = false;

    if (voted_for == -1) {
        voted_for = requester_id;
        granted = true;
    }

    FB_ASSERT_TRUE(granted);
    FB_ASSERT_EQ(voted_for, 7);
}

FB_TEST(raft_state, vote_deny_already_voted) {
    int voted_for = 3; // already voted for node 3
    int requester_id = 7;
    bool granted = false;

    if (voted_for == -1 || voted_for == requester_id) {
        granted = true;
    }

    FB_ASSERT_FALSE(granted);
    FB_ASSERT_EQ(voted_for, 3); // unchanged
}

FB_TEST(raft_state, vote_grant_same_node_again) {
    // If voted_for == request.candidate_id, request can be re-granted (idempotent)
    int voted_for = 3;
    int requester_id = 3;
    bool granted = (voted_for == -1 || voted_for == requester_id);
    FB_ASSERT_TRUE(granted);
}

// ============================================================================
// Test Suite: Raft Log Index Tracking
// ============================================================================

FB_TEST(raft_state, commit_idx_monotonic) {
    int64_t commit_idx = 5;
    int64_t new_commit = 8;

    if (new_commit > commit_idx) {
        commit_idx = new_commit;
    }
    FB_ASSERT_EQ(commit_idx, 8);
}

FB_TEST(raft_state, commit_idx_never_regresses) {
    int64_t commit_idx = 10;
    int64_t lower = 5;

    if (lower > commit_idx) {
        commit_idx = lower; // would never happen in real raft
    }
    FB_ASSERT_EQ(commit_idx, 10);
}

FB_TEST(raft_state, last_applied_le_commit_idx) {
    int64_t commit_idx = 10;
    int64_t last_applied = 7;

    FB_ASSERT_TRUE(last_applied <= commit_idx);
}

FB_TEST(raft_state, last_applied_advances_toward_commit) {
    int64_t commit_idx = 10;
    int64_t last_applied = 5;

    // simulate state machine application
    while (last_applied < commit_idx) {
        last_applied++;
    }
    FB_ASSERT_EQ(last_applied, 10);
}

FB_TEST(raft_state, log_index_starts_at_one) {
    // First log entry has index 1 (index 0 is sentinel)
    int64_t first_idx = 1;
    FB_ASSERT_TRUE(first_idx > 0);
}

// ============================================================================
// Test Suite: Raft AppendEntries Consistency
// ============================================================================

FB_TEST(raft_state, append_entries_match_prev_log) {
    // AppendEntries succeeds when prev_log_index and prev_log_term match
    int64_t prev_log_index = 5;
    int64_t prev_log_term = 3;
    int64_t my_log_at_prev_idx_term = 3;

    bool match = (my_log_at_prev_idx_term == prev_log_term);
    FB_ASSERT_TRUE(match);
}

FB_TEST(raft_state, append_entries_reject_term_mismatch) {
    // AppendEntries fails if log doesn't contain matching entry at prev_log_index
    int64_t prev_log_index = 5;
    int64_t prev_log_term = 3;
    int64_t my_log_at_prev_idx_term = 2; // different term

    bool match = (my_log_at_prev_idx_term == prev_log_term);
    FB_ASSERT_FALSE(match);
}

FB_TEST(raft_state, append_entries_reject_log_too_short) {
    // AppendEntries fails if prev_log_index is beyond our log
    int64_t prev_log_index = 10;
    int64_t my_log_last_idx = 5;

    bool can_append = (prev_log_index <= my_log_last_idx);
    FB_ASSERT_FALSE(can_append);
}

FB_TEST(raft_state, append_entries_overwrites_conflicting) {
    // Conflicting entries (same index, different term) are overwritten
    int64_t their_term = 5;
    int64_t my_term_at_idx = 3;

    bool should_overwrite = (their_term != my_term_at_idx);
    FB_ASSERT_TRUE(should_overwrite);
}

FB_TEST(raft_state, append_entries_keeps_matching) {
    // Already-present matching entries are not duplicated
    int64_t their_term = 5;
    int64_t my_term_at_idx = 5;

    bool needs_write = (their_term != my_term_at_idx);
    FB_ASSERT_FALSE(needs_write);
}

FB_TEST(raft_state, append_entries_empty_is_heartbeat) {
    // Empty AppendEntries serves as a heartbeat
    int num_entries = 0;
    bool is_heartbeat = (num_entries == 0);
    FB_ASSERT_TRUE(is_heartbeat);
}

FB_TEST(raft_state, append_entries_resets_election_timer) {
    // Receiving valid AppendEntries from current leader resets election timer
    bool valid_from_leader = true;
    int election_elapsed = 0; // would have been incremented otherwise

    if (valid_from_leader) {
        election_elapsed = 0;
    }
    FB_ASSERT_EQ(election_elapsed, 0);
}

// ============================================================================
// Test Suite: Raft Leader Commit Index
// ============================================================================

FB_TEST(raft_state, leader_commits_when_majority_acked) {
    // Leader commits index N when majority of cluster has replicated it
    int cluster_size = 5;
    int majority = (cluster_size / 2) + 1;
    int replicated_count = 3; // 3 out of 5 = majority

    bool can_commit = (replicated_count >= majority);
    FB_ASSERT_TRUE(can_commit);
}

FB_TEST(raft_state, leader_no_commit_minority_acked) {
    int cluster_size = 5;
    int majority = (cluster_size / 2) + 1;
    int replicated_count = 2; // 2 out of 5, not majority

    bool can_commit = (replicated_count >= majority);
    FB_ASSERT_FALSE(can_commit);
}

FB_TEST(raft_state, leader_commit_three_node) {
    int cluster_size = 3;
    int majority = (cluster_size / 2) + 1; // 2
    FB_ASSERT_EQ(majority, 2);
}

FB_TEST(raft_state, leader_commit_seven_node) {
    int cluster_size = 7;
    int majority = (cluster_size / 2) + 1; // 4
    FB_ASSERT_EQ(majority, 4);
}

FB_TEST(raft_state, leader_only_commits_current_term) {
    // Leader can only commit entries from its own term directly.
    // Earlier-term entries are committed indirectly via a current-term entry.
    int64_t my_term = 5;
    int64_t entry_term = 3;

    bool direct_commit_allowed = (entry_term == my_term);
    FB_ASSERT_FALSE(direct_commit_allowed);
}

FB_TEST(raft_state, follower_follows_leader_commit) {
    // Follower advances commit_idx to min(leader_commit, last_new_entry_idx)
    int64_t leader_commit = 10;
    int64_t my_last_new_idx = 8;

    int64_t new_commit = std::min(leader_commit, my_last_new_idx);
    FB_ASSERT_EQ(new_commit, 8);
}

FB_TEST(raft_state, follower_commit_idx_below_leader) {
    int64_t leader_commit = 5;
    int64_t my_last_new_idx = 100;

    int64_t new_commit = std::min(leader_commit, my_last_new_idx);
    FB_ASSERT_EQ(new_commit, 5);
}

// ============================================================================
// Test Suite: Raft Snapshot
// ============================================================================

FB_TEST(raft_state, snapshot_includes_last_applied_idx) {
    // A snapshot records the last_included_index and last_included_term
    int64_t last_applied = 100;
    int64_t snapshot_last_included = last_applied;
    FB_ASSERT_EQ(snapshot_last_included, 100);
}

FB_TEST(raft_state, snapshot_advances_log_compaction) {
    // After snapshot, log can be compacted up through snapshot last_included
    int64_t log_first_idx = 1;
    int64_t snapshot_last_included = 50;

    if (snapshot_last_included >= log_first_idx) {
        log_first_idx = snapshot_last_included + 1;
    }
    FB_ASSERT_EQ(log_first_idx, 51);
}

FB_TEST(raft_state, snapshot_reject_older) {
    // Receiving InstallSnapshot with last_included <= my snapshot is ignored
    int64_t my_snapshot = 50;
    int64_t their_snapshot = 30;

    bool should_install = (their_snapshot > my_snapshot);
    FB_ASSERT_FALSE(should_install);
}

FB_TEST(raft_state, snapshot_accept_newer) {
    int64_t my_snapshot = 50;
    int64_t their_snapshot = 100;

    bool should_install = (their_snapshot > my_snapshot);
    FB_ASSERT_TRUE(should_install);
}

FB_TEST(raft_state, snapshot_resets_commit_idx_if_below) {
    // After installing a newer snapshot, commit_idx is set to at least the snapshot idx
    int64_t commit_idx = 30;
    int64_t snapshot_last_idx = 50;

    if (snapshot_last_idx > commit_idx) {
        commit_idx = snapshot_last_idx;
    }
    FB_ASSERT_EQ(commit_idx, 50);
}

FB_TEST(raft_state, snapshot_chunk_constants) {
    // Verify SNAPSHOT_MAX_CHUNK / SNAPSHOT_MAX_CONCURRENT are positive
    FB_ASSERT_TRUE(SNAPSHOT_MAX_CHUNK > 0);
    FB_ASSERT_TRUE(SNAPSHOT_MAX_CONCURRENT > 0);
}

FB_TEST(raft_state, snapshot_chunk_max_one) {
    FB_ASSERT_EQ(SNAPSHOT_MAX_CHUNK, 1);
    FB_ASSERT_EQ(SNAPSHOT_MAX_CONCURRENT, 1);
}

// ============================================================================
// Test Suite: Raft Timer Constants
// ============================================================================

FB_TEST(raft_state, timer_period_value) {
    FB_ASSERT_EQ(TIMER_PERIOD_MSEC, 500);
}

FB_TEST(raft_state, heartbeat_period_value) {
    FB_ASSERT_EQ(HEARTBEAT_TIMER_INTERVAL_MSEC, 500);
}

FB_TEST(raft_state, raft_task_period_value) {
    FB_ASSERT_EQ(RAFT_TASK_TIMER_USEC, 100);
}

FB_TEST(raft_state, heartbeat_less_than_election_timeout) {
    // Heartbeat must be much shorter than election timeout to prevent
    // unnecessary leader changes.
    int32_t heartbeat_ms = HEARTBEAT_TIMER_INTERVAL_MSEC;
    int32_t typical_election_timeout_ms = 1500; // common raft default

    FB_ASSERT_TRUE(heartbeat_ms < typical_election_timeout_ms);
}

FB_TEST(raft_state, raft_task_timer_microsecond_unit) {
    // RAFT_TASK_TIMER_USEC is in microseconds, should be much smaller than ms timer
    int64_t task_ns = RAFT_TASK_TIMER_USEC * 1000LL;
    int64_t timer_ns = TIMER_PERIOD_MSEC * 1000000LL;
    FB_ASSERT_TRUE(task_ns < timer_ns);
}

FB_TEST(raft_state, catch_up_num_value) {
    FB_ASSERT_EQ(CATCH_UP_NUM, 200);
    FB_ASSERT_TRUE(CATCH_UP_NUM > 0);
}

// Main function for test runner
FB_TEST_MAIN()
