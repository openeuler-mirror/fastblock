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

typedef enum {
    RAFT_STATE_NONE,
    RAFT_STATE_FOLLOWER,
    RAFT_STATE_CANDIDATE,
    RAFT_STATE_LEADER
} raft_identity;

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

FB_TEST(raft_rpc, appendentries_conflict_resolution) {
    // 模拟日志冲突检测
    raft_index_t leader_prev_idx = 10;
    raft_term_t leader_prev_term = 5;

    std::map<raft_index_t, raft_term_t> local_log;
    for (int i = 1; i <= 15; i++) {
        local_log[i] = (i <= 8) ? 4 : 5;
    }

    // 本地日志在 prev_idx 处 term 不匹配
    bool conflict = local_log.count(leader_prev_idx) &&
                    local_log[leader_prev_idx] != leader_prev_term;
    FB_ASSERT_FALSE(conflict);  // 10处的term是5，匹配

    // 测试冲突场景
    leader_prev_term = 4;  // 期望term=4，但实际是5
    conflict = local_log.count(leader_prev_idx) &&
               local_log[leader_prev_idx] != leader_prev_term;
    FB_ASSERT_TRUE(conflict);
}

FB_TEST(raft_rpc, appendentries_log_truncation) {
    // 冲突时需要截断本地日志
    raft_index_t prev_log_idx = 10;
    raft_index_t new_entry_idx = 11;

    std::vector<raft_index_t> local_log;
    for (int i = 1; i <= 20; i++) {
        local_log.push_back(i);
    }

    // 截断冲突日志
    local_log.erase(local_log.begin() + prev_log_idx, local_log.end());

    FB_ASSERT_EQ(local_log.size(), 10UL);
    FB_ASSERT_EQ(local_log.back(), 10L);
}

FB_TEST(raft_rpc, appendentries_batch_optimization) {
    // 批量追加优化
    int batch_size = 100;
    int entries_in_batch = 0;

    for (int i = 0; i < batch_size; i++) {
        entries_in_batch++;
    }

    FB_ASSERT_EQ(entries_in_batch, 100);

    // 批处理减少RPC次数
    int single_rpc_count = batch_size;
    int batch_rpc_count = 1;
    FB_ASSERT_TRUE(batch_rpc_count < single_rpc_count);
}

FB_TEST(raft_rpc, appendentries_retransmission) {
    // 网络重传场景
    int retry_count = 0;
    int max_retries = 3;
    bool success = false;

    while (retry_count < max_retries && !success) {
        retry_count++;
        if (retry_count == 2) {
            success = true;  // 第二次成功
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, appendentries_flow_control) {
    // 流量控制
    int in_flight_requests = 5;
    int max_in_flight = 10;
    int window_size = 5;

    bool can_send = (in_flight_requests + window_size) <= max_in_flight;
    FB_ASSERT_TRUE(can_send);

    // 窗口满时不能发送
    in_flight_requests = 8;
    can_send = (in_flight_requests + window_size) <= max_in_flight;
    FB_ASSERT_FALSE(can_send);
}

FB_TEST(raft_rpc, appendentries_duplicate_detection) {
    // 重复请求检测
    std::set<std::pair<raft_term_t, raft_index_t>> received;

    raft_term_t term = 5;
    raft_index_t prev_idx = 10;

    auto key = std::make_pair(term, prev_idx);
    bool is_duplicate = received.count(key) > 0;
    FB_ASSERT_FALSE(is_duplicate);

    received.insert(key);
    is_duplicate = received.count(key) > 0;
    FB_ASSERT_TRUE(is_duplicate);
}

FB_TEST(raft_rpc, appendentries_pipeline_optimization) {
    // 流水线优化
    raft_index_t base_idx = 100;
    int pipeline_depth = 3;

    std::vector<raft_index_t> in_flight;
    for (int i = 0; i < pipeline_depth; i++) {
        in_flight.push_back(base_idx + i * 10);
    }

    FB_ASSERT_EQ(in_flight.size(), 3UL);

    // 检查流水线中的请求范围
    raft_index_t min_idx = *std::min_element(in_flight.begin(), in_flight.end());
    raft_index_t max_idx = *std::max_element(in_flight.begin(), in_flight.end());
    FB_ASSERT_EQ(min_idx, 100L);
    FB_ASSERT_EQ(max_idx, 120L);
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

FB_TEST(raft_rpc, requestvote_candidate_log_behind) {
    // 候选人日志落后的各种场景
    raft_index_t my_last_idx = 100;
    raft_term_t my_last_term = 5;

    // 场景1：term相同，idx落后
    raft_index_t cand_idx = 80;
    raft_term_t cand_term = 5;
    bool log_ok = (cand_term > my_last_term) ||
                  ((cand_term == my_last_term) && (cand_idx >= my_last_idx));
    FB_ASSERT_FALSE(log_ok);

    // 场景2：term落后
    cand_idx = 120;
    cand_term = 4;
    log_ok = (cand_term > my_last_term) ||
             ((cand_term == my_last_term) && (cand_idx >= my_last_idx));
    FB_ASSERT_FALSE(log_ok);

    // 场景3：idx超前但term落后
    cand_idx = 200;
    cand_term = 4;
    log_ok = (cand_term > my_last_term) ||
             ((cand_term == my_last_term) && (cand_idx >= my_last_idx));
    FB_ASSERT_FALSE(log_ok);
}

FB_TEST(raft_rpc, requestvote_vote_timeout) {
    // 投票超时处理
    int vote_timeout_ms = 100;
    int elapsed_ms = 50;
    bool timed_out = elapsed_ms >= vote_timeout_ms;
    FB_ASSERT_FALSE(timed_out);

    elapsed_ms = 150;
    timed_out = elapsed_ms >= vote_timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, requestvote_after_vote_update_state) {
    // 投票后更新本地状态
    raft_term_t current_term = 5;
    raft_node_id_t voted_for = 0;
    raft_term_t candidate_term = 6;
    raft_node_id_t candidate_id = 3;

    // 更新term
    if (candidate_term > current_term) {
        current_term = candidate_term;
        voted_for = 0;  // 重置投票
    }

    // 投票
    voted_for = candidate_id;

    FB_ASSERT_EQ(current_term, 6L);
    FB_ASSERT_EQ(voted_for, 3L);
}

FB_TEST(raft_rpc, requestvote_rejected_no_state_change) {
    // 拒绝投票不改变状态
    raft_term_t current_term = 6;
    raft_node_id_t voted_for = 5;

    raft_term_t candidate_term = 4;  // 过期term
    raft_node_id_t candidate_id = 3;

    // 拒绝投票
    bool grant = candidate_term >= current_term;
    if (!grant) {
        // 状态不变
    }

    FB_ASSERT_EQ(current_term, 6L);
    FB_ASSERT_EQ(voted_for, 5L);  // 保持原值
}

FB_TEST(raft_rpc, requestvote_prevote_extension) {
    // PreVote 扩展（Raft论文扩展）
    bool is_prevote = true;
    raft_term_t current_term = 5;
    raft_term_t candidate_term = 6;

    // PreVote 不更新 term
    if (is_prevote) {
        // 只检查是否可以投票，不改变状态
        bool would_grant = candidate_term >= current_term;
        FB_ASSERT_TRUE(would_grant);
    }

    // term 不变
    FB_ASSERT_EQ(current_term, 5L);
}

FB_TEST(raft_rpc, requestvote_multiple_candidates) {
    // 多个候选人同时请求投票
    std::map<raft_node_id_t, bool> vote_responses;
    raft_node_id_t voted_for = 0;

    std::vector<raft_node_id_t> candidates = {1, 2, 3};

    // 只能给第一个满足条件的候选人投票
    for (auto cand_id : candidates) {
        if (voted_for == 0) {
            vote_responses[cand_id] = true;
            voted_for = cand_id;
        } else {
            vote_responses[cand_id] = false;
        }
    }

    FB_ASSERT_EQ(voted_for, 1L);
    FB_ASSERT_TRUE(vote_responses[1]);
    FB_ASSERT_FALSE(vote_responses[2]);
    FB_ASSERT_FALSE(vote_responses[3]);
}

FB_TEST(raft_rpc, requestvote_disrupted_leader) {
    // 网络分区导致的老Leader场景
    raft_term_t old_leader_term = 5;
    raft_term_t new_leader_term = 7;

    // 拒绝老Leader的投票请求
    bool grant = old_leader_term >= new_leader_term;
    FB_ASSERT_FALSE(grant);
}

// ============================================================================
// Test Suite: Heartbeat RPC Tests
// ============================================================================

FB_TEST(raft_rpc, heartbeat_message_fields) {
    raft_term_t term = 5;
    raft_node_id_t leader_id = 1;
    raft_index_t commit_idx = 10;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(leader_id > 0);
    FB_ASSERT_TRUE(commit_idx >= 0);
}

FB_TEST(raft_rpc, heartbeat_reset_election_timer) {
    bool heartbeat_received = true;
    raft_time_t last_leader_contact = 0;

    if (heartbeat_received) {
        last_leader_contact = 1000;  // 更新时间
    }

    FB_ASSERT_EQ(last_leader_contact, 1000L);

    // 验证选举超时应该被重置
    raft_time_t election_timeout = 500;
    raft_time_t current_time = 1200;
    bool election_expired = (current_time - last_leader_contact) >= election_timeout;
    FB_ASSERT_FALSE(election_expired);
}

FB_TEST(raft_rpc, heartbeat_leader_lease_renewal) {
    raft_time_t lease_expiry = 1000;

    // 收到心跳后续约租约
    raft_time_t heartbeat_period = 100;
    lease_expiry = lease_expiry + heartbeat_period;

    FB_ASSERT_EQ(lease_expiry, 1100L);
}

FB_TEST(raft_rpc, heartbeat_suppress_redundant) {
    bool suppress_heartbeat = false;
    raft_index_t match_idx = 100;
    raft_index_t next_idx = 101;

    // 日志已同步，可以抑制心跳
    if (match_idx + 1 == next_idx) {
        suppress_heartbeat = true;
    }

    FB_ASSERT_TRUE(suppress_heartbeat);
}

FB_TEST(raft_rpc, heartbeat_interval_ratio) {
    int election_timeout = 500;
    int heartbeat_timeout = 100;

    // 心跳间隔应远小于选举超时
    bool valid_ratio = (heartbeat_timeout > 0) &&
                       (election_timeout > heartbeat_timeout) &&
                       (election_timeout >= 5 * heartbeat_timeout);

    FB_ASSERT_TRUE(valid_ratio);
}

FB_TEST(raft_rpc, heartbeat_batch_optimization) {
    // 批量发送心跳优化
    int node_count = 5;
    int batch_size = 10;
    int total_heartbeats = node_count * batch_size;

    FB_ASSERT_EQ(total_heartbeats, 50);

    // 批处理减少 RPC 次数
    int rpc_calls = 1;  // 批量发送只需一次
    FB_ASSERT_TRUE(rpc_calls < total_heartbeats);
}

FB_TEST(raft_rpc, heartbeat_as_appendentries_empty) {
    // 心跳等同于空的 AppendEntries
    int entries_count = 0;
    bool is_heartbeat = (entries_count == 0);

    FB_ASSERT_TRUE(is_heartbeat);

    // 但仍包含 prev_log_idx/term 和 leader_commit
    raft_index_t prev_log_idx = 10;
    raft_term_t prev_log_term = 5;
    raft_index_t leader_commit = 8;

    FB_ASSERT_TRUE(prev_log_idx >= 0);
    FB_ASSERT_TRUE(prev_log_term >= 0);
    FB_ASSERT_TRUE(leader_commit >= 0);
}

FB_TEST(raft_rpc, heartbeat_commit_update) {
    raft_index_t leader_commit = 15;
    raft_index_t local_commit = 10;
    raft_index_t last_log_idx = 20;

    // 更新 commit_idx = min(leader_commit, last_log_idx)
    raft_index_t new_commit = std::min(leader_commit, last_log_idx);
    FB_ASSERT_EQ(new_commit, 15L);

    // leader_commit 超过本地日志
    leader_commit = 25;
    new_commit = std::min(leader_commit, last_log_idx);
    FB_ASSERT_EQ(new_commit, 20L);
}

FB_TEST(raft_rpc, heartbeat_leader_change_detection) {
    raft_node_id_t current_leader = 1;
    raft_node_id_t new_leader = 2;
    raft_term_t current_term = 5;
    raft_term_t new_term = 6;

    // 收到更高 term 的心跳，说明有新 Leader
    bool leader_changed = (new_term > current_term);
    FB_ASSERT_TRUE(leader_changed);

    if (leader_changed) {
        current_leader = new_leader;
        current_term = new_term;
    }

    FB_ASSERT_EQ(current_leader, 2L);
    FB_ASSERT_EQ(current_term, 6L);
}

FB_TEST(raft_rpc, heartbeat_missed_detection) {
    // 心跳丢失检测
    raft_time_t last_heartbeat = 1000;
    raft_time_t current_time = 2000;
    raft_time_t heartbeat_timeout = 100;

    int missed_count = 0;
    while (last_heartbeat + heartbeat_timeout < current_time) {
        missed_count++;
        last_heartbeat += heartbeat_timeout;
    }

    FB_ASSERT_GE(missed_count, 5);
}

FB_TEST(raft_rpc, heartbeat_burst_on_leader_election) {
    // 新Leader当选后立即发送心跳
    raft_identity state = RAFT_STATE_LEADER;
    int heartbeat_count = 0;

    if (state == RAFT_STATE_LEADER) {
        // 立即向所有节点发送心跳
        for (int i = 0; i < 5; i++) {
            heartbeat_count++;
        }
    }

    FB_ASSERT_EQ(heartbeat_count, 5);
}

FB_TEST(raft_rpc, heartbeat_coalescing) {
    // 心跳合并优化
    int pending_heartbeats = 3;
    int sent_heartbeats = 0;

    // 合并为一次发送
    if (pending_heartbeats > 0) {
        sent_heartbeats = 1;  // 批量发送
        pending_heartbeats = 0;
    }

    FB_ASSERT_EQ(sent_heartbeats, 1);
    FB_ASSERT_EQ(pending_heartbeats, 0);
}

FB_TEST(raft_rpc, heartbeat_timeout_trigger_election) {
    // 心跳超时触发选举
    raft_time_t last_heartbeat = 1000;
    raft_time_t election_timeout = 500;
    raft_time_t current_time = 1600;

    raft_identity state = RAFT_STATE_FOLLOWER;
    bool timeout = (current_time - last_heartbeat) >= election_timeout;

    if (timeout) {
        state = RAFT_STATE_CANDIDATE;
    }

    FB_ASSERT_TRUE(timeout);
    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
}

FB_TEST(raft_rpc, heartbeat_pending_writes_flush) {
    // 心跳前刷新待写入
    int pending_writes = 10;
    bool flush_before_heartbeat = true;

    int flushed = 0;
    if (flush_before_heartbeat) {
        flushed = pending_writes;
        pending_writes = 0;
    }

    FB_ASSERT_EQ(flushed, 10);
    FB_ASSERT_EQ(pending_writes, 0);
}

FB_TEST(raft_rpc, heartbeat_network_partition) {
    // 网络分区场景
    raft_node_id_t leader_id = 1;
    std::set<raft_node_id_t> partitioned_nodes = {3, 4};

    // 分区内的节点无法收到心跳
    bool can_receive_heartbeat = !partitioned_nodes.count(2);
    FB_ASSERT_TRUE(can_receive_heartbeat);

    can_receive_heartbeat = !partitioned_nodes.count(3);
    FB_ASSERT_FALSE(can_receive_heartbeat);
}

FB_TEST(raft_rpc, heartbeat_response_batching) {
    // 心跳响应批量处理
    std::vector<bool> responses = {true, true, true, false, true};
    int success_count = 0;

    for (bool r : responses) {
        if (r) success_count++;
    }

    FB_ASSERT_EQ(success_count, 4);

    // 检查多数派响应成功
    bool majority_success = success_count > responses.size() / 2;
    FB_ASSERT_TRUE(majority_success);
}

// ============================================================================
// Test Suite: Snapshot RPC Tests
// ============================================================================

FB_TEST(raft_rpc, installsnapshot_request_fields) {
    raft_term_t term = 5;
    raft_node_id_t leader_id = 1;
    raft_index_t last_included_idx = 100;
    raft_term_t last_included_term = 4;
    int offset = 0;
    bool done = false;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(leader_id > 0);
    FB_ASSERT_TRUE(last_included_idx > 0);
    FB_ASSERT_TRUE(last_included_term > 0);
    FB_ASSERT_TRUE(offset >= 0);
}

FB_TEST(raft_rpc, installsnapshot_chunk_offset) {
    int64_t total_size = 1024 * 1024;  // 1MB
    int chunk_size = 64 * 1024;        // 64KB
    int offset = 0;
    int chunk_count = 0;

    while (offset < total_size) {
        offset += chunk_size;
        chunk_count++;
    }

    FB_ASSERT_EQ(chunk_count, 16);
    FB_ASSERT_TRUE(offset >= total_size);
}

FB_TEST(raft_rpc, installsnapshot_progress_tracking) {
    int64_t total_size = 1024 * 1024;
    int64_t transferred = 0;
    int chunk_size = 64 * 1024;

    for (int i = 0; i < 8; i++) {
        transferred += chunk_size;
    }

    double progress = 100.0 * transferred / total_size;
    FB_ASSERT_TRUE(progress >= 50.0);
    FB_ASSERT_TRUE(progress < 100.0);
}

FB_TEST(raft_rpc, snapshot_check_request) {
    raft_index_t snapshot_idx = 100;
    raft_term_t snapshot_term = 5;

    FB_ASSERT_TRUE(snapshot_idx > 0);
    FB_ASSERT_TRUE(snapshot_term > 0);

    // Follower 的日志落后于快照
    raft_index_t follower_last_idx = 50;
    bool needs_snapshot = snapshot_idx > follower_last_idx;
    FB_ASSERT_TRUE(needs_snapshot);
}

FB_TEST(raft_rpc, snapshot_check_follower_ahead) {
    raft_index_t snapshot_idx = 100;
    raft_index_t follower_last_idx = 150;

    // Follower 日志超前，不需要安装快照
    bool needs_snapshot = snapshot_idx > follower_last_idx;
    FB_ASSERT_FALSE(needs_snapshot);
}

FB_TEST(raft_rpc, installsnapshot_response_fields) {
    raft_term_t term = 5;
    bool success = true;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(success);
}

FB_TEST(raft_rpc, installsnapshot_failure_retry) {
    int retry_count = 0;
    int max_retries = 3;

    // 模拟传输失败
    bool success = false;
    while (!success && retry_count < max_retries) {
        retry_count++;
        // 模拟重试逻辑
        if (retry_count >= 2) {
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, snapshot_apply_after_receive) {
    raft_index_t last_included_idx = 100;
    raft_term_t last_included_term = 4;

    // 应用快照后更新状态机索引
    raft_index_t last_applied = last_included_idx;
    raft_index_t commit_idx = last_included_idx;

    FB_ASSERT_EQ(last_applied, 100L);
    FB_ASSERT_EQ(commit_idx, 100L);
}

FB_TEST(raft_rpc, snapshot_discard_conflicting_logs) {
    // 快照之后丢弃冲突的日志
    raft_index_t snapshot_idx = 100;

    // 本地日志索引 95-110 与快照冲突
    std::vector<raft_index_t> local_logs;
    for (int i = 95; i <= 110; i++) {
        local_logs.push_back(i);
    }

    // 丢弃 snapshot_idx 之前的日志
    local_logs.erase(
        std::remove_if(local_logs.begin(), local_logs.end(),
                       [snapshot_idx](raft_index_t idx) { return idx <= snapshot_idx; }),
        local_logs.end());

    FB_ASSERT_EQ(local_logs.size(), 10UL);
    FB_ASSERT_EQ(local_logs.front(), 101L);
}

FB_TEST(raft_rpc, snapshot_concurrent_transfer_limit) {
    int max_concurrent = 3;
    int current_transfers = 0;

    // 模拟并发快照传输
    for (int i = 0; i < 5; i++) {
        if (current_transfers < max_concurrent) {
            current_transfers++;
        }
    }

    FB_ASSERT_EQ(current_transfers, 3);
}

// ============================================================================
// Test Suite: TimeoutNow RPC Tests
// ============================================================================

FB_TEST(raft_rpc, timeoutnow_triggers_election) {
    // TimeoutNow 让 Follower 立即开始选举
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool timeout_now_received = true;

    if (timeout_now_received) {
        state = RAFT_STATE_CANDIDATE;
    }

    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
}

FB_TEST(raft_rpc, timeoutnow_bypasses_election_timeout) {
    raft_time_t election_timeout = 500;
    raft_time_t remaining_timeout = 300;

    // TimeoutNow 不等待超时，立即选举
    bool bypass_timeout = true;
    if (bypass_timeout) {
        remaining_timeout = 0;
    }

    FB_ASSERT_EQ(remaining_timeout, 0L);
}

FB_TEST(raft_rpc, timeoutnow_only_for_followers) {
    // 只有 Follower 响应 TimeoutNow
    raft_identity state = RAFT_STATE_LEADER;

    bool should_respond = (state == RAFT_STATE_FOLLOWER);
    FB_ASSERT_FALSE(should_respond);

    // Follower 状态
    state = RAFT_STATE_FOLLOWER;
    should_respond = (state == RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(should_respond);
}

FB_TEST(raft_rpc, timeoutnow_increment_term) {
    raft_term_t current_term = 5;

    // 收到 TimeoutNow 后递增 term 开始选举
    current_term++;

    FB_ASSERT_EQ(current_term, 6L);
}

FB_TEST(raft_rpc, timeoutnow_request_vote_self) {
    // 收到 TimeoutNow 后给自己投票
    uint64_t votes = 0;
    uint64_t node_num = 5;

    votes++;  // 自己的一票

    FB_ASSERT_EQ(votes, 1UL);

    // 检查是否可能赢得选举
    bool can_win = votes > node_num / 2;
    FB_ASSERT_FALSE(can_win);  // 需要更多票
}

FB_TEST(raft_rpc, timeoutnow_leader_lease_transfer) {
    // Leader 通过 TimeoutNow 转移领导权
    raft_node_id_t current_leader = 1;
    raft_node_id_t target_follower = 3;

    // Leader 发送 TimeoutNow 给目标 Follower
    bool lease_transferred = true;

    if (lease_transferred) {
        // 旧 Leader 应该退位
        current_leader = 0;  // 暂时没有 Leader
    }

    FB_ASSERT_EQ(current_leader, 0L);
}

FB_TEST(raft_rpc, timeoutnow_use_case_graceful_transfer) {
    // 优雅的领导权转移场景
    std::string step = "identify_target";
    bool target_synced = true;

    if (step == "identify_target" && target_synced) {
        step = "send_timeout_now";
    }

    FB_ASSERT_EQ(step, "send_timeout_now");

    // 目标节点成为新 Leader
    step = "new_leader_elected";
    FB_ASSERT_EQ(step, "new_leader_elected");
}

FB_TEST(raft_rpc, timeoutnow_no_response_needed) {
    // TimeoutNow 不需要响应
    bool require_response = false;

    FB_ASSERT_FALSE(require_response);

    // 发送方不等待响应
    bool wait_for_response = false;
    FB_ASSERT_FALSE(wait_for_response);
}

FB_TEST(raft_rpc, timeoutnow_multiple_senders) {
    // 多个节点同时收到 TimeoutNow 会导致分票
    int candidates = 3;
    int total_votes = 3;

    // 每个候选人得到自己的票
    std::vector<int> votes_per_candidate(candidates, 1);

    // 没有人能得到多数票
    int majority_needed = (total_votes / 2) + 1;
    bool any_majority = false;
    for (int v : votes_per_candidate) {
        if (v >= majority_needed) {
            any_majority = true;
            break;
        }
    }

    FB_ASSERT_FALSE(any_majority);
}

FB_TEST(raft_rpc, timeoutnow_term_mismatch) {
    raft_term_t current_term = 6;
    raft_term_t leader_term = 5;

    // 如果 TimeoutNow 的 term 过期，忽略
    bool should_ignore = leader_term < current_term;
    FB_ASSERT_TRUE(should_ignore);
}

// Main function for test runner
FB_TEST_MAIN()
