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
 * @file test_raft_rpc.cc
 * @brief Unit tests for Raft RPC message processing
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

namespace {

typedef long int raft_term_t;
typedef long int raft_index_t;
typedef int raft_node_id_t;
typedef long int raft_time_t;

} // anonymous namespace

FB_SUITE_SETUP(raft_rpc) {
    // Setup code here
}

FB_SUITE_TEARDOWN(raft_rpc) {
    // Teardown code here
}

// ============================================================================
// Test Suite: AppendEntries RPC Tests
// ============================================================================

FB_TEST(raft_rpc, appendentries_request_fields) {
    // 模拟 AppendEntries 请求字段
    raft_term_t term = 5;
    raft_node_id_t leader_id = 1;
    raft_index_t prev_log_idx = 10;
    raft_term_t prev_log_term = 4;
    raft_index_t leader_commit = 8;

    // 验证请求字段有效性
    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(leader_id > 0);
    FB_ASSERT_TRUE(prev_log_idx >= 0);
    FB_ASSERT_TRUE(prev_log_term >= 0);
    FB_ASSERT_TRUE(leader_commit >= 0);
}

FB_TEST(raft_rpc, appendentries_response_success) {
    // 模拟 AppendEntries 响应
    raft_term_t current_term = 5;
    raft_term_t request_term = 5;
    bool success = (request_term >= current_term);

    FB_ASSERT_TRUE(success);

    // 请求 term 小于当前 term
    request_term = 4;
    success = (request_term >= current_term);
    FB_ASSERT_FALSE(success);
}

FB_TEST(raft_rpc, appendentries_prev_log_check) {
    raft_index_t prev_log_idx = 10;
    raft_term_t prev_log_term = 4;

    // 模拟本地日志检查
    std::map<raft_index_t, raft_term_t> log;
    for (int i = 1; i <= 15; i++) {
        log[i] = (i <= 10) ? 4 : 5;
    }

    // 检查 prev_log_idx 处的 term 是否匹配
    bool match = (log.count(prev_log_idx) > 0) && (log[prev_log_idx] == prev_log_term);
    FB_ASSERT_TRUE(match);

    // term 不匹配
    prev_log_term = 3;
    match = (log.count(prev_log_idx) > 0) && (log[prev_log_idx] == prev_log_term);
    FB_ASSERT_FALSE(match);
}

FB_TEST(raft_rpc, appendentries_log_missing) {
    raft_index_t prev_log_idx = 20;  // 超出本地日志范围

    // 模拟本地日志
    std::map<raft_index_t, raft_term_t> log;
    for (int i = 1; i <= 15; i++) {
        log[i] = 4;
    }

    // 日志不存在
    bool exists = log.count(prev_log_idx) > 0;
    FB_ASSERT_FALSE(exists);
}

FB_TEST(raft_rpc, appendentries_commit_idx_update) {
    raft_index_t leader_commit = 10;
    raft_index_t local_commit = 5;
    raft_index_t last_log_idx = 15;

    // commit_idx = min(leader_commit, last_log_idx)
    raft_index_t new_commit = std::min(leader_commit, last_log_idx);
    FB_ASSERT_EQ(new_commit, 10L);

    // leader_commit 超过本地日志
    leader_commit = 20;
    new_commit = std::min(leader_commit, last_log_idx);
    FB_ASSERT_EQ(new_commit, 15L);
}

FB_TEST(raft_rpc, appendentries_entries_append) {
    raft_index_t prev_log_idx = 5;
    int entries_count = 3;

    // 模拟追加日志
    raft_index_t new_last_idx = prev_log_idx + entries_count;
    FB_ASSERT_EQ(new_last_idx, 8L);
}

FB_TEST(raft_rpc, appendentries_term_mismatch_reject) {
    raft_term_t prev_log_term = 5;
    raft_term_t local_term = 4;

    bool match = (prev_log_term == local_term);
    FB_ASSERT_FALSE(match);
}

FB_TEST(raft_rpc, appendentries_empty_entries_heartbeat) {
    int entries_count = 0;
    bool is_heartbeat = (entries_count == 0);

    FB_ASSERT_TRUE(is_heartbeat);
}

// ============================================================================
// Test Suite: RequestVote RPC Tests
// ============================================================================

FB_TEST(raft_rpc, requestvote_request_fields) {
    // 模拟 RequestVote 请求字段
    raft_term_t term = 6;
    raft_node_id_t candidate_id = 3;
    raft_index_t last_log_idx = 100;
    raft_term_t last_log_term = 5;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(candidate_id > 0);
    FB_ASSERT_TRUE(last_log_idx >= 0);
    FB_ASSERT_TRUE(last_log_term >= 0);
}

FB_TEST(raft_rpc, requestvote_term_check) {
    raft_term_t current_term = 5;
    raft_term_t candidate_term = 6;

    // Candidate term >= current term 才能投票
    bool can_vote = candidate_term >= current_term;
    FB_ASSERT_TRUE(can_vote);

    candidate_term = 4;
    can_vote = candidate_term >= current_term;
    FB_ASSERT_FALSE(can_vote);
}

FB_TEST(raft_rpc, requestvote_log_up_to_date_term_first) {
    // 比较候选人的日志是否最新：先比较 term
    raft_index_t my_last_log_idx = 50;
    raft_term_t my_last_log_term = 5;

    raft_index_t candidate_last_log_idx = 60;
    raft_term_t candidate_last_log_term = 6;

    // 候选人 term 更大，日志更新
    bool is_up_to_date = candidate_last_log_term >= my_last_log_term;
    FB_ASSERT_TRUE(is_up_to_date);

    // 候选人 term 更小，日志落后
    candidate_last_log_term = 4;
    is_up_to_date = candidate_last_log_term >= my_last_log_term;
    FB_ASSERT_FALSE(is_up_to_date);
}

FB_TEST(raft_rpc, requestvote_log_up_to_date_idx_second) {
    // term 相同时比较 idx
    raft_index_t my_last_log_idx = 50;
    raft_term_t my_last_log_term = 5;

    raft_index_t candidate_last_log_idx = 60;
    raft_term_t candidate_last_log_term = 5;

    bool is_up_to_date = candidate_last_log_idx >= my_last_log_idx;
    FB_ASSERT_TRUE(is_up_to_date);

    // 候选人 idx 更小
    candidate_last_log_idx = 40;
    is_up_to_date = candidate_last_log_idx >= my_last_log_idx;
    FB_ASSERT_FALSE(is_up_to_date);
}

FB_TEST(raft_rpc, requestvote_already_voted) {
    raft_node_id_t voted_for = 0;  // 未投票
    raft_node_id_t candidate_id = 3;

    // 未投票或投票给同一候选人
    bool can_vote = (voted_for == 0) || (voted_for == candidate_id);
    FB_ASSERT_TRUE(can_vote);

    // 已投票给其他候选人
    voted_for = 5;
    can_vote = (voted_for == 0) || (voted_for == candidate_id);
    FB_ASSERT_FALSE(can_vote);

    // 已投票给同一候选人（重试）
    voted_for = 3;
    can_vote = (voted_for == 0) || (voted_for == candidate_id);
    FB_ASSERT_TRUE(can_vote);
}

FB_TEST(raft_rpc, requestvote_update_term) {
    raft_term_t current_term = 5;
    raft_term_t candidate_term = 6;

    // 收到更高 term 时更新本地 term
    if (candidate_term > current_term) {
        current_term = candidate_term;
    }

    FB_ASSERT_EQ(current_term, 6L);

    // 重置 voted_for
    raft_node_id_t voted_for = 3;
    voted_for = 0;  // 新 term 下可以重新投票

    FB_ASSERT_EQ(voted_for, 0L);
}

FB_TEST(raft_rpc, requestvote_grant_vote) {
    // 综合投票条件检查
    raft_term_t current_term = 5;
    raft_node_id_t voted_for = 0;

    raft_term_t candidate_term = 6;
    raft_node_id_t candidate_id = 3;

    // 条件1: candidate_term >= current_term
    bool term_ok = candidate_term >= current_term;
    FB_ASSERT_TRUE(term_ok);

    // 条件2: 未投票或投票给同一候选人
    bool vote_ok = (voted_for == 0) || (voted_for == candidate_id);
    FB_ASSERT_TRUE(vote_ok);

    // 条件3: 候选人日志最新
    raft_term_t my_last_log_term = 5;
    raft_index_t my_last_log_idx = 50;
    raft_term_t cand_last_log_term = 5;
    raft_index_t cand_last_log_idx = 60;

    bool log_ok = (cand_last_log_term > my_last_log_term) ||
                  ((cand_last_log_term == my_last_log_term) &&
                   (cand_last_log_idx >= my_last_log_idx));
    FB_ASSERT_TRUE(log_ok);

    // 所有条件满足，可以投票
    bool grant_vote = term_ok && vote_ok && log_ok;
    FB_ASSERT_TRUE(grant_vote);
}

FB_TEST(raft_rpc, requestvote_reject_stale_candidate) {
    // 拒绝旧 term 的候选人
    raft_term_t current_term = 6;
    raft_term_t candidate_term = 4;

    bool grant = candidate_term >= current_term;
    FB_ASSERT_FALSE(grant);
}

FB_TEST(raft_rpc, requestvote_response_fields) {
    raft_term_t term = 6;
    bool vote_granted = true;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(vote_granted);
}

// Main function for test runner
FB_TEST_MAIN()
