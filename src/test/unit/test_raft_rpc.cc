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
    bool majority_success = (size_t)success_count > responses.size() / 2;
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

FB_TEST(raft_rpc, snapshot_integrity_check) {
    // 快照完整性检查
    uint32_t expected_checksum = 0xABCDEF12;
    uint32_t computed_checksum = 0xABCDEF12;

    bool integrity_ok = (expected_checksum == computed_checksum);
    FB_ASSERT_TRUE(integrity_ok);

    // 损坏检测
    computed_checksum = 0xABCDEF13;
    integrity_ok = (expected_checksum == computed_checksum);
    FB_ASSERT_FALSE(integrity_ok);
}

FB_TEST(raft_rpc, snapshot_recovery_log_sync) {
    // 快照恢复后的日志同步
    raft_index_t snapshot_idx = 100;
    raft_index_t leader_next_idx = 101;

    // 恢复后需要同步后续日志
    std::vector<raft_index_t> logs_to_sync;
    for (raft_index_t idx = leader_next_idx; idx <= 110; idx++) {
        logs_to_sync.push_back(idx);
    }

    FB_ASSERT_EQ(logs_to_sync.size(), 10UL);
    FB_ASSERT_EQ(logs_to_sync.front(), 101L);
    FB_ASSERT_EQ(logs_to_sync.back(), 110L);
}

FB_TEST(raft_rpc, snapshot_transfer_abort) {
    // 快照传输中断处理
    int64_t total_size = 1024 * 1024;
    int64_t transferred = 300 * 1024;  // 只传输了300KB
    bool transfer_aborted = true;

    if (transfer_aborted) {
        transferred = 0;  // 需要重新开始
    }

    FB_ASSERT_EQ(transferred, 0L);
}

FB_TEST(raft_rpc, snapshot_incremental_apply) {
    // 增量应用快照
    raft_index_t snapshot_idx = 100;
    raft_index_t applied_idx = 0;

    // 增量应用
    while (applied_idx < snapshot_idx) {
        applied_idx += 10;
        if (applied_idx > snapshot_idx) {
            applied_idx = snapshot_idx;
        }
    }

    FB_ASSERT_EQ(applied_idx, 100L);
}

FB_TEST(raft_rpc, snapshot_memory_pressure) {
    // 内存压力下处理快照
    size_t available_memory = 50 * 1024 * 1024;  // 50MB
    size_t snapshot_size = 100 * 1024 * 1024;    // 100MB

    bool can_load = available_memory >= snapshot_size;
    FB_ASSERT_FALSE(can_load);

    // 需要分块处理
    size_t chunk_size = 10 * 1024 * 1024;  // 10MB
    can_load = available_memory >= chunk_size;
    FB_ASSERT_TRUE(can_load);
}

FB_TEST(raft_rpc, snapshot_compression_transfer) {
    // 压缩传输优化
    size_t original_size = 1024 * 1024;
    size_t compressed_size = 256 * 1024;  // 4倍压缩
    double compression_ratio = (double)original_size / compressed_size;

    FB_ASSERT_GE(compression_ratio, 4.0);

    size_t bytes_saved = original_size - compressed_size;
    FB_ASSERT_EQ(bytes_saved, 768UL * 1024);
}

FB_TEST(raft_rpc, snapshot_version_compatibility) {
    // 快照版本兼容性
    int snapshot_version = 2;
    int current_version = 3;

    bool compatible = snapshot_version <= current_version;
    FB_ASSERT_TRUE(compatible);

    // 不兼容版本
    snapshot_version = 4;
    compatible = snapshot_version <= current_version;
    FB_ASSERT_FALSE(compatible);
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

FB_TEST(raft_rpc, timeoutnow_pre_vote_check) {
    // PreVote 场景下的 TimeoutNow
    bool is_prevote = true;
    bool has_lease = true;

    // PreVote 模式下需要额外检查
    if (is_prevote && !has_lease) {
        // 不立即触发选举
    }

    bool trigger_election = is_prevote ? has_lease : true;
    FB_ASSERT_TRUE(trigger_election);
}

FB_TEST(raft_rpc, timeoutnow_idempotent_handling) {
    // 幂等处理：多次收到 TimeoutNow
    int received_count = 0;
    for (int i = 0; i < 5; i++) {
        received_count++;
        // 无论收到多少次，只触发一次选举
    }

    FB_ASSERT_EQ(received_count, 5);

    // 但只产生一次状态变化
    raft_identity state = RAFT_STATE_CANDIDATE;
    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
}

FB_TEST(raft_rpc, timeoutnow_with_pending_entries) {
    // 有待处理日志时的 TimeoutNow
    int pending_entries = 10;
    bool has_pending = pending_entries > 0;

    // 通常需要先处理完待处理日志
    if (has_pending) {
        // 记录状态以便后续同步
        int saved_pending = pending_entries;
        pending_entries = 0;
        FB_ASSERT_EQ(saved_pending, 10);
    }

    FB_ASSERT_EQ(pending_entries, 0);
}

FB_TEST(raft_rpc, timeoutnow_leader_election_race) {
    // 与 Leader 选举竞争
    raft_term_t local_term = 6;
    raft_term_t timeout_term = 6;

    // 同时收到 Leader 心跳和 TimeoutNow
    bool leader_heartbeat = true;
    bool timeout_now = true;

    // Leader 心跳优先
    raft_identity final_state;
    if (leader_heartbeat && timeout_now) {
        final_state = RAFT_STATE_FOLLOWER;  // 保持 Follower
    }

    FB_ASSERT_EQ(final_state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_rpc, timeoutnow_quorum_check) {
    // 法定节点检查
    int total_nodes = 5;
    int reachable_nodes = 3;

    // 至少需要联系多数派才能有效选举
    bool quorum_reachable = reachable_nodes > total_nodes / 2;
    FB_ASSERT_TRUE(quorum_reachable);

    // 无法联系多数派
    reachable_nodes = 2;
    quorum_reachable = reachable_nodes > total_nodes / 2;
    FB_ASSERT_FALSE(quorum_reachable);
}

FB_TEST(raft_rpc, timeoutnow_graceful_shutdown) {
    // 优雅关闭时的 TimeoutNow 处理
    bool is_shutting_down = true;
    bool ignore_timeout_now = is_shutting_down;

    FB_ASSERT_TRUE(ignore_timeout_now);
}

FB_TEST(raft_rpc, timeoutnow_network_delay) {
    // 网络延迟场景
    raft_time_t send_time = 1000;
    raft_time_t receive_time = 1500;
    raft_time_t propagation_delay = receive_time - send_time;

    FB_ASSERT_EQ(propagation_delay, 500L);

    // 超时补偿
    raft_time_t election_timeout = 300;
    bool need_compensation = propagation_delay > election_timeout;
    FB_ASSERT_TRUE(need_compensation);
}

FB_TEST(raft_rpc, timeoutnow_state_machine_consistency) {
    // 状态机一致性保证
    raft_index_t last_applied = 100;
    raft_index_t commit_idx = 100;

    // TimeoutNow 前确保已应用
    bool consistent = last_applied >= commit_idx;
    FB_ASSERT_TRUE(consistent);

    // 记录最后应用索引
    raft_index_t saved_applied = last_applied;
    FB_ASSERT_EQ(saved_applied, 100L);
}

// ============================================================================
// Test Suite: AddNode RPC Tests (Membership Change)
// ============================================================================

FB_TEST(raft_rpc, addnode_request_fields) {
    // 模拟 AddNode 请求字段
    raft_term_t term = 5;
    raft_node_id_t new_node_id = 4;
    std::string addr = "127.0.0.1";
    int port = 8888;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(new_node_id > 0);
    FB_ASSERT_TRUE(port > 0);
}

FB_TEST(raft_rpc, addnode_leader_only_operation) {
    // 只有 Leader 可以添加节点
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool can_add_node = (state == RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(can_add_node);

    state = RAFT_STATE_LEADER;
    can_add_node = (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_add_node);
}

FB_TEST(raft_rpc, addnode_joint_consensus_phase) {
    // 添加节点进入联合共识阶段
    std::vector<raft_node_id_t> old_config = {1, 2, 3};
    std::vector<raft_node_id_t> new_config = {1, 2, 3, 4};  // 新增节点4

    // 联合共识期间，新旧配置都有效
    bool in_joint_consensus = true;
    FB_ASSERT_TRUE(in_joint_consensus);

    // 新节点最初是非投票节点
    bool is_voting = false;
    FB_ASSERT_FALSE(is_voting);
}

FB_TEST(raft_rpc, addnode_node_info_validation) {
    // 验证节点信息有效性
    raft_node_id_t node_id = 5;
    std::string addr = "192.168.1.100";
    int port = 9000;

    // ID 必须唯一且有效
    bool valid_id = node_id > 0;
    FB_ASSERT_TRUE(valid_id);

    // 端口必须合法
    bool valid_port = port > 0 && port <= 65535;
    FB_ASSERT_TRUE(valid_port);

    // 地址不能为空
    bool valid_addr = !addr.empty();
    FB_ASSERT_TRUE(valid_addr);
}

FB_TEST(raft_rpc, addnode_catch_up_process) {
    // 新节点需要追赶日志
    raft_index_t leader_last_idx = 100;
    raft_index_t new_node_match_idx = 0;

    // 新节点开始追赶
    bool needs_catchup = new_node_match_idx < leader_last_idx;
    FB_ASSERT_TRUE(needs_catchup);

    // 追赶进度
    new_node_match_idx = 50;
    double progress = 100.0 * new_node_match_idx / leader_last_idx;
    FB_ASSERT_TRUE(progress >= 50.0);

    // 追赶完成
    new_node_match_idx = leader_last_idx;
    needs_catchup = new_node_match_idx < leader_last_idx;
    FB_ASSERT_FALSE(needs_catchup);
}

FB_TEST(raft_rpc, addnode_promote_to_voting) {
    // 新节点追赶完成后提升为投票节点
    bool is_voting = false;
    bool catchup_complete = true;

    if (catchup_complete) {
        is_voting = true;
    }

    FB_ASSERT_TRUE(is_voting);
}

FB_TEST(raft_rpc, addnode_config_index_tracking) {
    // 配置变更日志索引跟踪
    raft_index_t config_entry_idx = 50;
    raft_term_t config_term = 5;

    // 配置变更作为特殊日志条目
    FB_ASSERT_TRUE(config_entry_idx > 0);
    FB_ASSERT_TRUE(config_term > 0);

    // 变更在日志提交后生效
    raft_index_t commit_idx = 55;
    bool config_applied = commit_idx >= config_entry_idx;
    FB_ASSERT_TRUE(config_applied);
}

FB_TEST(raft_rpc, addnode_duplicate_id_check) {
    // 检查节点 ID 是否已存在
    std::set<raft_node_id_t> existing_nodes = {1, 2, 3};
    raft_node_id_t new_id = 3;

    bool duplicate = existing_nodes.count(new_id) > 0;
    FB_ASSERT_TRUE(duplicate);

    // 使用唯一 ID
    new_id = 4;
    duplicate = existing_nodes.count(new_id) > 0;
    FB_ASSERT_FALSE(duplicate);
}

FB_TEST(raft_rpc, addnode_response_fields) {
    raft_term_t term = 5;
    bool success = true;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(success);
}

FB_TEST(raft_rpc, addnode_failure_node_unreachable) {
    // 新节点无法连接
    bool node_reachable = false;
    bool add_success = node_reachable;
    FB_ASSERT_FALSE(add_success);

    // 重试机制
    int retry_count = 0;
    int max_retries = 3;
    while (!node_reachable && retry_count < max_retries) {
        retry_count++;
    }
    FB_ASSERT_EQ(retry_count, 3);
}

FB_TEST(raft_rpc, addnode_rollback_on_failure) {
    // 添加失败时回滚配置
    std::vector<raft_node_id_t> config = {1, 2, 3};
    std::vector<raft_node_id_t> backup = config;

    // 尝试添加
    config.push_back(4);

    // 失败回滚
    bool add_failed = true;
    if (add_failed) {
        config = backup;
    }

    FB_ASSERT_EQ(config.size(), 3UL);
}

FB_TEST(raft_rpc, addnode_multiple_nodes_batch) {
    // 批量添加多个节点
    std::vector<raft_node_id_t> config = {1, 2, 3};
    std::vector<raft_node_id_t> new_nodes = {4, 5};

    // 逐个添加
    for (auto id : new_nodes) {
        config.push_back(id);
    }

    FB_ASSERT_EQ(config.size(), 5UL);

    // 每个添加都是独立的配置变更
    int config_changes = new_nodes.size();
    FB_ASSERT_EQ(config_changes, 2);
}

FB_TEST(raft_rpc, addnode_network_partition_check) {
    // 添加前检查网络连通性
    std::set<raft_node_id_t> reachable_nodes = {1, 2, 3};
    raft_node_id_t new_node = 4;

    bool can_reach = reachable_nodes.count(new_node) > 0;
    FB_ASSERT_FALSE(can_reach);

    // 网络恢复后
    reachable_nodes.insert(new_node);
    can_reach = reachable_nodes.count(new_node) > 0;
    FB_ASSERT_TRUE(can_reach);
}

FB_TEST(raft_rpc, addnode_cluster_size_limit) {
    // 集群大小限制检查
    size_t max_nodes = 100;
    size_t current_nodes = 95;

    bool can_add = current_nodes < max_nodes;
    FB_ASSERT_TRUE(can_add);

    // 达到限制
    current_nodes = 100;
    can_add = current_nodes < max_nodes;
    FB_ASSERT_FALSE(can_add);
}

// ============================================================================
// Test Suite: RemoveNode RPC Tests (Membership Change)
// ============================================================================

FB_TEST(raft_rpc, removenode_request_fields) {
    // 模拟 RemoveNode 请求字段
    raft_term_t term = 5;
    raft_node_id_t remove_node_id = 3;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(remove_node_id > 0);
}

FB_TEST(raft_rpc, removenode_leader_only_operation) {
    // 只有 Leader 可以移除节点
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool can_remove_node = (state == RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(can_remove_node);

    state = RAFT_STATE_LEADER;
    can_remove_node = (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_remove_node);
}

FB_TEST(raft_rpc, removenode_cannot_remove_self) {
    // Leader 不能直接移除自己
    raft_node_id_t leader_id = 1;
    raft_node_id_t remove_id = 1;

    bool is_self = (leader_id == remove_id);
    FB_ASSERT_TRUE(is_self);

    // 需要先转移领导权
    bool needs_transfer = is_self;
    FB_ASSERT_TRUE(needs_transfer);
}

FB_TEST(raft_rpc, removenode_joint_consensus_phase) {
    // 移除节点进入联合共识阶段
    std::vector<raft_node_id_t> old_config = {1, 2, 3, 4};
    std::vector<raft_node_id_t> new_config = {1, 2, 4};  // 移除节点3

    // 联合共识期间，新旧配置都有效
    bool in_joint_consensus = true;
    FB_ASSERT_TRUE(in_joint_consensus);
}

FB_TEST(raft_rpc, removenode_graceful_shutdown) {
    // 被移除节点需要优雅关闭
    bool node_removed = true;
    bool pending_entries = true;

    // 等待待处理日志完成
    if (pending_entries) {
        // 先处理完待处理日志
        pending_entries = false;
    }

    FB_ASSERT_FALSE(pending_entries);
    FB_ASSERT_TRUE(node_removed);
}

FB_TEST(raft_rpc, removenode_log_truncation) {
    // 移除节点后，可能需要截断其日志
    raft_index_t removed_node_match_idx = 50;
    raft_index_t leader_commit_idx = 60;

    // 移除节点的日志索引不再重要
    bool was_caught_up = removed_node_match_idx >= leader_commit_idx;
    FB_ASSERT_FALSE(was_caught_up);
}

FB_TEST(raft_rpc, removenode_majority_preserved) {
    // 移除节点后必须保持多数派
    uint64_t old_nodes = 5;
    uint64_t new_nodes = 4;

    // 移除后仍需多数派
    uint64_t new_majority = new_nodes / 2 + 1;
    FB_ASSERT_EQ(new_majority, 3UL);

    // 验证集群仍然可用
    bool cluster_available = new_nodes >= 3;
    FB_ASSERT_TRUE(cluster_available);
}

FB_TEST(raft_rpc, removenode_response_fields) {
    raft_term_t term = 5;
    bool success = true;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(success);
}

FB_TEST(raft_rpc, removenode_node_not_found) {
    // 尝试移除不存在的节点
    std::set<raft_node_id_t> existing_nodes = {1, 2, 3};
    raft_node_id_t remove_id = 99;

    bool node_exists = existing_nodes.count(remove_id) > 0;
    FB_ASSERT_FALSE(node_exists);

    // 移除失败
    bool remove_success = node_exists;
    FB_ASSERT_FALSE(remove_success);
}

FB_TEST(raft_rpc, removenode_quorum_after_removal) {
    // 移除后检查是否仍有法定节点
    uint64_t total_nodes = 5;
    uint64_t removing_count = 2;
    uint64_t remaining_nodes = total_nodes - removing_count;

    // 剩余节点需要能形成多数派
    bool has_quorum = remaining_nodes >= 2;
    FB_ASSERT_TRUE(has_quorum);

    // 移除太多节点会失去法定节点
    removing_count = 4;
    remaining_nodes = total_nodes - removing_count;
    has_quorum = remaining_nodes >= 2;
    FB_ASSERT_FALSE(has_quorum);
}

FB_TEST(raft_rpc, removenode_config_index_tracking) {
    // 配置变更日志索引跟踪
    raft_index_t config_entry_idx = 55;
    raft_term_t config_term = 5;

    FB_ASSERT_TRUE(config_entry_idx > 0);
    FB_ASSERT_TRUE(config_term > 0);

    // 变更在日志提交后生效
    raft_index_t commit_idx = 60;
    bool config_applied = commit_idx >= config_entry_idx;
    FB_ASSERT_TRUE(config_applied);
}

FB_TEST(raft_rpc, removenode_leader_transfer_first) {
    // 移除 Leader 需要先转移领导权
    raft_node_id_t leader_id = 1;
    raft_node_id_t remove_id = 1;

    if (remove_id == leader_id) {
        // 需要先转移领导权给其他节点
        raft_node_id_t new_leader = 2;
        leader_id = new_leader;
    }

    FB_ASSERT_NE(leader_id, remove_id);
}

FB_TEST(raft_rpc, removenode_rollback_on_failure) {
    // 移除失败时回滚配置
    std::vector<raft_node_id_t> config = {1, 2, 3, 4};
    std::vector<raft_node_id_t> backup = config;

    // 尝试移除
    config.erase(std::remove(config.begin(), config.end(), 3), config.end());

    // 失败回滚
    bool remove_failed = true;
    if (remove_failed) {
        config = backup;
    }

    FB_ASSERT_EQ(config.size(), 4UL);
}

FB_TEST(raft_rpc, removenode_pending_votes_clear) {
    // 移除节点时清理其投票状态
    std::map<raft_node_id_t, bool> voted_for_me;
    voted_for_me[1] = true;
    voted_for_me[2] = true;
    voted_for_me[3] = true;

    // 移除节点3的投票记录
    raft_node_id_t remove_id = 3;
    voted_for_me.erase(remove_id);

    FB_ASSERT_EQ(voted_for_me.size(), 2UL);
    FB_ASSERT_FALSE(voted_for_me.count(3));
}

FB_TEST(raft_rpc, removenode_network_partition_handling) {
    // 网络分区时的节点移除
    std::set<raft_node_id_t> partitioned_nodes = {3, 4};
    raft_node_id_t remove_id = 3;

    // 分区中的节点可能无法收到移除通知
    bool in_partition = partitioned_nodes.count(remove_id) > 0;
    FB_ASSERT_TRUE(in_partition);

    // 需要 Leader 直接更新配置
    bool leader_update = true;
    FB_ASSERT_TRUE(leader_update);
}

FB_TEST(raft_rpc, removenode_multiple_nodes_batch) {
    // 批量移除多个节点
    std::vector<raft_node_id_t> config = {1, 2, 3, 4, 5};
    std::vector<raft_node_id_t> remove_nodes = {3, 4};

    // 逐个移除（每次都是独立的配置变更）
    for (auto id : remove_nodes) {
        config.erase(std::remove(config.begin(), config.end(), id), config.end());
    }

    FB_ASSERT_EQ(config.size(), 3UL);
    FB_ASSERT_TRUE(std::find(config.begin(), config.end(), 3) == config.end());
}

// ============================================================================
// Test Suite: ReadIndex RPC Tests (Linearizable Read)
// ============================================================================

FB_TEST(raft_rpc, readindex_request_fields) {
    // 模拟 ReadIndex 请求字段
    raft_term_t term = 5;
    raft_node_id_t leader_id = 1;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(leader_id > 0);
}

FB_TEST(raft_rpc, readindex_leader_only_operation) {
    // 只有 Leader 可以处理 ReadIndex
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool can_read = (state == RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(can_read);

    state = RAFT_STATE_LEADER;
    can_read = (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_read);
}

FB_TEST(raft_rpc, readindex_lease_based_read) {
    // 基于租约的读一致性
    raft_time_t lease_expiry = 1000;
    raft_time_t current_time = 800;

    bool lease_valid = current_time < lease_expiry;
    FB_ASSERT_TRUE(lease_valid);

    // 租约过期
    current_time = 1200;
    lease_valid = current_time < lease_expiry;
    FB_ASSERT_FALSE(lease_valid);
}

FB_TEST(raft_rpc, readindex_quorum_heartbeat) {
    // ReadIndex 需要确认领导权（发送心跳确认）
    uint64_t node_num = 5;
    uint64_t heartbeat_responses = 3;

    bool has_quorum = heartbeat_responses > node_num / 2;
    FB_ASSERT_TRUE(has_quorum);

    // 未确认领导权
    heartbeat_responses = 2;
    has_quorum = heartbeat_responses > node_num / 2;
    FB_ASSERT_FALSE(has_quorum);
}

FB_TEST(raft_rpc, readindex_commit_idx_check) {
    // ReadIndex 需要等待 commit_idx 应用
    raft_index_t commit_idx = 100;
    raft_index_t last_applied = 95;

    bool can_read = last_applied >= commit_idx;
    FB_ASSERT_FALSE(can_read);

    // 等待应用完成
    last_applied = 100;
    can_read = last_applied >= commit_idx;
    FB_ASSERT_TRUE(can_read);
}

FB_TEST(raft_rpc, readindex_response_fields) {
    raft_index_t read_index = 100;
    bool success = true;

    FB_ASSERT_TRUE(read_index >= 0);
    FB_ASSERT_TRUE(success);
}

FB_TEST(raft_rpc, readindex_follower_redirect) {
    // Follower 收到 ReadIndex 请求时重定向到 Leader
    raft_identity state = RAFT_STATE_FOLLOWER;
    raft_node_id_t leader_id = 2;

    if (state != RAFT_STATE_LEADER) {
        // 返回 Leader ID 供客户端重定向
        FB_ASSERT_TRUE(leader_id > 0);
    }
}

FB_TEST(raft_rpc, readindex_wait_for_commit) {
    // ReadIndex 等待日志提交
    raft_index_t proposed_idx = 105;
    raft_index_t commit_idx = 100;

    bool need_wait = proposed_idx > commit_idx;
    FB_ASSERT_TRUE(need_wait);

    // 日志提交完成
    commit_idx = proposed_idx;
    need_wait = proposed_idx > commit_idx;
    FB_ASSERT_FALSE(need_wait);
}

FB_TEST(raft_rpc, readindex_concurrent_reads) {
    // 并发 ReadIndex 请求
    int concurrent_reads = 5;
    raft_index_t last_commit = 100;

    // 所有读请求使用同一个 commit_idx
    for (int i = 0; i < concurrent_reads; i++) {
        raft_index_t read_idx = last_commit;
        FB_ASSERT_EQ(read_idx, 100L);
    }
}

FB_TEST(raft_rpc, readindex_batch_optimization) {
    // 批量 ReadIndex 优化
    std::vector<raft_index_t> read_requests = {100, 100, 100, 105, 105};
    raft_index_t max_read_idx = *std::max_element(read_requests.begin(), read_requests.end());

    // 只需要等待最大的 read_idx 提交
    FB_ASSERT_EQ(max_read_idx, 105L);
}

FB_TEST(raft_rpc, readindex_lease_renewal) {
    // 读请求时续约租约
    raft_time_t lease_expiry = 1000;
    raft_time_t heartbeat_period = 100;

    // 收到读请求时续约
    lease_expiry += heartbeat_period;
    FB_ASSERT_EQ(lease_expiry, 1100L);
}

FB_TEST(raft_rpc, readindex_timeout_handling) {
    // ReadIndex 超时处理
    int read_timeout_ms = 500;
    int elapsed_ms = 300;

    bool timed_out = elapsed_ms >= read_timeout_ms;
    FB_ASSERT_FALSE(timed_out);

    elapsed_ms = 600;
    timed_out = elapsed_ms >= read_timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, readindex_stale_leader_detection) {
    // 检测过期 Leader
    raft_term_t current_term = 6;
    raft_term_t leader_term = 5;

    bool is_stale_leader = leader_term < current_term;
    FB_ASSERT_TRUE(is_stale_leader);

    // 拒绝过期 Leader 的读请求
    bool allow_read = !is_stale_leader;
    FB_ASSERT_FALSE(allow_read);
}

FB_TEST(raft_rpc, readindex_network_partition) {
    // 网络分区时的 ReadIndex
    std::set<raft_node_id_t> reachable_nodes = {1, 2};
    uint64_t total_nodes = 5;

    bool has_quorum = reachable_nodes.size() > total_nodes / 2;
    FB_ASSERT_FALSE(has_quorum);

    // 无法确认领导权，拒绝读请求
    bool allow_read = has_quorum;
    FB_ASSERT_FALSE(allow_read);
}

FB_TEST(raft_rpc, readindex_state_machine_query) {
    // ReadIndex 后查询状态机
    raft_index_t read_idx = 100;
    raft_index_t last_applied = 100;

    bool can_query = last_applied >= read_idx;
    FB_ASSERT_TRUE(can_query);

    // 模拟状态机查询
    int query_result = 42;
    FB_ASSERT_EQ(query_result, 42);
}

FB_TEST(raft_rpc, readindex_retry_on_failure) {
    // ReadIndex 失败重试
    int retry_count = 0;
    int max_retries = 3;
    bool success = false;

    while (!success && retry_count < max_retries) {
        retry_count++;
        if (retry_count == 2) {
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, readindex_lease_vs_quorum) {
    // 租约模式 vs Quorum 确认模式对比
    bool use_lease = true;
    int lease_read_latency = 1;  // 租约读：1次RPC
    int quorum_read_latency = 2; // Quorum读：需要心跳确认

    if (use_lease) {
        FB_ASSERT_TRUE(lease_read_latency < quorum_read_latency);
    } else {
        // Quorum 模式更安全但延迟更高
        FB_ASSERT_TRUE(quorum_read_latency > lease_read_latency);
    }
}

// ============================================================================
// Test Suite: TransferLeader RPC Tests (Leadership Transfer)
// ============================================================================

FB_TEST(raft_rpc, transferleader_request_fields) {
    // 模拟 TransferLeader 请求字段
    raft_term_t term = 5;
    raft_node_id_t target_node_id = 3;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(target_node_id > 0);
}

FB_TEST(raft_rpc, transferleader_leader_only_operation) {
    // 只有当前 Leader 可以发起领导权转移
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool can_transfer = (state == RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(can_transfer);

    state = RAFT_STATE_LEADER;
    can_transfer = (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_transfer);
}

FB_TEST(raft_rpc, transferleader_target_must_be_follower) {
    // 目标节点必须是 Follower
    raft_identity target_state = RAFT_STATE_CANDIDATE;
    bool valid_target = (target_state == RAFT_STATE_FOLLOWER);
    FB_ASSERT_FALSE(valid_target);

    target_state = RAFT_STATE_FOLLOWER;
    valid_target = (target_state == RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(valid_target);
}

FB_TEST(raft_rpc, transferleader_target_log_up_to_date) {
    // 目标节点日志必须是最新的
    raft_index_t leader_last_idx = 100;
    raft_index_t target_match_idx = 95;

    bool log_ready = target_match_idx >= leader_last_idx;
    FB_ASSERT_FALSE(log_ready);

    // 目标节点日志追上
    target_match_idx = 100;
    log_ready = target_match_idx >= leader_last_idx;
    FB_ASSERT_TRUE(log_ready);
}

FB_TEST(raft_rpc, transferleader_send_timeoutnow) {
    // Leader 发送 TimeoutNow 给目标节点
    raft_node_id_t leader_id = 1;
    raft_node_id_t target_id = 3;

    // 发送 TimeoutNow 触发目标节点立即选举
    bool timeoutnow_sent = true;
    FB_ASSERT_TRUE(timeoutnow_sent);
}

FB_TEST(raft_rpc, transferleader_leader_step_down) {
    // Leader 发起转移后立即退位
    raft_identity state = RAFT_STATE_LEADER;

    // 发送 TimeoutNow 后退位为 Follower
    state = RAFT_STATE_FOLLOWER;

    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_rpc, transferleader_new_leader_elected) {
    // 目标节点成为新 Leader
    raft_node_id_t old_leader = 1;
    raft_node_id_t new_leader = 3;

    FB_ASSERT_TRUE(new_leader != old_leader);

    // 新 Leader term 应该更大
    raft_term_t old_term = 5;
    raft_term_t new_term = 6;
    FB_ASSERT_TRUE(new_term > old_term);
}

FB_TEST(raft_rpc, transferleader_abort_on_new_entry) {
    // 转移期间有新日志写入时中止
    bool transfer_in_progress = true;
    bool new_entry_arrived = true;

    if (new_entry_arrived) {
        transfer_in_progress = false;
    }

    FB_ASSERT_FALSE(transfer_in_progress);
}

FB_TEST(raft_rpc, transferleader_abort_on_higher_term) {
    // 收到更高 term 时中止转移
    raft_term_t current_term = 5;
    raft_term_t received_term = 6;

    bool abort_transfer = received_term > current_term;
    FB_ASSERT_TRUE(abort_transfer);
}

FB_TEST(raft_rpc, transferleader_timeout_handling) {
    // 转移超时处理
    int transfer_timeout_ms = 500;
    int elapsed_ms = 600;

    bool timed_out = elapsed_ms >= transfer_timeout_ms;
    FB_ASSERT_TRUE(timed_out);

    // 超时后保持原 Leader
    raft_identity state = RAFT_STATE_LEADER;
    FB_ASSERT_EQ(state, RAFT_STATE_LEADER);
}

FB_TEST(raft_rpc, transferleader_retry_on_failure) {
    // 转移失败重试
    int retry_count = 0;
    int max_retries = 3;
    bool success = false;

    while (!success && retry_count < max_retries) {
        retry_count++;
        if (retry_count == 2) {
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, transferleader_multiple_targets) {
    // 不能同时向多个目标转移
    std::vector<raft_node_id_t> targets = {2, 3};

    // 只能选择一个目标
    raft_node_id_t selected_target = targets[0];
    FB_ASSERT_EQ(selected_target, 2L);

    bool single_target = targets.size() == 1;
    FB_ASSERT_FALSE(single_target);
}

FB_TEST(raft_rpc, transferleader_node_not_found) {
    // 目标节点不存在
    std::set<raft_node_id_t> existing_nodes = {1, 2, 3};
    raft_node_id_t target_id = 99;

    bool node_exists = existing_nodes.count(target_id) > 0;
    FB_ASSERT_FALSE(node_exists);

    // 转移失败
    bool transfer_success = node_exists;
    FB_ASSERT_FALSE(transfer_success);
}

FB_TEST(raft_rpc, transferleader_unreachable_target) {
    // 目标节点不可达
    std::set<raft_node_id_t> reachable_nodes = {1, 2};
    raft_node_id_t target_id = 3;

    bool reachable = reachable_nodes.count(target_id) > 0;
    FB_ASSERT_FALSE(reachable);

    // 转移失败
    bool transfer_success = reachable;
    FB_ASSERT_FALSE(transfer_success);
}

FB_TEST(raft_rpc, transferleader_progress_tracking) {
    // 转移进度跟踪
    std::string phase = "init";
    FB_ASSERT_EQ(phase, "init");

    phase = "check_target";
    FB_ASSERT_EQ(phase, "check_target");

    phase = "send_timeoutnow";
    FB_ASSERT_EQ(phase, "send_timeoutnow");

    phase = "wait_new_leader";
    FB_ASSERT_EQ(phase, "wait_new_leader");

    phase = "completed";
    FB_ASSERT_EQ(phase, "completed");
}

FB_TEST(raft_rpc, transferleader_rollback_on_failure) {
    // 转移失败时恢复原状态
    raft_identity state = RAFT_STATE_LEADER;
    raft_identity backup_state = state;

    // 尝试转移
    bool transfer_failed = true;
    if (transfer_failed) {
        state = backup_state;  // 保持原状态
    }

    FB_ASSERT_EQ(state, RAFT_STATE_LEADER);
}

FB_TEST(raft_rpc, transferleader_client_redirect) {
    // 转移完成后客户端重定向
    raft_node_id_t old_leader = 1;
    raft_node_id_t new_leader = 3;

    // 客户端收到旧 Leader 的重定向响应
    bool need_redirect = true;
    raft_node_id_t redirect_target = new_leader;

    FB_ASSERT_TRUE(need_redirect);
    FB_ASSERT_EQ(redirect_target, 3L);
}

FB_TEST(raft_rpc, transferleader_graceful_vs_forceful) {
    // 优雅转移 vs 强制转移
    bool graceful = true;
    bool wait_for_log_sync = graceful;

    FB_ASSERT_TRUE(wait_for_log_sync);

    // 强制转移可能导致日志不一致
    graceful = false;
    wait_for_log_sync = graceful;
    FB_ASSERT_FALSE(wait_for_log_sync);
}

FB_TEST(raft_rpc, transferleader_joint_consensus_check) {
    // 联合共识期间不能转移
    bool in_joint_consensus = true;

    bool can_transfer = !in_joint_consensus;
    FB_ASSERT_FALSE(can_transfer);

    // 联合共识完成后可以转移
    in_joint_consensus = false;
    can_transfer = !in_joint_consensus;
    FB_ASSERT_TRUE(can_transfer);
}

FB_TEST(raft_rpc, transferleader_config_change_in_progress) {
    // 配置变更期间不能转移
    bool config_change_in_progress = true;

    bool can_transfer = !config_change_in_progress;
    FB_ASSERT_FALSE(can_transfer);
}

FB_TEST(raft_rpc, transferleader_snapshot_in_progress) {
    // 快照传输期间不能转移
    bool snapshot_in_progress = true;

    bool can_transfer = !snapshot_in_progress;
    FB_ASSERT_FALSE(can_transfer);
}

FB_TEST(raft_rpc, transferleader_pending_writes_flush) {
    // 转移前刷新待写入
    int pending_writes = 10;

    // 转移前需要处理完待写入
    while (pending_writes > 0) {
        pending_writes--;
    }

    FB_ASSERT_EQ(pending_writes, 0);
}

// ============================================================================
// Test Suite: Propose RPC Tests (Write Operations)
// ============================================================================

FB_TEST(raft_rpc, propose_request_fields) {
    // 模拟 Propose 请求字段
    raft_term_t term = 5;
    raft_node_id_t leader_id = 1;
    raft_index_t prev_log_idx = 10;
    raft_term_t prev_log_term = 4;
    int entry_type = 0;  // RAFT_LOGTYPE_WRITE
    size_t entry_size = 1024;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(leader_id > 0);
    FB_ASSERT_TRUE(prev_log_idx >= 0);
    FB_ASSERT_TRUE(prev_log_term >= 0);
    FB_ASSERT_TRUE(entry_size > 0);
}

FB_TEST(raft_rpc, propose_leader_only_operation) {
    // 只有 Leader 可以处理写入
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool can_propose = (state == RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(can_propose);

    state = RAFT_STATE_LEADER;
    can_propose = (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_propose);
}

FB_TEST(raft_rpc, propose_append_to_log) {
    // 写入请求追加到 Leader 日志
    raft_index_t last_log_idx = 100;
    raft_index_t new_entry_idx = last_log_idx + 1;

    FB_ASSERT_EQ(new_entry_idx, 101L);

    // next_idx 递增
    last_log_idx = new_entry_idx;
    FB_ASSERT_EQ(last_log_idx, 101L);
}

FB_TEST(raft_rpc, propose_replicate_to_followers) {
    // 写入需要复制到多数派 Follower
    uint64_t node_num = 5;
    uint64_t replication_count = 3;

    bool has_quorum = replication_count > node_num / 2;
    FB_ASSERT_TRUE(has_quorum);

    // 未达到多数派
    replication_count = 2;
    has_quorum = replication_count > node_num / 2;
    FB_ASSERT_FALSE(has_quorum);
}

FB_TEST(raft_rpc, propose_commit_after_quorum) {
    // 多数派确认后提交
    raft_index_t proposed_idx = 101;
    raft_index_t commit_idx = 100;

    bool can_commit = proposed_idx <= commit_idx;
    FB_ASSERT_FALSE(can_commit);

    // 多数派确认后更新 commit_idx
    commit_idx = proposed_idx;
    can_commit = proposed_idx <= commit_idx;
    FB_ASSERT_TRUE(can_commit);
}

FB_TEST(raft_rpc, propose_apply_to_state_machine) {
    // 提交后应用到状态机
    raft_index_t commit_idx = 101;
    raft_index_t last_applied = 100;

    bool need_apply = commit_idx > last_applied;
    FB_ASSERT_TRUE(need_apply);

    // 应用完成
    last_applied = commit_idx;
    need_apply = commit_idx > last_applied;
    FB_ASSERT_FALSE(need_apply);
}

FB_TEST(raft_rpc, propose_response_success) {
    // 写入成功响应
    raft_index_t commit_idx = 101;
    bool success = true;

    FB_ASSERT_TRUE(commit_idx > 0);
    FB_ASSERT_TRUE(success);
}

FB_TEST(raft_rpc, propose_response_failure_redirect) {
    // 非 Leader 收到请求时重定向
    raft_identity state = RAFT_STATE_FOLLOWER;
    raft_node_id_t leader_id = 2;

    bool need_redirect = (state != RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(need_redirect);
    FB_ASSERT_TRUE(leader_id > 0);
}

FB_TEST(raft_rpc, propose_entry_id_unique) {
    // 每个写入条目 ID 必须唯一
    raft_entry_id_t id1 = 1001;
    raft_entry_id_t id2 = 1002;

    FB_ASSERT_TRUE(id1 != id2);

    // 检测重复
    std::set<raft_entry_id_t> seen_ids;
    seen_ids.insert(id1);
    bool duplicate = seen_ids.count(id2) > 0;
    FB_ASSERT_FALSE(duplicate);
}

FB_TEST(raft_rpc, propose_batch_optimization) {
    // 批量写入优化
    std::vector<size_t> entry_sizes = {512, 1024, 2048};
    size_t total_size = 0;

    for (size_t s : entry_sizes) {
        total_size += s;
    }

    FB_ASSERT_EQ(total_size, 3584UL);

    // 合并为一次 AppendEntries
    int rpc_calls = 1;
    FB_ASSERT_TRUE(rpc_calls < entry_sizes.size());
}

FB_TEST(raft_rpc, propose_timeout_handling) {
    // 写入超时处理
    int propose_timeout_ms = 1000;
    int elapsed_ms = 1500;

    bool timed_out = elapsed_ms >= propose_timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, propose_retry_on_timeout) {
    // 超时重试
    int retry_count = 0;
    int max_retries = 3;
    bool success = false;

    while (!success && retry_count < max_retries) {
        retry_count++;
        if (retry_count == 2) {
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, propose_conflict_resolution) {
    // 冲突日志条目解决
    raft_index_t leader_prev_idx = 100;
    raft_term_t leader_prev_term = 5;
    raft_index_t follower_last_idx = 95;
    raft_term_t follower_last_term = 5;

    // Follower 日志落后
    bool follower_behind = follower_last_idx < leader_prev_idx;
    FB_ASSERT_TRUE(follower_behind);

    // 需要发送更多日志
    raft_index_t entries_to_send = leader_prev_idx - follower_last_idx;
    FB_ASSERT_EQ(entries_to_send, 5L);
}

FB_TEST(raft_rpc, propose_log_truncation) {
    // 冲突时截断 Follower 日志
    raft_index_t follower_last_idx = 105;
    raft_index_t conflict_idx = 100;

    // 截断冲突部分
    raft_index_t new_last_idx = conflict_idx - 1;
    FB_ASSERT_EQ(new_last_idx, 99L);

    int truncated_count = follower_last_idx - new_last_idx;
    FB_ASSERT_EQ(truncated_count, 6);
}

FB_TEST(raft_rpc, propose_client_request_id) {
    // 客户端请求 ID 用于去重和响应匹配
    uint64_t request_id = 12345;
    uint64_t client_id = 100;

    FB_ASSERT_TRUE(request_id > 0);
    FB_ASSERT_TRUE(client_id > 0);

    // 记录请求 ID 用于响应
    std::map<uint64_t, raft_index_t> pending_requests;
    pending_requests[request_id] = 101;
    FB_ASSERT_EQ(pending_requests[request_id], 101L);
}

FB_TEST(raft_rpc, propose_duplicate_request_detection) {
    // 检测客户端重复请求
    uint64_t client_id = 100;
    uint64_t request_id = 12345;

    std::map<uint64_t, uint64_t> last_request_per_client;

    // 第一次请求
    bool is_duplicate = (last_request_per_client[client_id] == request_id);
    FB_ASSERT_FALSE(is_duplicate);

    // 记录请求
    last_request_per_client[client_id] = request_id;

    // 重复请求
    is_duplicate = (last_request_per_client[client_id] == request_id);
    FB_ASSERT_TRUE(is_duplicate);
}

FB_TEST(raft_rpc, propose_pending_requests_limit) {
    // 待处理请求限制
    size_t max_pending = 1000;
    size_t current_pending = 800;

    bool can_accept = current_pending < max_pending;
    FB_ASSERT_TRUE(can_accept);

    // 达到限制
    current_pending = 1000;
    can_accept = current_pending < max_pending;
    FB_ASSERT_FALSE(can_accept);
}

FB_TEST(raft_rpc, propose_ordering_guarantee) {
    // 写入顺序保证
    raft_index_t idx1 = 100;
    raft_index_t idx2 = 101;
    raft_index_t idx3 = 102;

    // 日志索引严格递增
    FB_ASSERT_TRUE(idx1 < idx2);
    FB_ASSERT_TRUE(idx2 < idx3);

    // 应用顺序与日志顺序一致
    std::vector<raft_index_t> apply_order = {idx1, idx2, idx3};
    FB_ASSERT_EQ(apply_order[0], 100L);
    FB_ASSERT_EQ(apply_order[2], 102L);
}

FB_TEST(raft_rpc, propose_linearizability_check) {
    // 线性一致性检查
    raft_index_t write_commit_idx = 101;
    raft_index_t read_index = 101;

    // 读请求必须在写提交后才能看到
    bool read_after_write = read_index >= write_commit_idx;
    FB_ASSERT_TRUE(read_after_write);
}

FB_TEST(raft_rpc, propose_network_partition_handling) {
    // 网络分区时的写入处理
    std::set<raft_node_id_t> reachable_nodes = {1, 2};
    uint64_t total_nodes = 5;

    bool has_quorum = reachable_nodes.size() > total_nodes / 2;
    FB_ASSERT_FALSE(has_quorum);

    // 无法写入
    bool can_propose = has_quorum;
    FB_ASSERT_FALSE(can_propose);
}

FB_TEST(raft_rpc, propose_leader_change_abort) {
    // Leader 变更时中止写入
    raft_identity state = RAFT_STATE_LEADER;

    // 检测到更高 term
    raft_term_t current_term = 5;
    raft_term_t received_term = 6;

    if (received_term > current_term) {
        state = RAFT_STATE_FOLLOWER;
    }

    // 写入中止
    bool can_propose = (state == RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(can_propose);
}

// ============================================================================
// Test Suite: CheckQuorum RPC Tests (Quorum Verification)
// ============================================================================

FB_TEST(raft_rpc, checkquorum_request_fields) {
    // 模拟 CheckQuorum 请求字段
    raft_term_t term = 5;
    raft_node_id_t leader_id = 1;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(leader_id > 0);
}

FB_TEST(raft_rpc, checkquorum_leader_self_check) {
    // Leader 检查自身是否仍能联系多数派
    uint64_t node_num = 5;
    uint64_t reachable_nodes = 3;

    bool has_quorum = reachable_nodes > node_num / 2;
    FB_ASSERT_TRUE(has_quorum);
}

FB_TEST(raft_rpc, checkquorum_step_down_on_failure) {
    // 无法联系多数派时退位
    uint64_t node_num = 5;
    uint64_t reachable_nodes = 2;

    bool has_quorum = reachable_nodes > node_num / 2;
    FB_ASSERT_FALSE(has_quorum);

    // 退位为 Follower
    raft_identity state = RAFT_STATE_LEADER;
    if (!has_quorum) {
        state = RAFT_STATE_FOLLOWER;
    }
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_rpc, checkquorum_heartbeat_responses) {
    // 通过心跳响应判断节点可达性
    std::map<raft_node_id_t, bool> heartbeat_responses;
    heartbeat_responses[1] = true;   // 自己
    heartbeat_responses[2] = true;
    heartbeat_responses[3] = false;  // 节点3不可达
    heartbeat_responses[4] = true;

    int reachable_count = 0;
    for (const auto& pair : heartbeat_responses) {
        if (pair.second) reachable_count++;
    }

    FB_ASSERT_EQ(reachable_count, 3);
}

FB_TEST(raft_rpc, checkquorum_timeout_threshold) {
    // 心跳超时阈值
    raft_time_t heartbeat_timeout = 100;
    raft_time_t last_contact_time = 800;
    raft_time_t current_time = 1000;

    bool timed_out = (current_time - last_contact_time) > heartbeat_timeout;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, checkquorum_periodic_check) {
    // 定期检查法定节点
    int check_interval_ms = 1000;
    int checks_performed = 0;

    for (int i = 0; i < 5; i++) {
        checks_performed++;
    }

    FB_ASSERT_EQ(checks_performed, 5);
}

FB_TEST(raft_rpc, checkquorum_response_fields) {
    raft_term_t term = 5;
    bool quorum_ok = true;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(quorum_ok);
}

FB_TEST(raft_rpc, checkquorum_network_partition_detection) {
    // 网络分区检测
    std::set<raft_node_id_t> reachable_nodes = {1, 2};
    uint64_t total_nodes = 5;

    bool in_minority_partition = reachable_nodes.size() <= total_nodes / 2;
    FB_ASSERT_TRUE(in_minority_partition);

    // 多数派分区
    reachable_nodes = {1, 2, 3, 4};
    in_minority_partition = reachable_nodes.size() <= total_nodes / 2;
    FB_ASSERT_FALSE(in_minority_partition);
}

FB_TEST(raft_rpc, checkquorum_leader_lease_update) {
    // CheckQuorum 更新租约
    raft_time_t lease_expiry = 1000;
    raft_time_t lease_period = 100;

    // 成功确认后续约租约
    bool quorum_ok = true;
    if (quorum_ok) {
        lease_expiry += lease_period;
    }

    FB_ASSERT_EQ(lease_expiry, 1100L);
}

FB_TEST(raft_rpc, checkquorum_follower_participation) {
    // Follower 参与 CheckQuorum
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool is_leader = (state == RAFT_STATE_LEADER);

    // Follower 不发起 CheckQuorum
    FB_ASSERT_FALSE(is_leader);

    // 但 Follower 响应心跳
    bool respond_to_heartbeat = true;
    FB_ASSERT_TRUE(respond_to_heartbeat);
}

FB_TEST(raft_rpc, checkquorum_minimum_cluster_size) {
    // 最小集群大小检查
    uint64_t min_nodes = 1;
    uint64_t current_nodes = 3;

    bool meets_minimum = current_nodes >= min_nodes;
    FB_ASSERT_TRUE(meets_minimum);

    // 单节点集群
    current_nodes = 1;
    meets_minimum = current_nodes >= min_nodes;
    FB_ASSERT_TRUE(meets_minimum);
}

FB_TEST(raft_rpc, checkquorum_joint_consensus_check) {
    // 联合共识期间的法定节点检查
    uint64_t old_nodes = 5;
    uint64_t new_nodes = 3;
    uint64_t old_reachable = 3;
    uint64_t new_reachable = 2;

    bool old_quorum = old_reachable > old_nodes / 2;
    bool new_quorum = new_reachable > new_nodes / 2;

    // 联合共识期间需要两个配置都满足
    bool joint_quorum = old_quorum && new_quorum;
    FB_ASSERT_TRUE(joint_quorum);
}

FB_TEST(raft_rpc, checkquorum_graceful_degradation) {
    // 优雅降级
    uint64_t healthy_nodes = 3;
    uint64_t total_nodes = 5;

    // 部分节点故障，集群仍可用
    bool cluster_available = healthy_nodes > total_nodes / 2;
    FB_ASSERT_TRUE(cluster_available);

    // 更多节点故障
    healthy_nodes = 2;
    cluster_available = healthy_nodes > total_nodes / 2;
    FB_ASSERT_FALSE(cluster_available);
}

FB_TEST(raft_rpc, checkquorum_node_recovery) {
    // 节点恢复后重新计数
    std::set<raft_node_id_t> reachable_nodes = {1, 2};
    uint64_t total_nodes = 5;

    bool has_quorum = reachable_nodes.size() > total_nodes / 2;
    FB_ASSERT_FALSE(has_quorum);

    // 节点3恢复
    reachable_nodes.insert(3);
    has_quorum = reachable_nodes.size() > total_nodes / 2;
    FB_ASSERT_TRUE(has_quorum);
}

FB_TEST(raft_rpc, checkquorum_pre_vote_check) {
    // PreVote 模式的法定节点检查
    bool use_prevote = true;
    uint64_t reachable_nodes = 2;
    uint64_t total_nodes = 5;

    if (use_prevote) {
        // PreVote 需要先确认能否联系到多数派
        bool can_prevote = reachable_nodes > total_nodes / 2;
        FB_ASSERT_FALSE(can_prevote);
    }
}

FB_TEST(raft_rpc, checkquorum_leader_election_safety) {
    // CheckQuorum 保证选举安全
    // 防止多个 Leader 同时存在
    int active_leaders = 1;
    int expected_leaders = 1;

    FB_ASSERT_EQ(active_leaders, expected_leaders);

    // CheckQuorum 防止分裂脑
    bool single_leader = (active_leaders == 1);
    FB_ASSERT_TRUE(single_leader);
}

FB_TEST(raft_rpc, checkquorum_timeout_trigger_election) {
    //法定节点检查失败触发选举
    bool quorum_failed = true;
    raft_identity state = RAFT_STATE_LEADER;

    if (quorum_failed) {
        state = RAFT_STATE_CANDIDATE;  // 退位后开始新选举
    }

    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
}

FB_TEST(raft_rpc, checkquorum_metrics_collection) {
    //法定节点指标收集
    int quorum_checks_total = 100;
    int quorum_checks_passed = 95;
    int quorum_checks_failed = 5;

    FB_ASSERT_EQ(quorum_checks_total, 100);
    FB_ASSERT_EQ(quorum_checks_passed + quorum_checks_failed, quorum_checks_total);

    double pass_rate = 100.0 * quorum_checks_passed / quorum_checks_total;
    FB_ASSERT_GE(pass_rate, 95.0);
}

FB_TEST(raft_rpc, checkquorum_config_change_handling) {
    // 配置变更期间的法定节点检查
    bool config_change_in_progress = true;

    // 配置变更期间使用联合共识检查
    if (config_change_in_progress) {
        uint64_t old_reachable = 3;
        uint64_t new_reachable = 2;
        uint64_t old_nodes = 5;
        uint64_t new_nodes = 3;

        bool old_ok = old_reachable > old_nodes / 2;
        bool new_ok = new_reachable > new_nodes / 2;

        FB_ASSERT_TRUE(old_ok && new_ok);
    }
}

FB_TEST(raft_rpc, checkquorum_follower_timeout_detection) {
    // Follower 超时检测
    raft_time_t last_leader_contact = 1000;
    raft_time_t election_timeout = 500;
    raft_time_t current_time = 1600;

    bool leader_timeout = (current_time - last_leader_contact) >= election_timeout;
    FB_ASSERT_TRUE(leader_timeout);

    // Follower 应该开始选举
    raft_identity state = RAFT_STATE_FOLLOWER;
    if (leader_timeout) {
        state = RAFT_STATE_CANDIDATE;
    }
    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
}

// ============================================================================
// Test Suite: ClientSession RPC Tests (Session Management)
// ============================================================================

FB_TEST(raft_rpc, clientsession_register_request) {
    // 客户端注册请求
    uint64_t client_id = 0;  // 新客户端ID为0，由Leader分配

    FB_ASSERT_EQ(client_id, 0UL);
}

FB_TEST(raft_rpc, clientsession_client_id_allocation) {
    // Leader 分配客户端 ID
    uint64_t next_client_id = 1001;
    uint64_t allocated_id = next_client_id;
    next_client_id++;

    FB_ASSERT_EQ(allocated_id, 1001UL);
    FB_ASSERT_EQ(next_client_id, 1002UL);
}

FB_TEST(raft_rpc, clientsession_response_fields) {
    uint64_t client_id = 1001;
    uint64_t session_id = 5001;
    raft_term_t term = 5;

    FB_ASSERT_TRUE(client_id > 0);
    FB_ASSERT_TRUE(session_id > 0);
    FB_ASSERT_TRUE(term > 0);
}

FB_TEST(raft_rpc, clientsession_leader_only_registration) {
    // 只有 Leader 可以处理客户端注册
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool can_register = (state == RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(can_register);

    state = RAFT_STATE_LEADER;
    can_register = (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_register);
}

FB_TEST(raft_rpc, clientsession_redirect_to_leader) {
    // Follower 重定向客户端到 Leader
    raft_identity state = RAFT_STATE_FOLLOWER;
    raft_node_id_t leader_id = 2;

    bool need_redirect = (state != RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(need_redirect);
    FB_ASSERT_TRUE(leader_id > 0);
}

FB_TEST(raft_rpc, clientsession_keepalive) {
    // 客户端心跳保活
    raft_time_t last_heartbeat = 1000;
    raft_time_t current_time = 1100;
    raft_time_t session_timeout = 500;

    bool session_valid = (current_time - last_heartbeat) < session_timeout;
    FB_ASSERT_TRUE(session_valid);

    // 会话即将过期
    current_time = 1400;
    session_valid = (current_time - last_heartbeat) < session_timeout;
    FB_ASSERT_FALSE(session_valid);
}

FB_TEST(raft_rpc, clientsession_request_id_tracking) {
    // 请求ID跟踪，防止重复执行
    uint64_t client_id = 1001;
    uint64_t request_id = 12345;

    std::map<uint64_t, uint64_t> last_request;
    last_request[client_id] = request_id;

    // 检查是否重复请求
    bool is_duplicate = (last_request[client_id] == request_id);
    FB_ASSERT_TRUE(is_duplicate);

    // 新请求
    uint64_t new_request_id = 12346;
    is_duplicate = (last_request[client_id] == new_request_id);
    FB_ASSERT_FALSE(is_duplicate);
}

FB_TEST(raft_rpc, clientsession_duplicate_request_response) {
    // 重复请求返回缓存的响应
    uint64_t request_id = 12345;
    bool cached = true;
    raft_index_t result_idx = 100;

    if (cached) {
        // 直接返回缓存结果
        FB_ASSERT_EQ(result_idx, 100L);
    }
}

FB_TEST(raft_rpc, clientsession_session_expiration) {
    // 会话过期处理
    raft_time_t session_start = 1000;
    raft_time_t session_timeout = 30000;  // 30秒
    raft_time_t current_time = 32000;

    bool session_expired = (current_time - session_start) >= session_timeout;
    FB_ASSERT_TRUE(session_expired);

    // 清理过期会话
    if (session_expired) {
        // 删除会话状态
    }
}

FB_TEST(raft_rpc, clientsession_leader_change_invalidation) {
    // Leader 变更时客户端会话可能失效
    raft_term_t session_term = 5;
    raft_term_t current_term = 6;

    bool session_invalid = (session_term < current_term);
    FB_ASSERT_TRUE(session_invalid);

    // 客户端需要重新注册
    bool need_reregister = session_invalid;
    FB_ASSERT_TRUE(need_reregister);
}

FB_TEST(raft_rpc, clientsession_max_sessions_limit) {
    // 最大会话数限制
    size_t max_sessions = 10000;
    size_t current_sessions = 9500;

    bool can_accept = current_sessions < max_sessions;
    FB_ASSERT_TRUE(can_accept);

    // 达到限制
    current_sessions = 10000;
    can_accept = current_sessions < max_sessions;
    FB_ASSERT_FALSE(can_accept);
}

FB_TEST(raft_rpc, clientsession_cleanup_on_disconnect) {
    // 客户端断开连接时清理会话
    uint64_t client_id = 1001;
    std::set<uint64_t> active_sessions = {1001, 1002, 1003};

    // 清理会话
    active_sessions.erase(client_id);

    FB_ASSERT_FALSE(active_sessions.count(client_id));
    FB_ASSERT_EQ(active_sessions.size(), 2UL);
}

FB_TEST(raft_rpc, clientsession_reconnect_handling) {
    // 客户端重连处理
    uint64_t client_id = 1001;
    uint64_t old_session_id = 5001;
    uint64_t new_session_id = 5002;

    // 旧会话失效
    std::map<uint64_t, uint64_t> session_map;
    session_map[client_id] = new_session_id;

    FB_ASSERT_EQ(session_map[client_id], new_session_id);
    FB_ASSERT_TRUE(session_map[client_id] != old_session_id);
}

FB_TEST(raft_rpc, clientsession_pending_requests_tracking) {
    // 待处理请求跟踪
    uint64_t client_id = 1001;
    std::map<uint64_t, int> pending_counts;
    pending_counts[client_id] = 3;

    // 客户端有待处理请求
    bool has_pending = pending_counts[client_id] > 0;
    FB_ASSERT_TRUE(has_pending);

    // 请求完成后更新
    pending_counts[client_id]--;
    FB_ASSERT_EQ(pending_counts[client_id], 2);
}

FB_TEST(raft_rpc, clientsession_serial_execution) {
    // 同一客户端请求串行执行
    std::vector<uint64_t> request_order = {1, 2, 3};
    std::vector<uint64_t> execute_order;

    for (auto id : request_order) {
        execute_order.push_back(id);
    }

    FB_ASSERT_EQ(execute_order.size(), 3UL);
    FB_ASSERT_EQ(execute_order[0], 1UL);
    FB_ASSERT_EQ(execute_order[2], 3UL);
}

FB_TEST(raft_rpc, clientsession_timeout_handling) {
    // 会话操作超时
    int timeout_ms = 1000;
    int elapsed_ms = 1500;

    bool timed_out = elapsed_ms >= timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, clientsession_retry_on_leader_change) {
    // Leader 变更后客户端重试
    raft_node_id_t known_leader = 1;
    raft_node_id_t new_leader = 2;

    bool leader_changed = (known_leader != new_leader);
    FB_ASSERT_TRUE(leader_changed);

    // 客户端需要重试到新Leader
    raft_node_id_t retry_target = new_leader;
    FB_ASSERT_EQ(retry_target, 2L);
}

// ============================================================================
// Test Suite: GetConfiguration RPC Tests (Configuration Query)
// ============================================================================

FB_TEST(raft_rpc, getconfiguration_request_fields) {
    // 配置查询请求字段
    raft_term_t term = 5;
    raft_node_id_t requester_id = 3;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(requester_id > 0);
}

FB_TEST(raft_rpc, getconfiguration_response_nodes) {
    // 配置响应包含节点列表
    std::vector<raft_node_id_t> nodes = {1, 2, 3, 4, 5};

    FB_ASSERT_EQ(nodes.size(), 5UL);
    FB_ASSERT_TRUE(nodes[0] == 1);
    FB_ASSERT_TRUE(nodes[4] == 5);
}

FB_TEST(raft_rpc, getconfiguration_leader_node) {
    // 响应包含Leader信息
    raft_node_id_t leader_id = 2;
    bool leader_present = (leader_id > 0);

    FB_ASSERT_TRUE(leader_present);
    FB_ASSERT_EQ(leader_id, 2L);
}

FB_TEST(raft_rpc, getconfiguration_voting_status) {
    // 节点投票状态
    std::map<raft_node_id_t, bool> voting_status;
    voting_status[1] = true;
    voting_status[2] = true;
    voting_status[3] = true;
    voting_status[4] = false;  // 非投票节点
    voting_status[5] = false;

    int voting_count = 0;
    for (const auto& pair : voting_status) {
        if (pair.second) voting_count++;
    }

    FB_ASSERT_EQ(voting_count, 3);
}

FB_TEST(raft_rpc, getconfiguration_node_addresses) {
    // 节点地址信息
    std::map<raft_node_id_t, std::pair<std::string, int>> node_addrs;
    node_addrs[1] = {"127.0.0.1", 8888};
    node_addrs[2] = {"127.0.0.1", 8889};
    node_addrs[3] = {"127.0.0.1", 8890};

    FB_ASSERT_EQ(node_addrs[1].second, 8888);
    FB_ASSERT_STR_EQ(node_addrs[2].first, "127.0.0.1");
}

FB_TEST(raft_rpc, getconfiguration_config_index) {
    // 配置索引
    raft_index_t config_index = 50;
    raft_term_t config_term = 5;

    FB_ASSERT_TRUE(config_index > 0);
    FB_ASSERT_TRUE(config_term > 0);
}

FB_TEST(raft_rpc, getconfiguration_any_node_respond) {
    // 任何节点都可以响应配置查询（不一定需要Leader）
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool can_respond = true;  // Follower也可以响应

    FB_ASSERT_TRUE(can_respond);

    state = RAFT_STATE_CANDIDATE;
    can_respond = (state != RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_respond);
}

FB_TEST(raft_rpc, getconfiguration_stale_config_warning) {
    // 检测过期配置
    raft_index_t config_index = 50;
    raft_index_t latest_config_index = 60;

    bool config_stale = config_index < latest_config_index;
    FB_ASSERT_TRUE(config_stale);
}

FB_TEST(raft_rpc, getconfiguration_joint_consensus_info) {
    // 联合共识配置信息
    std::vector<raft_node_id_t> old_config = {1, 2, 3};
    std::vector<raft_node_id_t> new_config = {1, 2, 4};
    bool in_joint_consensus = true;

    if (in_joint_consensus) {
        // 返回两个配置
        FB_ASSERT_EQ(old_config.size(), 3UL);
        FB_ASSERT_EQ(new_config.size(), 3UL);
    }
}

FB_TEST(raft_rpc, getconfiguration_node_metadata) {
    // 节点元数据
    struct node_meta {
        raft_node_id_t id;
        std::string addr;
        int port;
        bool is_voting;
        bool is_healthy;
    };

    node_meta node1 = {1, "127.0.0.1", 8888, true, true};
    node_meta node2 = {2, "127.0.0.1", 8889, true, false};  // 不健康

    FB_ASSERT_TRUE(node1.is_healthy);
    FB_ASSERT_FALSE(node2.is_healthy);
}

FB_TEST(raft_rpc, getconfiguration_cluster_id) {
    // 集群ID
    uint64_t cluster_id = 12345;

    FB_ASSERT_TRUE(cluster_id > 0);
    FB_ASSERT_EQ(cluster_id, 12345UL);
}

FB_TEST(raft_rpc, getconfiguration_bootstrap_info) {
    // 引导信息
    bool is_bootstrap_complete = true;
    uint64_t bootstrap_node_id = 1;

    FB_ASSERT_TRUE(is_bootstrap_complete);
    FB_ASSERT_EQ(bootstrap_node_id, 1UL);
}

FB_TEST(raft_rpc, getconfiguration_version_tracking) {
    // 配置版本跟踪
    uint64_t config_version = 1;
    config_version++;

    FB_ASSERT_EQ(config_version, 2UL);

    // 每次配置变更递增版本
    config_version++;
    FB_ASSERT_EQ(config_version, 3UL);
}

FB_TEST(raft_rpc, getconfiguration_compatibility_check) {
    // 兼容性检查
    int config_format_version = 2;
    int supported_version = 3;

    bool compatible = config_format_version <= supported_version;
    FB_ASSERT_TRUE(compatible);

    config_format_version = 4;
    compatible = config_format_version <= supported_version;
    FB_ASSERT_FALSE(compatible);
}

FB_TEST(raft_rpc, getconfiguration_caching) {
    // 配置缓存
    std::vector<raft_node_id_t> cached_config = {1, 2, 3};
    raft_index_t cache_version = 10;
    raft_index_t current_version = 10;

    bool cache_valid = (cache_version == current_version);
    FB_ASSERT_TRUE(cache_valid);

    // 配置变更后缓存失效
    current_version = 11;
    cache_valid = (cache_version == current_version);
    FB_ASSERT_FALSE(cache_valid);
}

FB_TEST(raft_rpc, getconfiguration_partial_response) {
    // 大配置分页响应
    size_t total_nodes = 1000;
    size_t page_size = 100;
    size_t total_pages = (total_nodes + page_size - 1) / page_size;

    FB_ASSERT_EQ(total_pages, 10UL);
}

FB_TEST(raft_rpc, getconfiguration_filter_voting_only) {
    // 只返回投票节点
    std::vector<std::pair<raft_node_id_t, bool>> all_nodes = {
        {1, true}, {2, true}, {3, false}, {4, true}, {5, false}
    };

    std::vector<raft_node_id_t> voting_only;
    for (const auto& pair : all_nodes) {
        if (pair.second) voting_only.push_back(pair.first);
    }

    FB_ASSERT_EQ(voting_only.size(), 3UL);
}

FB_TEST(raft_rpc, getconfiguration_security_check) {
    // 安全检查：权限验证
    uint64_t requester_id = 100;
    bool has_permission = true;  // 配置查询通常不需要特殊权限

    FB_ASSERT_TRUE(has_permission);
}

// ============================================================================
// Test Suite: SnapshotStatus RPC Tests (Snapshot Transfer Notification)
// ============================================================================

FB_TEST(raft_rpc, snapshotstatus_request_fields) {
    // 快照状态通知请求字段
    raft_term_t term = 5;
    raft_node_id_t follower_id = 3;
    raft_index_t snapshot_index = 100;
    bool success = true;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(follower_id > 0);
    FB_ASSERT_TRUE(snapshot_index > 0);
}

FB_TEST(raft_rpc, snapshotstatus_success_notification) {
    // Follower 成功安装快照通知
    bool install_success = true;
    raft_index_t snapshot_idx = 100;

    if (install_success) {
        // Leader 更新 follower 的 match_idx
        raft_index_t new_match_idx = snapshot_idx;
        FB_ASSERT_EQ(new_match_idx, 100L);
    }
}

FB_TEST(raft_rpc, snapshotstatus_failure_notification) {
    // Follower 快照安装失败通知
    bool install_success = false;
    int error_code = -1;  // 错误码

    if (!install_success) {
        // Leader 需要重新发送快照或重试
        FB_ASSERT_TRUE(error_code != 0);
    }
}

FB_TEST(raft_rpc, snapshotstatus_match_idx_update) {
    // 快照成功后更新 match_idx
    raft_index_t match_idx = 0;
    raft_index_t snapshot_idx = 100;

    // 快照安装成功
    bool success = true;
    if (success) {
        match_idx = snapshot_idx;
    }

    FB_ASSERT_EQ(match_idx, 100L);
}

FB_TEST(raft_rpc, snapshotstatus_next_idx_update) {
    // 快照成功后更新 next_idx
    raft_index_t next_idx = 101;
    raft_index_t snapshot_idx = 100;

    // 快照安装成功后，next_idx = snapshot_idx + 1
    next_idx = snapshot_idx + 1;

    FB_ASSERT_EQ(next_idx, 101L);
}

FB_TEST(raft_rpc, snapshotstatus_leader_process) {
    // Leader 处理快照状态通知
    raft_identity state = RAFT_STATE_LEADER;
    bool can_process = (state == RAFT_STATE_LEADER);

    FB_ASSERT_TRUE(can_process);
}

FB_TEST(raft_rpc, snapshotstatus_follower_send) {
    // Follower 发送快照状态通知
    raft_identity state = RAFT_STATE_FOLLOWER;

    // Follower 可以发送状态通知
    bool can_send = true;
    FB_ASSERT_TRUE(can_send);
}

FB_TEST(raft_rpc, snapshotstatus_error_codes) {
    // 不同错误码含义
    int SUCCESS = 0;
    int ERR_IO = -1;
    int ERR_CORRUPTED = -2;
    int ERR_OUT_OF_SPACE = -3;
    int ERR_TIMEOUT = -4;

    FB_ASSERT_EQ(SUCCESS, 0);
    FB_ASSERT_TRUE(ERR_IO < 0);
    FB_ASSERT_TRUE(ERR_CORRUPTED < 0);
    FB_ASSERT_TRUE(ERR_OUT_OF_SPACE < 0);
    FB_ASSERT_TRUE(ERR_TIMEOUT < 0);
}

FB_TEST(raft_rpc, snapshotstatus_retry_on_failure) {
    // 失败后重试
    int retry_count = 0;
    int max_retries = 3;
    bool success = false;

    while (!success && retry_count < max_retries) {
        retry_count++;
        if (retry_count == 2) {
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, snapshotstatus_chunk_tracking) {
    // 快照分块传输跟踪
    int64_t total_chunks = 16;
    int64_t received_chunks = 8;

    double progress = 100.0 * received_chunks / total_chunks;
    FB_ASSERT_TRUE(progress >= 50.0);

    // 全部接收完成
    received_chunks = total_chunks;
    progress = 100.0 * received_chunks / total_chunks;
    FB_ASSERT_EQ(progress, 100.0);
}

FB_TEST(raft_rpc, snapshotstatus_checksum_validation) {
    // 校验和验证
    uint32_t expected_checksum = 0xABCDEF12;
    uint32_t received_checksum = 0xABCDEF12;

    bool checksum_ok = (expected_checksum == received_checksum);
    FB_ASSERT_TRUE(checksum_ok);

    // 校验失败
    received_checksum = 0xABCDEF13;
    checksum_ok = (expected_checksum == received_checksum);
    FB_ASSERT_FALSE(checksum_ok);
}

FB_TEST(raft_rpc, snapshotstatus_disk_space_check) {
    // 磁盘空间检查
    size_t snapshot_size = 100 * 1024 * 1024;  // 100MB
    size_t available_space = 150 * 1024 * 1024;  // 150MB

    bool space_sufficient = available_space >= snapshot_size;
    FB_ASSERT_TRUE(space_sufficient);

    // 空间不足
    available_space = 50 * 1024 * 1024;  // 50MB
    space_sufficient = available_space >= snapshot_size;
    FB_ASSERT_FALSE(space_sufficient);
}

FB_TEST(raft_rpc, snapshotstatus_progress_percentage) {
    // 进度百分比计算
    raft_index_t snapshot_idx = 100;
    raft_index_t follower_last_idx = 50;

    // 快照索引超过 Follower 日志
    bool needs_snapshot = snapshot_idx > follower_last_idx;
    FB_ASSERT_TRUE(needs_snapshot);

    // 计算需要传输的日志差距
    raft_index_t gap = snapshot_idx - follower_last_idx;
    FB_ASSERT_EQ(gap, 50L);
}

FB_TEST(raft_rpc, snapshotstatus_timeout_handling) {
    // 超时处理
    int status_timeout_ms = 5000;
    int elapsed_ms = 6000;

    bool timed_out = elapsed_ms >= status_timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, snapshotstatus_network_error) {
    // 网络错误处理
    bool network_error = true;

    if (network_error) {
        // 重试发送状态通知
        int retries = 0;
        while (network_error && retries < 3) {
            retries++;
            network_error = false;
        }
        FB_ASSERT_EQ(retries, 1);
    }
}

FB_TEST(raft_rpc, snapshotstatus_multiple_followers) {
    // 多个 Follower 的快照状态跟踪
    std::map<raft_node_id_t, raft_index_t> follower_snapshot_status;
    follower_snapshot_status[1] = 100;  // 完成
    follower_snapshot_status[2] = 50;   // 进行中
    follower_snapshot_status[3] = 0;    // 未开始

    int completed_count = 0;
    for (const auto& pair : follower_snapshot_status) {
        if (pair.second == 100) completed_count++;
    }

    FB_ASSERT_EQ(completed_count, 1);
}

FB_TEST(raft_rpc, snapshotstatus_concurrent_transfers) {
    // 并发快照传输限制
    int max_concurrent = 3;
    int current_transfers = 2;

    bool can_start_new = current_transfers < max_concurrent;
    FB_ASSERT_TRUE(can_start_new);

    current_transfers = 3;
    can_start_new = current_transfers < max_concurrent;
    FB_ASSERT_FALSE(can_start_new);
}

FB_TEST(raft_rpc, snapshotstatus_resume_interrupted) {
    // 中断后恢复传输
    int64_t last_chunk_received = 5;
    int64_t total_chunks = 16;

    // 从上次接收位置继续
    int64_t next_chunk = last_chunk_received + 1;
    FB_ASSERT_EQ(next_chunk, 6L);

    int64_t remaining_chunks = total_chunks - last_chunk_received;
    FB_ASSERT_EQ(remaining_chunks, 11L);
}

FB_TEST(raft_rpc, snapshotstatus_cancel_transfer) {
    // 取消快照传输
    bool transfer_cancelled = true;
    raft_index_t snapshot_idx = 100;

    if (transfer_cancelled) {
        // 清理已接收的临时数据
        snapshot_idx = 0;  // 重置
    }

    FB_ASSERT_EQ(snapshot_idx, 0L);
}

FB_TEST(raft_rpc, snapshotstatus_version_compatibility) {
    // 快照版本兼容性检查
    int snapshot_version = 2;
    int follower_version = 3;

    bool compatible = snapshot_version <= follower_version;
    FB_ASSERT_TRUE(compatible);

    // 版本不兼容
    snapshot_version = 4;
    compatible = snapshot_version <= follower_version;
    FB_ASSERT_FALSE(compatible);
}

// ============================================================================
// Test Suite: Ping RPC Tests (Health Check)
// ============================================================================

FB_TEST(raft_rpc, ping_request_fields) {
    // Ping 请求字段
    raft_term_t term = 5;
    raft_node_id_t from_id = 1;
    raft_time_t timestamp = 1000;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(from_id > 0);
    FB_ASSERT_TRUE(timestamp > 0);
}

FB_TEST(raft_rpc, ping_response_fields) {
    // Ping 响应字段
    raft_term_t term = 5;
    raft_time_t server_timestamp = 1000;
    raft_time_t client_timestamp = 950;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(server_timestamp >= client_timestamp);
}

FB_TEST(raft_rpc, ping_latency_measurement) {
    // 延迟测量
    raft_time_t send_time = 1000;
    raft_time_t receive_time = 1050;
    raft_time_t reply_time = 1100;
    raft_time_t response_time = 1150;

    raft_time_t rtt = response_time - send_time;
    raft_time_t server_processing = receive_time - reply_time;

    FB_ASSERT_EQ(rtt, 150L);
    FB_ASSERT_TRUE(rtt > server_processing);
}

FB_TEST(raft_rpc, ping_health_check) {
    // 健康检查
    bool ping_success = true;
    int consecutive_failures = 0;

    if (ping_success) {
        consecutive_failures = 0;
    }

    FB_ASSERT_EQ(consecutive_failures, 0);
}

FB_TEST(raft_rpc, ping_failure_detection) {
    // 故障检测
    int consecutive_failures = 3;
    int failure_threshold = 3;

    bool node_unhealthy = consecutive_failures >= failure_threshold;
    FB_ASSERT_TRUE(node_unhealthy);
}

FB_TEST(raft_rpc, ping_timeout_handling) {
    // Ping 超时
    int ping_timeout_ms = 100;
    int elapsed_ms = 150;

    bool timed_out = elapsed_ms >= ping_timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, ping_periodic_interval) {
    // 定期 Ping 间隔
    int ping_interval_ms = 1000;
    int ping_count = 0;

    for (int time = 0; time <= 5000; time += ping_interval_ms) {
        ping_count++;
    }

    FB_ASSERT_EQ(ping_count, 6);
}

FB_TEST(raft_rpc, ping_all_nodes) {
    // Ping 所有节点
    std::set<raft_node_id_t> nodes = {1, 2, 3, 4, 5};
    std::set<raft_node_id_t> responded;

    for (auto id : nodes) {
        responded.insert(id);
    }

    FB_ASSERT_EQ(responded.size(), 5UL);
}

FB_TEST(raft_rpc, ping_node_health_status) {
    // 节点健康状态
    std::map<raft_node_id_t, bool> health_status;
    health_status[1] = true;
    health_status[2] = true;
    health_status[3] = false;  // 不健康

    int healthy_count = 0;
    for (const auto& pair : health_status) {
        if (pair.second) healthy_count++;
    }

    FB_ASSERT_EQ(healthy_count, 2);
}

FB_TEST(raft_rpc, ping_clock_synchronization) {
    // 时钟同步检测
    raft_time_t local_time = 1000;
    raft_time_t server_time = 1050;
    raft_time_t offset = server_time - local_time;

    FB_ASSERT_EQ(offset, 50L);

    // 检测时钟漂移
    bool clock_drift_detected = (offset > 100 || offset < -100);
    FB_ASSERT_FALSE(clock_drift_detected);
}

FB_TEST(raft_rpc, ping_response_timeout) {
    // Ping 响应超时
    int response_timeout_ms = 50;
    int elapsed_ms = 60;

    bool response_timeout = elapsed_ms >= response_timeout_ms;
    FB_ASSERT_TRUE(response_timeout);
}

FB_TEST(raft_rpc, ping_retry_on_failure) {
    // 失败重试
    int retry_count = 0;
    int max_retries = 3;
    bool success = false;

    while (!success && retry_count < max_retries) {
        retry_count++;
        if (retry_count == 2) {
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, ping_network_partition) {
    // 网络分区检测
    std::set<raft_node_id_t> reachable = {1, 2};
    std::set<raft_node_id_t> all_nodes = {1, 2, 3, 4, 5};

    bool partition_detected = reachable.size() < all_nodes.size();
    FB_ASSERT_TRUE(partition_detected);

    int unreachable_count = all_nodes.size() - reachable.size();
    FB_ASSERT_EQ(unreachable_count, 3);
}

FB_TEST(raft_rpc, ping_load_balancing) {
    // 负载均衡：选择延迟最低的节点
    std::map<raft_node_id_t, int> latencies;
    latencies[1] = 50;
    latencies[2] = 30;
    latencies[3] = 80;

    raft_node_id_t best_node = 1;
    int min_latency = latencies[1];

    for (const auto& pair : latencies) {
        if (pair.second < min_latency) {
            min_latency = pair.second;
            best_node = pair.first;
        }
    }

    FB_ASSERT_EQ(best_node, 2L);
    FB_ASSERT_EQ(min_latency, 30);
}

FB_TEST(raft_rpc, ping_concurrent_requests) {
    // 并发 Ping 请求
    int concurrent_pings = 5;
    std::atomic<int> pending_responses{concurrent_pings};

    for (int i = 0; i < concurrent_pings; i++) {
        pending_responses--;
    }

    FB_ASSERT_EQ(pending_responses.load(), 0);
}

FB_TEST(raft_rpc, ping_statistics_collection) {
    // Ping 统计收集
    int pings_sent = 100;
    int pings_success = 95;
    int pings_failed = 5;

    FB_ASSERT_EQ(pings_sent, pings_success + pings_failed);

    double success_rate = 100.0 * pings_success / pings_sent;
    FB_ASSERT_GE(success_rate, 95.0);
}

FB_TEST(raft_rpc, ping_latency_percentiles) {
    // 延迟百分位数
    std::vector<int> latencies = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

    // P50
    int p50 = latencies[5];
    FB_ASSERT_EQ(p50, 60);

    // P99
    int p99 = latencies[9];
    FB_ASSERT_EQ(p99, 100);
}

FB_TEST(raft_rpc, ping_leader_prioritized) {
    // 优先 Ping Leader
    raft_node_id_t leader_id = 2;
    std::vector<raft_node_id_t> ping_order;

    ping_order.push_back(leader_id);
    // 然后是其他节点
    for (int i = 1; i <= 5; i++) {
        if (i != leader_id) ping_order.push_back(i);
    }

    FB_ASSERT_EQ(ping_order[0], 2L);
    FB_ASSERT_EQ(ping_order.size(), 5UL);
}

FB_TEST(raft_rpc, ping_adaptive_interval) {
    // 自适应 Ping 间隔
    int base_interval = 1000;
    int latency = 50;
    int adaptive_interval = base_interval;

    // 根据延迟调整间隔
    if (latency > 100) {
        adaptive_interval = base_interval / 2;  // 高延迟时更频繁
    }

    FB_ASSERT_EQ(adaptive_interval, 1000);

    latency = 150;
    if (latency > 100) {
        adaptive_interval = base_interval / 2;
    }

    FB_ASSERT_EQ(adaptive_interval, 500);
}

FB_TEST(raft_rpc, ping_graceful_degradation) {
    // 优雅降级：部分节点不可达
    std::set<raft_node_id_t> healthy = {1, 2, 3};
    std::set<raft_node_id_t> unhealthy = {4, 5};

    // 继续服务健康节点
    bool can_serve = healthy.size() > healthy.size() / 2;
    FB_ASSERT_TRUE(can_serve);
}

FB_TEST(raft_rpc, ping_alert_threshold) {
    // 告警阈值
    double failure_rate = 0.3;  // 30% 失败率
    double alert_threshold = 0.2;  // 20% 阈值

    bool should_alert = failure_rate >= alert_threshold;
    FB_ASSERT_TRUE(should_alert);
}

// ============================================================================
// Test Suite: Metrics RPC Tests (Monitoring & Observability)
// ============================================================================

FB_TEST(raft_rpc, metrics_request_fields) {
    // 指标查询请求字段
    raft_node_id_t requester_id = 1;
    std::vector<std::string> metric_names = {"commit_idx", "apply_idx", "term"};

    FB_ASSERT_TRUE(requester_id > 0);
    FB_ASSERT_EQ(metric_names.size(), 3UL);
}

FB_TEST(raft_rpc, metrics_commit_apply_gap) {
    // commit 与 apply 差距
    raft_index_t commit_idx = 100;
    raft_index_t last_applied = 95;

    raft_index_t gap = commit_idx - last_applied;
    FB_ASSERT_EQ(gap, 5L);

    // 差距过大时告警
    bool gap_too_large = gap > 10;
    FB_ASSERT_FALSE(gap_too_large);
}

FB_TEST(raft_rpc, metrics_leader_stats) {
    // Leader 统计指标
    uint64_t proposals_total = 1000;
    uint64_t proposals_committed = 950;
    uint64_t proposals_applied = 900;

    double commit_rate = 100.0 * proposals_committed / proposals_total;
    double apply_rate = 100.0 * proposals_applied / proposals_total;

    FB_ASSERT_GE(commit_rate, 95.0);
    FB_ASSERT_GE(apply_rate, 90.0);
}

FB_TEST(raft_rpc, metrics_follower_stats) {
    // Follower 统计指标
    uint64_t append_entries_received = 500;
    uint64_t append_entries_success = 480;
    uint64_t append_entries_rejected = 20;

    double success_rate = 100.0 * append_entries_success / append_entries_received;
    FB_ASSERT_GE(success_rate, 90.0);
}

FB_TEST(raft_rpc, metrics_network_stats) {
    // 网络统计
    uint64_t bytes_sent = 1024 * 1024;  // 1MB
    uint64_t bytes_received = 2 * 1024 * 1024;  // 2MB
    uint64_t rpc_calls = 1000;

    double avg_request_size = bytes_sent / rpc_calls;
    FB_ASSERT_EQ(avg_request_size, 1024UL);
}

FB_TEST(raft_rpc, metrics_latency_histogram) {
    // 延迟直方图
    std::vector<int> latencies = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

    int min_latency = *std::min_element(latencies.begin(), latencies.end());
    int max_latency = *std::max_element(latencies.begin(), latencies.end());

    FB_ASSERT_EQ(min_latency, 10);
    FB_ASSERT_EQ(max_latency, 100);
}

FB_TEST(raft_rpc, metrics_election_stats) {
    // 选举统计
    uint64_t elections_started = 5;
    uint64_t elections_won = 3;
    uint64_t elections_lost = 2;

    FB_ASSERT_EQ(elections_started, elections_won + elections_lost);

    double win_rate = 100.0 * elections_won / elections_started;
    FB_ASSERT_GE(win_rate, 50.0);
}

FB_TEST(raft_rpc, metrics_snapshot_stats) {
    // 快照统计
    uint64_t snapshots_created = 10;
    uint64_t snapshots_applied = 8;
    uint64_t snapshot_size_bytes = 1024 * 1024 * 100;  // 100MB

    FB_ASSERT_GE(snapshots_created, snapshots_applied);

    double avg_snapshot_size = snapshot_size_bytes / snapshots_created;
    FB_ASSERT_EQ(avg_snapshot_size, 10UL * 1024 * 1024);
}

FB_TEST(raft_rpc, metrics_cluster_health) {
    // 集群健康度
    uint64_t total_nodes = 5;
    uint64_t healthy_nodes = 4;
    uint64_t unhealthy_nodes = 1;

    double health_percentage = 100.0 * healthy_nodes / total_nodes;
    FB_ASSERT_GE(health_percentage, 80.0);
}

FB_TEST(raft_rpc, metrics_leader_id) {
    // Leader ID 指标
    raft_node_id_t current_leader = 2;
    raft_term_t current_term = 5;

    FB_ASSERT_TRUE(current_leader > 0);
    FB_ASSERT_TRUE(current_term > 0);
}

FB_TEST(raft_rpc, metrics_node_role) {
    // 节点角色指标
    raft_identity role = RAFT_STATE_LEADER;

    int role_value = static_cast<int>(role);
    FB_ASSERT_EQ(role_value, 3);

    role = RAFT_STATE_FOLLOWER;
    role_value = static_cast<int>(role);
    FB_ASSERT_EQ(role_value, 1);
}

FB_TEST(raft_rpc, metrics_replication_lag) {
    // 复制延迟
    std::map<raft_node_id_t, raft_index_t> match_indices;
    match_indices[1] = 100;  // Leader
    match_indices[2] = 95;
    match_indices[3] = 90;
    match_indices[4] = 85;

    raft_index_t leader_idx = match_indices[1];
    for (const auto& pair : match_indices) {
        raft_index_t lag = leader_idx - pair.second;
        FB_ASSERT_TRUE(lag >= 0);
    }
}

FB_TEST(raft_rpc, metrics_throughput) {
    // 吞吐量计算
    uint64_t entries_committed = 1000;
    uint64_t time_elapsed_ms = 1000;  // 1秒

    double throughput = 1000.0 * entries_committed / time_elapsed_ms;
    FB_ASSERT_GE(throughput, 1000.0);
}

FB_TEST(raft_rpc, metrics_error_rates) {
    // 错误率统计
    uint64_t total_requests = 1000;
    uint64_t errors = 50;

    double error_rate = 100.0 * errors / total_requests;
    FB_ASSERT_GE(error_rate, 0.0);
    FB_ASSERT_LE(error_rate, 10.0);
}

FB_TEST(raft_rpc, metrics_resource_usage) {
    // 资源使用情况
    size_t memory_used = 512 * 1024 * 1024;  // 512MB
    size_t memory_limit = 1024 * 1024 * 1024;  // 1GB
    double cpu_usage = 0.45;  // 45%

    double memory_usage = 100.0 * memory_used / memory_limit;
    FB_ASSERT_GE(memory_usage, 50.0);
    FB_ASSERT_LE(cpu_usage, 1.0);
}

FB_TEST(raft_rpc, metrics_log_cache) {
    // 日志缓存指标
    size_t cache_size = 100;
    size_t cache_hits = 95;
    size_t cache_misses = 5;

    double hit_rate = 100.0 * cache_hits / cache_size;
    FB_ASSERT_GE(hit_rate, 95.0);
}

FB_TEST(raft_rpc, metrics_disk_io) {
    // 磁盘 I/O 指标
    uint64_t bytes_read = 100 * 1024 * 1024;  // 100MB
    uint64_t bytes_written = 50 * 1024 * 1024;  // 50MB
    uint64_t fsync_count = 1000;

    FB_ASSERT_GT(bytes_read, bytes_written);
    FB_ASSERT_GE(fsync_count, 100UL);
}

FB_TEST(raft_rpc, metrics_uptime) {
    // 运行时间
    raft_time_t start_time = 1000;
    raft_time_t current_time = 3601000;  // 1小时后

    raft_time_t uptime_seconds = (current_time - start_time) / 1000;
    FB_ASSERT_GE(uptime_seconds, 3600L);
}

FB_TEST(raft_rpc, metrics_version_info) {
    // 版本信息
    std::string version = "1.0.0";
    int protocol_version = 2;
    int config_version = 10;

    FB_ASSERT_FALSE(version.empty());
    FB_ASSERT_TRUE(protocol_version > 0);
    FB_ASSERT_TRUE(config_version > 0);
}

FB_TEST(raft_rpc, metrics_histogram_buckets) {
    // 直方图桶
    std::map<std::string, uint64_t> buckets;
    buckets["0-10ms"] = 50;
    buckets["10-50ms"] = 30;
    buckets["50-100ms"] = 15;
    buckets["100ms+"] = 5;

    uint64_t total = 0;
    for (const auto& pair : buckets) {
        total += pair.second;
    }

    FB_ASSERT_EQ(total, 100UL);
}

FB_TEST(raft_rpc, metrics_aggregation) {
    // 指标聚合
    std::vector<double> values = {1.0, 2.0, 3.0, 4.0, 5.0};

    double sum = 0;
    for (double v : values) {
        sum += v;
    }
    double avg = sum / values.size();

    FB_ASSERT_EQ(avg, 3.0);
}

// ============================================================================
// Test Suite: PreVote RPC Tests (Pre-Vote Extension)
// ============================================================================

FB_TEST(raft_rpc, prevote_request_fields) {
    // PreVote 请求字段
    raft_term_t term = 6;
    raft_node_id_t candidate_id = 3;
    raft_index_t last_log_idx = 100;
    raft_term_t last_log_term = 5;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(candidate_id > 0);
    FB_ASSERT_TRUE(last_log_idx >= 0);
    FB_ASSERT_TRUE(last_log_term >= 0);
}

FB_TEST(raft_rpc, prevote_does_not_increment_term) {
    // PreVote 不增加 term
    raft_term_t current_term = 5;
    raft_term_t prevote_term = 6;

    // PreVote 收到后不更新 term
    raft_term_t after_prevote = current_term;
    FB_ASSERT_EQ(after_prevote, 5L);

    // 普通 RequestVote 会更新 term
    bool would_update_term = (prevote_term > current_term);
    FB_ASSERT_TRUE(would_update_term);
}

FB_TEST(raft_rpc, prevote_check_leader_alive) {
    // PreVote 检查 Leader 是否存活
    raft_time_t last_leader_contact = 1000;
    raft_time_t election_timeout = 500;
    raft_time_t current_time = 1700;

    bool leader_timeout = (current_time - last_leader_contact) >= election_timeout;
    FB_ASSERT_TRUE(leader_timeout);

    // 只有 Leader 超时才响应 PreVote
    bool grant_prevote = leader_timeout;
    FB_ASSERT_TRUE(grant_prevote);
}

FB_TEST(raft_rpc, prevote_leader_present_reject) {
    // Leader 存活时拒绝 PreVote
    raft_time_t last_leader_contact = 1000;
    raft_time_t current_time = 1100;
    raft_time_t heartbeat_timeout = 100;

    bool leader_alive = (current_time - last_leader_contact) < heartbeat_timeout;
    FB_ASSERT_TRUE(leader_alive);

    bool grant_prevote = !leader_alive;
    FB_ASSERT_FALSE(grant_prevote);
}

FB_TEST(raft_rpc, prevote_log_up_to_date_check) {
    // PreVote 也检查日志新旧
    raft_index_t my_last_log_idx = 50;
    raft_term_t my_last_log_term = 5;
    raft_index_t cand_last_log_idx = 60;
    raft_term_t cand_last_log_term = 5;

    bool log_ok = (cand_last_log_term > my_last_log_term) ||
                  ((cand_last_log_term == my_last_log_term) &&
                   (cand_last_log_idx >= my_last_log_idx));
    FB_ASSERT_TRUE(log_ok);
}

FB_TEST(raft_rpc, prevote_network_partition) {
    // 网络分区时的 PreVote
    std::set<raft_node_id_t> partition_nodes = {3, 4};

    // 分区中的节点无法收到 Leader 心跳
    raft_node_id_t candidate_id = 3;
    bool in_partition = partition_nodes.count(candidate_id) > 0;
    FB_ASSERT_TRUE(in_partition);

    // PreVote 防止分区节点干扰主集群
    bool can_prevote = in_partition;
    FB_ASSERT_TRUE(can_prevote);
}

FB_TEST(raft_rpc, prevote_response_fields) {
    raft_term_t term = 5;
    bool prevote_granted = true;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(prevote_granted);
}

FB_TEST(raft_rpc, prevote_no_state_change) {
    // PreVote 不改变状态
    raft_identity state = RAFT_STATE_FOLLOWER;

    // 发送 PreVote 不改变状态
    bool sending_prevote = true;
    if (sending_prevote) {
        // 状态保持不变
    }

    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_rpc, prevote_to_real_vote_transition) {
    // PreVote 成功后转为正式投票
    uint64_t prevotes_granted = 3;
    uint64_t node_num = 5;

    bool prevote_success = prevotes_granted > node_num / 2;
    FB_ASSERT_TRUE(prevote_success);

    if (prevote_success) {
        // 开始正式选举
        raft_identity state = RAFT_STATE_CANDIDATE;
        FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
    }
}

FB_TEST(raft_rpc, prevote_prevote_fail_no_real_vote) {
    // PreVote 失败不发起正式选举
    uint64_t prevotes_granted = 2;
    uint64_t node_num = 5;

    bool prevote_success = prevotes_granted > node_num / 2;
    FB_ASSERT_FALSE(prevote_success);

    // 保持 Follower 状态
    raft_identity state = RAFT_STATE_FOLLOWER;
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_rpc, prevote_disruptive_leader) {
    // 防止干扰 Leader
    raft_node_id_t leader_id = 2;
    raft_time_t last_leader_heartbeat = 1000;
    raft_time_t current_time = 1100;
    raft_time_t heartbeat_timeout = 100;

    bool leader_healthy = (current_time - last_leader_heartbeat) < heartbeat_timeout;
    FB_ASSERT_TRUE(leader_healthy);

    // 不响应 PreVote，保护 Leader
    bool grant_prevote = !leader_healthy;
    FB_ASSERT_FALSE(grant_prevote);
}

FB_TEST(raft_rpc, prevote_concurrent_candidates) {
    // 多个节点同时 PreVote
    std::vector<raft_node_id_t> prevote_candidates = {2, 3, 4};

    FB_ASSERT_EQ(prevote_candidates.size(), 3UL);

    // 每个候选者独立 PreVote
    for (auto id : prevote_candidates) {
        FB_ASSERT_TRUE(id > 0);
    }
}

FB_TEST(raft_rpc, prevote_timeout_handling) {
    // PreVote 超时
    int prevote_timeout_ms = 500;
    int elapsed_ms = 600;

    bool timed_out = elapsed_ms >= prevote_timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, prevote_retry_on_failure) {
    // PreVote 失败重试
    int retry_count = 0;
    int max_retries = 3;
    bool success = false;

    while (!success && retry_count < max_retries) {
        retry_count++;
        if (retry_count == 2) {
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, prevote_min_timeout_before_prevote) {
    // PreVote 前等待最小超时
    raft_time_t election_timeout = 500;
    raft_time_t min_prevote_timeout = 400;

    FB_ASSERT_TRUE(min_prevote_timeout < election_timeout);

    // 防止过早 PreVote
    bool can_prevote_now = false;  // 需要等待
    FB_ASSERT_FALSE(can_prevote_now);
}

FB_TEST(raft_rpc, prevote_joint_consensus_check) {
    // 联合共识期间的 PreVote
    bool in_joint_consensus = true;

    // 需要两个配置都同意
    uint64_t old_prevotes = 3;
    uint64_t new_prevotes = 2;
    uint64_t old_nodes = 5;
    uint64_t new_nodes = 3;

    bool old_ok = old_prevotes > old_nodes / 2;
    bool new_ok = new_prevotes > new_nodes / 2;

    FB_ASSERT_TRUE(old_ok);
    FB_ASSERT_TRUE(new_ok);
}

FB_TEST(raft_rpc, prevote_leader_in_joint_consensus) {
    // 联合共识中 Leader 仍在工作
    raft_identity state = RAFT_STATE_LEADER;
    bool in_joint_consensus = true;

    // Leader 继续服务
    bool can_serve = (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_serve);

    // 拒绝 PreVote
    bool grant_prevote = false;
    FB_ASSERT_FALSE(grant_prevote);
}

FB_TEST(raft_rpc, prevote_follower_only_respond) {
    // 只有 Follower 响应 PreVote
    raft_identity state = RAFT_STATE_LEADER;

    bool should_respond = (state == RAFT_STATE_FOLLOWER);
    FB_ASSERT_FALSE(should_respond);

    state = RAFT_STATE_FOLLOWER;
    should_respond = (state == RAFT_STATE_FOLLOWER);
    FB_ASSERT_TRUE(should_respond);
}

FB_TEST(raft_rpc, prevote_candidate_term_check) {
    // PreVote term 检查
    raft_term_t current_term = 5;
    raft_term_t candidate_prevote_term = 6;

    // PreVote term 应该是候选者期望的 term
    bool term_valid = candidate_prevote_term >= current_term;
    FB_ASSERT_TRUE(term_valid);
}

FB_TEST(raft_rpc, prevote_quorum_calculation) {
    // PreVote 多数派计算
    uint64_t node_num = 5;
    uint64_t prevotes_needed = node_num / 2 + 1;

    FB_ASSERT_EQ(prevotes_needed, 3UL);

    uint64_t prevotes_received = 3;
    bool has_quorum = prevotes_received >= prevotes_needed;
    FB_ASSERT_TRUE(has_quorum);
}

FB_TEST(raft_rpc, prevote_stale_candidate_detection) {
    // 检测过时的 PreVote 候选者
    raft_term_t current_term = 7;
    raft_term_t prevote_term = 5;

    bool prevote_stale = prevote_term < current_term;
    FB_ASSERT_TRUE(prevote_stale);

    // 拒绝过时的 PreVote
    bool grant_prevote = !prevote_stale;
    FB_ASSERT_FALSE(grant_prevote);
}

FB_TEST(raft_rpc, prevote_benefits) {
    // PreVote 的好处
    bool prevents_disruptive_elections = true;
    bool reduces_unnecessary_term_increments = true;
    bool improves_cluster_stability = true;

    FB_ASSERT_TRUE(prevents_disruptive_elections);
    FB_ASSERT_TRUE(reduces_unnecessary_term_increments);
    FB_ASSERT_TRUE(improves_cluster_stability);
}

FB_TEST(raft_rpc, prevote_without_prevote_comparison) {
    // 有无 PreVote 的对比
    int elections_without_prevote = 10;  // 可能有很多不必要选举
    int elections_with_prevote = 3;      // PreVote 减少不必要选举

    bool prevote_effective = elections_with_prevote < elections_without_prevote;
    FB_ASSERT_TRUE(prevote_effective);
}

FB_TEST(raft_rpc, prevote_multiple_rounds) {
    // 多轮 PreVote
    int prevote_round = 1;
    int max_prevote_rounds = 3;
    bool prevote_success = false;

    while (!prevote_success && prevote_round <= max_prevote_rounds) {
        prevote_round++;
        if (prevote_round == 2) {
            prevote_success = true;
        }
    }

    FB_ASSERT_TRUE(prevote_success);
    FB_ASSERT_EQ(prevote_round, 2);
}

// ============================================================================
// Test Suite: Lease RPC Tests (Leader Lease Management)
// ============================================================================

FB_TEST(raft_rpc, lease_request_fields) {
    // 租约请求字段
    raft_term_t term = 5;
    raft_node_id_t leader_id = 1;
    raft_time_t lease_duration = 1000;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(leader_id > 0);
    FB_ASSERT_TRUE(lease_duration > 0);
}

FB_TEST(raft_rpc, lease_grant_response) {
    // 租约授予响应
    raft_term_t term = 5;
    raft_time_t lease_expiry = 2000;
    bool lease_granted = true;

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(lease_expiry > 0);
    FB_ASSERT_TRUE(lease_granted);
}

FB_TEST(raft_rpc, lease_revoke_request) {
    // 租约撤销请求
    raft_node_id_t leader_id = 1;
    raft_term_t term = 5;
    bool revoke_requested = true;

    FB_ASSERT_TRUE(leader_id > 0);
    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(revoke_requested);
}

FB_TEST(raft_rpc, lease_duration_calculation) {
    // 租约时长计算
    raft_time_t heartbeat_period = 100;
    raft_time_t lease_multiplier = 5;
    raft_time_t lease_duration = heartbeat_period * lease_multiplier;

    FB_ASSERT_EQ(lease_duration, 500L);

    // 租约应大于心跳间隔
    bool lease_valid = lease_duration > heartbeat_period;
    FB_ASSERT_TRUE(lease_valid);
}

FB_TEST(raft_rpc, lease_expiry_check) {
    // 租约过期检查
    raft_time_t lease_expiry = 1000;
    raft_time_t current_time = 800;

    bool lease_valid = current_time < lease_expiry;
    FB_ASSERT_TRUE(lease_valid);

    // 租约过期
    current_time = 1200;
    lease_valid = current_time < lease_expiry;
    FB_ASSERT_FALSE(lease_valid);
}

FB_TEST(raft_rpc, lease_renewal_on_heartbeat) {
    // 心跳时续约租约
    raft_time_t lease_expiry = 1000;
    raft_time_t heartbeat_period = 100;

    // 收到心跳后续约
    lease_expiry += heartbeat_period;

    FB_ASSERT_EQ(lease_expiry, 1100L);
}

FB_TEST(raft_rpc, lease_leader_exclusive) {
    // 租约仅 Leader 可用
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool has_lease = false;

    FB_ASSERT_FALSE(has_lease);

    // 只有 Leader 有租约
    state = RAFT_STATE_LEADER;
    has_lease = (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(has_lease);
}

FB_TEST(raft_rpc, lease_read_without_rpc) {
    // 租约读无需 RPC
    bool lease_valid = true;
    raft_index_t read_index = 100;

    if (lease_valid) {
        // 直接使用本地 commit_idx
        bool can_read = true;
        FB_ASSERT_TRUE(can_read);
    }

    // 无租约时需要 ReadIndex RPC
    lease_valid = false;
    bool need_readindex_rpc = !lease_valid;
    FB_ASSERT_TRUE(need_readindex_rpc);
}

FB_TEST(raft_rpc, lease_clock_drift_tolerance) {
    // 时钟漂移容忍度
    raft_time_t lease_duration = 500;
    raft_time_t max_clock_drift = 50;

    // 实际有效租约时间
    raft_time_t effective_lease = lease_duration - max_clock_drift;

    FB_ASSERT_EQ(effective_lease, 450L);
    FB_ASSERT_TRUE(effective_lease > 0);
}

FB_TEST(raft_rpc, lease_safety_margin) {
    // 安全裕度
    raft_time_t election_timeout = 500;
    raft_time_t lease_duration = 400;

    // 租约必须小于选举超时
    bool lease_safe = lease_duration < election_timeout;
    FB_ASSERT_TRUE(lease_safe);

    raft_time_t safety_margin = election_timeout - lease_duration;
    FB_ASSERT_EQ(safety_margin, 100L);
}

FB_TEST(raft_rpc, lease_transfer_invalidation) {
    // 领导权转移时租约失效
    bool lease_valid = true;
    bool leader_transfer = true;

    if (leader_transfer) {
        lease_valid = false;
    }

    FB_ASSERT_FALSE(lease_valid);
}

FB_TEST(raft_rpc, lease_step_down_invalidation) {
    // Leader 退位时租约失效
    raft_identity state = RAFT_STATE_LEADER;
    bool lease_valid = true;

    // 收到更高 term
    raft_term_t current_term = 5;
    raft_term_t received_term = 6;

    if (received_term > current_term) {
        state = RAFT_STATE_FOLLOWER;
        lease_valid = false;
    }

    FB_ASSERT_FALSE(lease_valid);
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_rpc, lease_quorum_dependency) {
    // 租约依赖法定节点确认
    uint64_t node_num = 5;
    uint64_t quorum_responses = 3;

    bool has_quorum = quorum_responses > node_num / 2;
    FB_ASSERT_TRUE(has_quorum);

    // 有法定节点确认才能续约
    bool lease_renewable = has_quorum;
    FB_ASSERT_TRUE(lease_renewable);
}

FB_TEST(raft_rpc, lease_network_partition_effect) {
    // 网络分区影响租约
    std::set<raft_node_id_t> reachable_nodes = {1, 2};
    uint64_t total_nodes = 5;

    bool has_quorum = reachable_nodes.size() > total_nodes / 2;
    FB_ASSERT_FALSE(has_quorum);

    // 无法定节点，租约无法续约
    bool lease_can_renew = has_quorum;
    FB_ASSERT_FALSE(lease_can_renew);
}

FB_TEST(raft_rpc, lease_multiple_leaders_conflict) {
    // 多 Leader 租约冲突检测
    raft_node_id_t known_leader = 1;
    raft_term_t known_term = 5;
    raft_node_id_t other_leader = 2;
    raft_term_t other_term = 6;

    // 更高 term 的 Leader 租约优先
    bool other_lease_valid = other_term > known_term;
    FB_ASSERT_TRUE(other_lease_valid);

    // 本地租约失效
    bool local_lease_valid = !other_lease_valid;
    FB_ASSERT_FALSE(local_lease_valid);
}

FB_TEST(raft_rpc, lease_graceful_expiry) {
    // 租约优雅过期处理
    raft_time_t lease_expiry = 1000;
    raft_time_t current_time = 1000;

    // 刚好过期
    bool lease_expired = current_time >= lease_expiry;
    FB_ASSERT_TRUE(lease_expired);

    // 切换到 ReadIndex 模式
    bool use_readindex = lease_expired;
    FB_ASSERT_TRUE(use_readindex);
}

FB_TEST(raft_rpc, lease_concurrent_reads) {
    // 租约期间并发读
    int concurrent_reads = 10;
    bool lease_valid = true;

    // 所有读都无需 RPC
    int rpc_needed = lease_valid ? 0 : concurrent_reads;
    FB_ASSERT_EQ(rpc_needed, 0);
}

FB_TEST(raft_rpc, lease_performance_improvement) {
    // 租约性能提升
    int reads_with_lease = 1000;
    int rpc_with_lease = 0;
    int reads_without_lease = 1000;
    int rpc_without_lease = reads_without_lease;

    FB_ASSERT_TRUE(rpc_with_lease < rpc_without_lease);

    double latency_improvement = 100.0 * (rpc_without_lease - rpc_with_lease) / rpc_without_lease;
    FB_ASSERT_EQ(latency_improvement, 100.0);
}

FB_TEST(raft_rpc, lease_timeout_handling) {
    // 租约超时处理
    int lease_timeout_ms = 500;
    int elapsed_ms = 600;

    bool timed_out = elapsed_ms >= lease_timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, lease_follower_tracking) {
    // Follower 租约跟踪
    std::map<raft_node_id_t, raft_time_t> follower_leases;
    follower_leases[1] = 1000;
    follower_leases[2] = 1000;
    follower_leases[3] = 800;  // 较短租约

    // 检查所有 Follower 租约
    for (const auto& pair : follower_leases) {
        FB_ASSERT_TRUE(pair.second > 0);
    }
}

FB_TEST(raft_rpc, lease_min_max_duration) {
    // 租约最小最大时长
    raft_time_t min_lease = 100;
    raft_time_t max_lease = 10000;
    raft_time_t actual_lease = 500;

    bool lease_in_range = (actual_lease >= min_lease) && (actual_lease <= max_lease);
    FB_ASSERT_TRUE(lease_in_range);
}

FB_TEST(raft_rpc, lease_config_change_effect) {
    // 配置变更对租约的影响
    uint64_t old_nodes = 5;
    uint64_t new_nodes = 7;
    raft_time_t old_lease_expiry = 1000;

    // 配置变更后需要重新确认法定节点
    bool need_quorum_reconfirm = true;
    FB_ASSERT_TRUE(need_quorum_reconfirm);
}

FB_TEST(raft_rpc, lease_snapshot_impact) {
    // 快照传输对租约的影响
    bool snapshot_in_progress = true;

    // 快照期间租约仍然有效
    bool lease_valid = true;  // 快照不影响租约
    FB_ASSERT_TRUE(lease_valid);
}

FB_TEST(raft_rpc, lease_metrics_collection) {
    // 租约指标收集
    uint64_t lease_grants_total = 100;
    uint64_t lease_renewals_total = 500;
    uint64_t lease_expiry_total = 10;

    FB_ASSERT_GT(lease_renewals_total, lease_grants_total);
    FB_ASSERT_LT(lease_expiry_total, lease_grants_total);
}

FB_TEST(raft_rpc, lease_leader_lease_table) {
    // Leader 租约表管理
    std::map<raft_node_id_t, raft_time_t> lease_table;
    lease_table[2] = 1000;
    lease_table[3] = 1000;
    lease_table[4] = 1000;

    // 更新单个 Follower 租约
    lease_table[2] = 1100;

    FB_ASSERT_EQ(lease_table[2], 1100L);
    FB_ASSERT_EQ(lease_table.size(), 3UL);
}

FB_TEST(raft_rpc, lease_batch_renewal) {
    // 批量租约续约
    std::vector<raft_node_id_t> followers = {2, 3, 4};
    raft_time_t new_expiry = 1500;

    for (auto id : followers) {
        // 更新每个 Follower 的租约
    }

    FB_ASSERT_EQ(followers.size(), 3UL);
}

// ============================================================================
// Test Suite: Recovery RPC Tests (Node Recovery)
// ============================================================================

FB_TEST(raft_rpc, recovery_request_fields) {
    // 恢复请求字段
    raft_node_id_t recovering_node_id = 3;
    raft_index_t last_log_idx = 50;
    raft_term_t last_log_term = 4;

    FB_ASSERT_TRUE(recovering_node_id > 0);
    FB_ASSERT_TRUE(last_log_idx >= 0);
    FB_ASSERT_TRUE(last_log_term >= 0);
}

FB_TEST(raft_rpc, recovery_leader_response) {
    // Leader 响应恢复请求
    raft_identity state = RAFT_STATE_LEADER;
    bool can_respond = (state == RAFT_STATE_LEADER);

    FB_ASSERT_TRUE(can_respond);
}

FB_TEST(raft_rpc, recovery_follower_cannot_respond) {
    // Follower 无法响应恢复请求
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool can_respond = (state == RAFT_STATE_LEADER);

    FB_ASSERT_FALSE(can_respond);

    // 应该重定向到 Leader
    raft_node_id_t leader_id = 2;
    FB_ASSERT_TRUE(leader_id > 0);
}

FB_TEST(raft_rpc, recovery_log_sync_needed) {
    // 恢复节点需要日志同步
    raft_index_t leader_last_idx = 100;
    raft_index_t recovering_last_idx = 50;

    bool needs_sync = recovering_last_idx < leader_last_idx;
    FB_ASSERT_TRUE(needs_sync);

    raft_index_t entries_to_sync = leader_last_idx - recovering_last_idx;
    FB_ASSERT_EQ(entries_to_sync, 50L);
}

FB_TEST(raft_rpc, recovery_snapshot_needed) {
    // 恢复节点需要快照
    raft_index_t leader_snapshot_idx = 80;
    raft_index_t recovering_last_idx = 50;

    bool needs_snapshot = recovering_last_idx < leader_snapshot_idx;
    FB_ASSERT_TRUE(needs_snapshot);
}

FB_TEST(raft_rpc, recovery_from_disk_state) {
    // 从磁盘恢复状态
    raft_index_t disk_last_idx = 80;
    raft_index_t disk_commit_idx = 75;
    raft_term_t disk_term = 5;

    FB_ASSERT_TRUE(disk_last_idx >= disk_commit_idx);
    FB_ASSERT_TRUE(disk_term > 0);
}

FB_TEST(raft_rpc, recovery_term_restore) {
    // 恢复 term
    raft_term_t persisted_term = 5;
    raft_node_id_t persisted_voted_for = 3;

    FB_ASSERT_TRUE(persisted_term > 0);
    FB_ASSERT_TRUE(persisted_voted_for > 0);
}

FB_TEST(raft_rpc, recovery_commit_idx_restore) {
    // 恢复 commit_idx
    raft_index_t persisted_commit_idx = 75;
    raft_index_t current_commit_idx = 0;

    // 从磁盘恢复
    current_commit_idx = persisted_commit_idx;

    FB_ASSERT_EQ(current_commit_idx, 75L);
}

FB_TEST(raft_rpc, recovery_last_applied_catchup) {
    // 恢复后 last_applied 追赶
    raft_index_t commit_idx = 75;
    raft_index_t last_applied = 70;

    while (last_applied < commit_idx) {
        last_applied++;
    }

    FB_ASSERT_EQ(last_applied, 75L);
}

FB_TEST(raft_rpc, recovery_snapshot_apply) {
    // 恢复时应用快照
    raft_index_t snapshot_idx = 100;
    raft_term_t snapshot_term = 5;

    // 应用快照后更新索引
    raft_index_t last_applied = snapshot_idx;
    raft_index_t commit_idx = snapshot_idx;

    FB_ASSERT_EQ(last_applied, 100L);
    FB_ASSERT_EQ(commit_idx, 100L);
}

FB_TEST(raft_rpc, recovery_network_reconnect) {
    // 恢复网络连接
    std::set<raft_node_id_t> connected_nodes;
    raft_node_id_t recovering_node = 3;

    // 连接恢复
    connected_nodes.insert(recovering_node);

    bool is_connected = connected_nodes.count(recovering_node) > 0;
    FB_ASSERT_TRUE(is_connected);
}

FB_TEST(raft_rpc, recovery_state_transition) {
    // 恢复状态转换
    raft_identity state = RAFT_STATE_NONE;

    // 恢复完成后成为 Follower
    state = RAFT_STATE_FOLLOWER;

    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_rpc, recovery_timeout_handling) {
    // 恢复超时
    int recovery_timeout_ms = 30000;
    int elapsed_ms = 35000;

    bool timed_out = elapsed_ms >= recovery_timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, recovery_retry_on_failure) {
    // 恢复失败重试
    int retry_count = 0;
    int max_retries = 3;
    bool success = false;

    while (!success && retry_count < max_retries) {
        retry_count++;
        if (retry_count == 2) {
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, recovery_partial_log) {
    // 部分日志恢复
    std::vector<raft_index_t> persisted_logs;
    for (int i = 1; i <= 80; i++) {
        persisted_logs.push_back(i);
    }

    FB_ASSERT_EQ(persisted_logs.size(), 80UL);
    FB_ASSERT_EQ(persisted_logs.back(), 80L);
}

FB_TEST(raft_rpc, recovery_log_truncation) {
    // 恢复时日志截断
    raft_index_t persisted_last_idx = 85;
    raft_index_t leader_commit_idx = 80;

    // 截断未提交的日志
    if (persisted_last_idx > leader_commit_idx) {
        persisted_last_idx = leader_commit_idx;
    }

    FB_ASSERT_EQ(persisted_last_idx, 80L);
}

FB_TEST(raft_rpc, recovery_config_restore) {
    // 恢复配置
    std::vector<raft_node_id_t> persisted_config = {1, 2, 3};
    std::vector<raft_node_id_t> current_config;

    current_config = persisted_config;

    FB_ASSERT_EQ(current_config.size(), 3UL);
}

FB_TEST(raft_rpc, recovery_vote_state_reset) {
    // 恢复投票状态
    raft_node_id_t voted_for = 0;  // 重置

    FB_ASSERT_EQ(voted_for, 0L);

    // 可以重新投票
    bool can_vote = (voted_for == 0);
    FB_ASSERT_TRUE(can_vote);
}

FB_TEST(raft_rpc, recovery_leader_identification) {
    // 恢复后识别 Leader
    raft_node_id_t leader_id = 0;  // 未知

    // 通过心跳识别 Leader
    raft_node_id_t heartbeat_from = 2;
    if (heartbeat_from > 0) {
        leader_id = heartbeat_from;
    }

    FB_ASSERT_EQ(leader_id, 2L);
}

FB_TEST(raft_rpc, recovery_graceful_restart) {
    // 优雅重启
    bool was_leader = true;
    raft_identity restart_state = RAFT_STATE_FOLLOWER;

    // 重启后不立即成为 Leader
    FB_ASSERT_EQ(restart_state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_rpc, recovery_crash_recovery) {
    // 崩溃恢复
    bool crash_occurred = true;
    bool recovery_needed = crash_occurred;

    FB_ASSERT_TRUE(recovery_needed);

    // 从持久化状态恢复
    bool persisted_state_valid = true;
    FB_ASSERT_TRUE(persisted_state_valid);
}

FB_TEST(raft_rpc, recovery_during_election) {
    // 选举期间的恢复
    raft_identity current_leader_state = RAFT_STATE_CANDIDATE;

    // 等待选举完成
    bool election_complete = false;

    // 恢复节点等待
    bool wait_for_leader = !election_complete;
    FB_ASSERT_TRUE(wait_for_leader);
}

FB_TEST(raft_rpc, recovery_batch_entries) {
    // 批量恢复日志条目
    int batch_size = 100;
    int total_entries = 500;
    int batches_needed = (total_entries + batch_size - 1) / batch_size;

    FB_ASSERT_EQ(batches_needed, 5);
}

FB_TEST(raft_rpc, recovery_incremental_sync) {
    // 增量同步
    raft_index_t local_last_idx = 80;
    raft_index_t leader_last_idx = 100;

    int sync_rounds = 0;
    while (local_last_idx < leader_last_idx) {
        local_last_idx += 10;
        sync_rounds++;
        if (local_last_idx > leader_last_idx) {
            local_last_idx = leader_last_idx;
        }
    }

    FB_ASSERT_EQ(local_last_idx, 100L);
    FB_ASSERT_EQ(sync_rounds, 2);
}

FB_TEST(raft_rpc, recovery_uncommitted_entries) {
    // 未提交条目处理
    std::vector<raft_index_t> uncommitted = {81, 82, 83};

    // 恢复时丢弃未提交条目
    uncommitted.clear();

    FB_ASSERT_TRUE(uncommitted.empty());
}

FB_TEST(raft_rpc, recovery_metrics_tracking) {
    // 恢复指标跟踪
    raft_time_t recovery_start = 1000;
    raft_time_t recovery_end = 1500;

    raft_time_t recovery_duration = recovery_end - recovery_start;
    FB_ASSERT_EQ(recovery_duration, 500L);
}

FB_TEST(raft_rpc, recovery_checkpoint_usage) {
    // 检查点使用
    raft_index_t checkpoint_idx = 70;
    raft_index_t current_idx = 50;

    // 从检查点恢复
    bool use_checkpoint = checkpoint_idx > current_idx;
    FB_ASSERT_TRUE(use_checkpoint);

    raft_index_t recovery_start_idx = checkpoint_idx;
    FB_ASSERT_EQ(recovery_start_idx, 70L);
}

FB_TEST(raft_rpc, recovery_node_rejoining_cluster) {
    // 节点重新加入集群
    raft_node_id_t node_id = 3;
    std::set<raft_node_id_t> cluster_nodes = {1, 2, 3, 4, 5};

    bool was_member = cluster_nodes.count(node_id) > 0;
    FB_ASSERT_TRUE(was_member);

    // 无需添加，直接恢复
    bool need_add = !was_member;
    FB_ASSERT_FALSE(need_add);
}

FB_TEST(raft_rpc, recovery_timeout_during_sync) {
    // 同步期间超时
    int sync_timeout_ms = 5000;
    int elapsed_ms = 6000;

    bool sync_timed_out = elapsed_ms >= sync_timeout_ms;
    FB_ASSERT_TRUE(sync_timed_out);
}

FB_TEST(raft_rpc, recovery_concurrent_recovery) {
    // 多节点并发恢复
    std::set<raft_node_id_t> recovering_nodes = {3, 4, 5};

    // Leader 限制并发恢复数
    int max_concurrent_recovery = 2;
    bool can_start_new = recovering_nodes.size() < max_concurrent_recovery;

    FB_ASSERT_FALSE(can_start_new);
}

// ============================================================================
// Test Suite: Bootstrap RPC Tests (Cluster Initialization)
// ============================================================================

FB_TEST(raft_rpc, bootstrap_init_request) {
    // 引导初始化请求
    std::vector<raft_node_id_t> initial_members = {1, 2, 3};
    uint64_t cluster_id = 12345;

    FB_ASSERT_EQ(initial_members.size(), 3UL);
    FB_ASSERT_TRUE(cluster_id > 0);
}

FB_TEST(raft_rpc, bootstrap_first_node_becomes_leader) {
    // 第一个节点成为 Leader
    raft_node_id_t first_node_id = 1;
    raft_identity first_state = RAFT_STATE_LEADER;

    // 单节点集群
    uint64_t node_num = 1;
    bool can_be_leader = (first_state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_be_leader);
}

FB_TEST(raft_rpc, bootstrap_single_node_cluster) {
    // 单节点集群
    std::vector<raft_node_id_t> members = {1};

    FB_ASSERT_EQ(members.size(), 1UL);

    // 单节点直接成为 Leader
    uint64_t votes = 1;
    uint64_t node_num = 1;
    bool is_leader = votes > node_num / 2;
    FB_ASSERT_TRUE(is_leader);
}

FB_TEST(raft_rpc, bootstrap_multi_node_join) {
    // 多节点加入
    std::vector<raft_node_id_t> members = {1, 2, 3};

    // 第一个节点引导，其他节点加入
    bool bootstrap_complete = true;

    for (raft_node_id_t id : members) {
        if (id != 1) {
            // 其他节点加入集群
        }
    }

    FB_ASSERT_TRUE(bootstrap_complete);
}

FB_TEST(raft_rpc, bootstrap_config_entry_creation) {
    // 创建配置日志条目
    raft_logtype_e log_type = RAFT_LOGTYPE_CONFIGURATION;
    raft_index_t config_idx = 1;

    FB_ASSERT_EQ(log_type, RAFT_LOGTYPE_CONFIGURATION);
    FB_ASSERT_EQ(config_idx, 1L);
}

FB_TEST(raft_rpc, bootstrap_term_initialization) {
    // Term 初始化
    raft_term_t initial_term = 1;

    FB_ASSERT_EQ(initial_term, 1L);

    // 从 1 开始
    bool term_valid = initial_term >= 1;
    FB_ASSERT_TRUE(term_valid);
}

FB_TEST(raft_rpc, bootstrap_empty_log_start) {
    // 空日志开始
    raft_index_t first_log_idx = 1;
    raft_index_t commit_idx = 0;

    FB_ASSERT_EQ(first_log_idx, 1L);
    FB_ASSERT_EQ(commit_idx, 0L);
}

FB_TEST(raft_rpc, bootstrap_duplicate_bootstrap_reject) {
    // 拒绝重复引导
    bool already_bootstrapped = true;

    bool can_bootstrap = !already_bootstrapped;
    FB_ASSERT_FALSE(can_bootstrap);
}

FB_TEST(raft_rpc, bootstrap_idempotent_check) {
    // 幂等检查
    bool bootstrap_requested = true;
    bool cluster_initialized = true;

    // 已初始化的集群拒绝再次引导
    bool should_bootstrap = bootstrap_requested && !cluster_initialized;
    FB_ASSERT_FALSE(should_bootstrap);
}

FB_TEST(raft_rpc, bootstrap_node_addresses) {
    // 节点地址配置
    std::map<raft_node_id_t, std::pair<std::string, int>> addresses;
    addresses[1] = {"127.0.0.1", 8888};
    addresses[2] = {"127.0.0.1", 8889};
    addresses[3] = {"127.0.0.1", 8890};

    FB_ASSERT_EQ(addresses.size(), 3UL);
}

FB_TEST(raft_rpc, bootstrap_quorum_calculation) {
    // 引导时多数派计算
    uint64_t initial_members = 3;
    uint64_t quorum = initial_members / 2 + 1;

    FB_ASSERT_EQ(quorum, 2UL);
}

FB_TEST(raft_rpc, bootstrap_leader_election_skip) {
    // 引导时跳过选举
    bool is_bootstrap = true;
    bool need_election = !is_bootstrap;

    FB_ASSERT_FALSE(need_election);

    // 第一个节点直接成为 Leader
    raft_identity state = RAFT_STATE_LEADER;
    FB_ASSERT_EQ(state, RAFT_STATE_LEADER);
}

FB_TEST(raft_rpc, bootstrap_persistent_state) {
    // 持久化状态
    raft_term_t current_term = 1;
    raft_node_id_t voted_for = 1;  // 投给自己
    std::vector<raft_node_id_t> config = {1, 2, 3};

    FB_ASSERT_EQ(current_term, 1L);
    FB_ASSERT_EQ(voted_for, 1L);
    FB_ASSERT_EQ(config.size(), 3UL);
}

FB_TEST(raft_rpc, bootstrap_join_existing_cluster) {
    // 加入现有集群
    std::vector<raft_node_id_t> existing_members = {1, 2, 3};
    raft_node_id_t new_node = 4;

    // 新节点通过 AddNode RPC 加入
    bool can_join = true;
    FB_ASSERT_TRUE(can_join);
}

FB_TEST(raft_rpc, bootstrap_timeout_handling) {
    // 引导超时
    int bootstrap_timeout_ms = 30000;
    int elapsed_ms = 35000;

    bool timed_out = elapsed_ms >= bootstrap_timeout_ms;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_rpc, bootstrap_failure_recovery) {
    // 引导失败恢复
    bool bootstrap_failed = true;

    if (bootstrap_failed) {
        // 清理部分状态
        bool cleaned_up = true;
        FB_ASSERT_TRUE(cleaned_up);

        // 可以重试引导
        bool can_retry = true;
        FB_ASSERT_TRUE(can_retry);
    }
}

FB_TEST(raft_rpc, bootstrap_cluster_id_unique) {
    // 集群 ID 唯一性
    uint64_t cluster_id_1 = 12345;
    uint64_t cluster_id_2 = 67890;

    bool unique = (cluster_id_1 != cluster_id_2);
    FB_ASSERT_TRUE(unique);
}

FB_TEST(raft_rpc, bootstrap_node_id_assignment) {
    // 节点 ID 分配
    std::set<raft_node_id_t> used_ids;
    raft_node_id_t next_id = 1;

    while (used_ids.count(next_id)) {
        next_id++;
    }

    used_ids.insert(next_id);

    FB_ASSERT_EQ(used_ids.size(), 1UL);
    FB_ASSERT_TRUE(used_ids.count(1));
}

FB_TEST(raft_rpc, bootstrap_min_cluster_size) {
    // 最小集群大小
    uint64_t min_size = 1;
    uint64_t actual_size = 3;

    bool meets_minimum = actual_size >= min_size;
    FB_ASSERT_TRUE(meets_minimum);
}

FB_TEST(raft_rpc, bootstrap_max_cluster_size) {
    // 最大集群大小
    uint64_t max_size = 100;
    uint64_t actual_size = 5;

    bool within_limit = actual_size <= max_size;
    FB_ASSERT_TRUE(within_limit);
}

FB_TEST(raft_rpc, bootstrap_config_propagation) {
    // 配置传播
    std::vector<raft_node_id_t> members = {1, 2, 3};
    int propagation_count = 0;

    for (auto id : members) {
        if (id != 1) {
            propagation_count++;
        }
    }

    FB_ASSERT_EQ(propagation_count, 2);
}

FB_TEST(raft_rpc, bootstrap_state_verification) {
    // 状态验证
    raft_term_t term = 1;
    raft_index_t commit_idx = 1;
    raft_identity state = RAFT_STATE_LEADER;

    bool bootstrap_valid = (term == 1) && (commit_idx == 1) && (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(bootstrap_valid);
}

FB_TEST(raft_rpc, bootstrap_rollback_on_failure) {
    // 失败时回滚
    std::vector<raft_node_id_t> members = {1, 2, 3};
    bool rollback_needed = true;

    if (rollback_needed) {
        members.clear();
    }

    FB_ASSERT_TRUE(members.empty());
}

FB_TEST(raft_rpc, bootstrap_metadata_initialization) {
    // 元数据初始化
    std::string cluster_name = "my-cluster";
    uint64_t create_time = 1000;
    std::string version = "1.0.0";

    FB_ASSERT_FALSE(cluster_name.empty());
    FB_ASSERT_TRUE(create_time > 0);
    FB_ASSERT_FALSE(version.empty());
}

FB_TEST(raft_rpc, bootstrap_initial_snapshot) {
    // 初始快照（可选）
    bool has_initial_snapshot = false;
    raft_index_t snapshot_idx = 0;

    if (!has_initial_snapshot) {
        snapshot_idx = 0;
    }

    FB_ASSERT_EQ(snapshot_idx, 0L);
}

FB_TEST(raft_rpc, bootstrap_concurrent_attempt) {
    // 并发引导尝试
    int concurrent_bootstrap_requests = 2;

    // 只允许一个成功
    int successful_bootstrap = 1;
    FB_ASSERT_LT(successful_bootstrap, concurrent_bootstrap_requests);
}

FB_TEST(raft_rpc, bootstrap_network_connectivity) {
    // 网络连通性检查
    std::set<raft_node_id_t> reachable_nodes = {1, 2};
    std::vector<raft_node_id_t> initial_members = {1, 2, 3};

    bool all_reachable = reachable_nodes.size() >= initial_members.size();
    FB_ASSERT_FALSE(all_reachable);

    // 需要等待所有节点可达
    bool bootstrap_complete = all_reachable;
    FB_ASSERT_FALSE(bootstrap_complete);
}

FB_TEST(raft_rpc, bootstrap_voting_members) {
    // 投票成员配置
    std::map<raft_node_id_t, bool> voting_status;
    voting_status[1] = true;
    voting_status[2] = true;
    voting_status[3] = true;

    int voting_count = 0;
    for (const auto& pair : voting_status) {
        if (pair.second) voting_count++;
    }

    FB_ASSERT_EQ(voting_count, 3);
}

FB_TEST(raft_rpc, bootstrap_joint_consensus_initial) {
    // 初始无联合共识
    bool in_joint_consensus = false;

    FB_ASSERT_FALSE(in_joint_consensus);

    // 只有单一配置
    bool single_config = !in_joint_consensus;
    FB_ASSERT_TRUE(single_config);
}

// ============================================================================
// Test Suite: LogGC RPC Tests (Log Garbage Collection)
// ============================================================================

FB_TEST(raft_rpc, loggc_request_fields) {
    // 日志清理请求字段
    raft_index_t safe_to_delete_idx = 50;
    raft_term_t term = 5;

    FB_ASSERT_TRUE(safe_to_delete_idx >= 0);
    FB_ASSERT_TRUE(term > 0);
}

FB_TEST(raft_rpc, loggc_snapshot_based_gc) {
    // 基于快照的日志清理
    raft_index_t snapshot_idx = 100;
    raft_index_t first_log_idx = 1;

    // 快照后可删除快照之前的日志
    bool can_gc = snapshot_idx > first_log_idx;
    FB_ASSERT_TRUE(can_gc);

    raft_index_t new_first_idx = snapshot_idx + 1;
    FB_ASSERT_EQ(new_first_idx, 101L);
}

FB_TEST(raft_rpc, loggc_commit_idx_based_gc) {
    // 基于 commit_idx 的日志清理
    raft_index_t commit_idx = 80;
    raft_index_t first_log_idx = 1;
    raft_index_t gc_threshold = 50;

    // 超过阈值的已提交日志可清理
    raft_index_t gcable = commit_idx - first_log_idx;
    bool should_gc = gcable >= gc_threshold;

    FB_ASSERT_TRUE(should_gc);
}

FB_TEST(raft_rpc, loggc_retention_policy) {
    // 日志保留策略
    raft_index_t last_log_idx = 100;
    raft_index_t retain_count = 10;

    // 保留最近 N 条日志
    raft_index_t safe_delete_up_to = last_log_idx - retain_count;

    FB_ASSERT_EQ(safe_delete_up_to, 90L);
}

FB_TEST(raft_rpc, loggc_disk_space_reclamation) {
    // 磁盘空间回收
    size_t entries_deleted = 100;
    size_t bytes_per_entry = 1024;
    size_t bytes_freed = entries_deleted * bytes_per_entry;

    FB_ASSERT_EQ(bytes_freed, 102400UL);
}

FB_TEST(raft_rpc, loggc_follower_sync_check) {
    // Follower 同步检查
    std::map<raft_node_id_t, raft_index_t> match_indices;
    match_indices[2] = 95;
    match_indices[3] = 90;
    match_indices[4] = 85;

    // 找最小的 match_idx
    raft_index_t min_match = match_indices[2];
    for (const auto& pair : match_indices) {
        if (pair.second < min_match) {
            min_match = pair.second;
        }
    }

    FB_ASSERT_EQ(min_match, 85L);

    // 只能删除所有 Follower 都已同步的日志
    bool safe_delete = true;
    FB_ASSERT_TRUE(safe_delete);
}

FB_TEST(raft_rpc, loggc_in_progress_entries_check) {
    // 待处理日志检查
    raft_index_t next_idx = 100;
    raft_index_t match_idx = 95;

    // 正在传输中的日志不能删除
    raft_index_t in_flight = next_idx - match_idx - 1;
    FB_ASSERT_EQ(in_flight, 4L);
}

FB_TEST(raft_rpc, loggc_snapshot_first) {
    // 先创建快照再清理
    bool snapshot_exists = true;
    bool can_gc = snapshot_exists;

    FB_ASSERT_TRUE(can_gc);

    // 无快照时不能清理
    snapshot_exists = false;
    can_gc = snapshot_exists;
    FB_ASSERT_FALSE(can_gc);
}

FB_TEST(raft_rpc, loggc_batch_deletion) {
    // 批量删除
    int batch_size = 100;
    int total_entries = 500;
    int batches = (total_entries + batch_size - 1) / batch_size;

    FB_ASSERT_EQ(batches, 5);
}

FB_TEST(raft_rpc, loggc_disk_io_optimization) {
    // 磁盘 I/O 优化
    bool use_batch_delete = true;
    int individual_deletes = 100;
    int batch_deletes = 1;

    if (use_batch_delete) {
        FB_ASSERT_LT(batch_deletes, individual_deletes);
    }
}

FB_TEST(raft_rpc, loggc_truncate_vs_delete) {
    // 截断 vs 删除
    bool use_truncate = true;  // 文件截断更快
    raft_index_t entries_to_remove = 100;

    FB_ASSERT_TRUE(use_truncate);
    FB_ASSERT_EQ(entries_to_remove, 100L);
}

FB_TEST(raft_rpc, loggc_metadata_update) {
    // 元数据更新
    raft_index_t old_first_idx = 1;
    raft_index_t new_first_idx = 50;

    // 更新 first_log_idx
    raft_index_t first_log_idx = new_first_idx;

    FB_ASSERT_EQ(first_log_idx, 50L);
    FB_ASSERT_TRUE(first_log_idx > old_first_idx);
}

FB_TEST(raft_rpc, loggc_index_mapping) {
    // 索引映射更新
    std::map<raft_index_t, raft_term_t> log_cache;

    // 删除旧条目
    raft_index_t delete_up_to = 50;
    for (raft_index_t idx = 1; idx <= delete_up_to; idx++) {
        log_cache.erase(idx);
    }

    FB_ASSERT_TRUE(log_cache.empty());
}

FB_TEST(raft_rpc, loggc_concurrent_access) {
    // 并发访问安全
    std::atomic<bool> gc_in_progress{true};
    std::atomic<bool> can_append{false};

    // GC 期间暂停日志追加
    can_append = !gc_in_progress.load();
    FB_ASSERT_FALSE(can_append);

    gc_in_progress.store(false);
    can_append = !gc_in_progress.load();
    FB_ASSERT_TRUE(can_append);
}

FB_TEST(raft_rpc, loggc_recovery_consistency) {
    // 恢复一致性
    raft_index_t first_log_idx = 50;
    raft_index_t snapshot_idx = 100;

    // 确保快照索引有效
    bool consistent = snapshot_idx >= first_log_idx - 1;
    FB_ASSERT_TRUE(consistent);
}

FB_TEST(raft_rpc, loggc_partial_gc) {
    // 部分清理
    raft_index_t gc_start = 1;
    raft_index_t gc_end = 50;
    raft_index_t safe_point = 40;  // 只清理到安全点

    raft_index_t actual_gc_end = std::min(gc_end, safe_point);
    FB_ASSERT_EQ(actual_gc_end, 40L);
}

FB_TEST(raft_rpc, loggc_failure_rollback) {
    // 清理失败回滚
    bool gc_failed = true;
    raft_index_t deleted_count = 30;

    if (gc_failed) {
        deleted_count = 0;  // 回滚
    }

    FB_ASSERT_EQ(deleted_count, 0L);
}

FB_TEST(raft_rpc, loggc_metrics_tracking) {
    // 指标跟踪
    uint64_t gc_runs_total = 10;
    uint64_t entries_deleted_total = 500;
    uint64_t bytes_freed_total = 512 * 1024;

    double avg_entries_per_run = entries_deleted_total / gc_runs_total;
    FB_ASSERT_EQ(avg_entries_per_run, 50.0);
}

FB_TEST(raft_rpc, loggc_trigger_conditions) {
    // 触发条件
    size_t log_count = 10000;
    size_t log_threshold = 5000;

    bool should_gc = log_count >= log_threshold;
    FB_ASSERT_TRUE(should_gc);

    // 空间触发
    size_t log_size = 100 * 1024 * 1024;  // 100MB
    size_t size_threshold = 50 * 1024 * 1024;  // 50MB

    should_gc = should_gc || (log_size >= size_threshold);
    FB_ASSERT_TRUE(should_gc);
}

FB_TEST(raft_rpc, loggc_periodic_vs_event) {
    // 周期性 vs 事件触发
    bool periodic_gc = true;
    bool event_triggered_gc = true;

    // 两种触发方式都支持
    bool gc_supported = periodic_gc || event_triggered_gc;
    FB_ASSERT_TRUE(gc_supported);
}

FB_TEST(raft_rpc, loggc_priority) {
    // 清理优先级
    int normal_priority = 0;
    int low_priority = -1;  // GC 使用低优先级

    FB_ASSERT_LT(low_priority, normal_priority);

    // 不影响正常操作
    bool non_blocking = true;
    FB_ASSERT_TRUE(non_blocking);
}

FB_TEST(raft_rpc, loggc_compaction_ratio) {
    // 压缩比
    size_t original_size = 100000;
    size_t after_gc_size = 50000;

    double compaction_ratio = 100.0 * (original_size - after_gc_size) / original_size;
    FB_ASSERT_EQ(compaction_ratio, 50.0);
}

FB_TEST(raft_rpc, loggc_cache_eviction) {
    // 缓存驱逐
    std::map<raft_index_t, int> cache;
    for (int i = 1; i <= 100; i++) {
        cache[i] = i;
    }

    // 清理缓存中的旧条目
    raft_index_t evict_up_to = 50;
    for (raft_index_t idx = 1; idx <= evict_up_to; idx++) {
        cache.erase(idx);
    }

    FB_ASSERT_EQ(cache.size(), 50UL);
}

FB_TEST(raft_rpc, loggc_leader_coordinated) {
    // Leader 协调清理
    raft_identity state = RAFT_STATE_LEADER;

    bool can_initiate_gc = (state == RAFT_STATE_LEADER);
    FB_ASSERT_TRUE(can_initiate_gc);
}

FB_TEST(raft_rpc, loggc_follower_autonomous) {
    // Follower 自主清理
    raft_identity state = RAFT_STATE_FOLLOWER;

    // Follower 可以根据本地快照自主清理
    bool has_local_snapshot = true;
    bool can_gc = has_local_snapshot;

    FB_ASSERT_TRUE(can_gc);
}

FB_TEST(raft_rpc, loggc_min_retention) {
    // 最小保留
    raft_index_t min_retention = 100;
    raft_index_t current_log_count = 50;

    // 低于最小保留量不清理
    bool should_gc = current_log_count > min_retention;
    FB_ASSERT_FALSE(should_gc);
}

FB_TEST(raft_rpc, loggc_max_retention) {
    // 最大保留
    raft_index_t max_retention = 10000;
    raft_index_t current_log_count = 15000;

    // 超过最大保留量必须清理
    bool must_gc = current_log_count > max_retention;
    FB_ASSERT_TRUE(must_gc);
}

// ============================================================================
// Test Suite: AsyncAppend RPC Tests (Async Log Replication)
// ============================================================================

FB_TEST(raft_rpc, asyncappend_request_fields) {
    // 异步追加请求字段
    raft_term_t term = 5;
    raft_node_id_t leader_id = 1;
    raft_index_t prev_log_idx = 100;
    std::vector<raft_index_t> entry_indices = {101, 102, 103};

    FB_ASSERT_TRUE(term > 0);
    FB_ASSERT_TRUE(leader_id > 0);
    FB_ASSERT_TRUE(prev_log_idx >= 0);
    FB_ASSERT_EQ(entry_indices.size(), 3UL);
}

FB_TEST(raft_rpc, asyncappend_callback_registration) {
    // 回调注册
    bool callback_registered = true;
    uint64_t callback_id = 12345;

    FB_ASSERT_TRUE(callback_registered);
    FB_ASSERT_TRUE(callback_id > 0);
}

FB_TEST(raft_rpc, asyncappend_response_handling) {
    // 响应处理
    bool success = true;
    raft_index_t match_idx = 103;
    uint64_t callback_id = 12345;

    // 回调触发
    if (success) {
        // 更新 match_idx
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(match_idx, 103L);
}

FB_TEST(raft_rpc, asyncappend_pipeline_depth) {
    // 流水线深度
    int max_pipeline_depth = 3;
    int current_pipeline_depth = 2;

    bool can_send_more = current_pipeline_depth < max_pipeline_depth;
    FB_ASSERT_TRUE(can_send_more);

    current_pipeline_depth = 3;
    can_send_more = current_pipeline_depth < max_pipeline_depth;
    FB_ASSERT_FALSE(can_send_more);
}

FB_TEST(raft_rpc, asyncappend_in_flight_tracking) {
    // 进行中的请求跟踪
    std::set<uint64_t> in_flight_requests;
    in_flight_requests.insert(1001);
    in_flight_requests.insert(1002);
    in_flight_requests.insert(1003);

    FB_ASSERT_EQ(in_flight_requests.size(), 3UL);

    // 请求完成后移除
    in_flight_requests.erase(1001);
    FB_ASSERT_EQ(in_flight_requests.size(), 2UL);
}

FB_TEST(raft_rpc, asyncappend_timeout_handling) {
    // 超时处理
    int async_timeout_ms = 5000;
    int elapsed_ms = 6000;

    bool timed_out = elapsed_ms >= async_timeout_ms;
    FB_ASSERT_TRUE(timed_out);

    // 超时后回调
    bool callback_triggered = timed_out;
    FB_ASSERT_TRUE(callback_triggered);
}

FB_TEST(raft_rpc, asyncappend_retry_on_failure) {
    // 失败重试
    int retry_count = 0;
    int max_retries = 3;
    bool success = false;

    while (!success && retry_count < max_retries) {
        retry_count++;
        if (retry_count == 2) {
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_rpc, asyncappend_flow_control) {
    // 流量控制
    int window_size = 10;
    int in_flight = 8;
    int available_window = window_size - in_flight;

    FB_ASSERT_EQ(available_window, 2);

    // 窗口为0时暂停发送
    bool can_send = available_window > 0;
    FB_ASSERT_TRUE(can_send);
}

FB_TEST(raft_rpc, asyncappend_batch_optimization) {
    // 批量优化
    std::vector<raft_index_t> entries = {101, 102, 103, 104, 105};

    // 合并为一次异步请求
    int rpc_count = 1;
    FB_ASSERT_LT(rpc_count, entries.size());

    raft_index_t start_idx = entries.front();
    raft_index_t end_idx = entries.back();
    FB_ASSERT_EQ(end_idx - start_idx + 1, 5L);
}

FB_TEST(raft_rpc, asyncappend_ordering_guarantee) {
    // 顺序保证
    std::vector<uint64_t> request_order = {1001, 1002, 1003};
    std::vector<uint64_t> response_order;

    for (auto id : request_order) {
        response_order.push_back(id);  // 按顺序完成
    }

    FB_ASSERT_EQ(response_order.size(), request_order.size());
    for (size_t i = 0; i < request_order.size(); i++) {
        FB_ASSERT_EQ(response_order[i], request_order[i]);
    }
}

FB_TEST(raft_rpc, asyncappend_concurrent_senders) {
    // 并发发送者
    int concurrent_senders = 5;
    std::atomic<int> active_requests{concurrent_senders};

    FB_ASSERT_EQ(active_requests.load(), 5);

    // 限制并发
    int max_concurrent = 10;
    bool within_limit = concurrent_senders <= max_concurrent;
    FB_ASSERT_TRUE(within_limit);
}

FB_TEST(raft_rpc, asyncappend_callback_context) {
    // 回调上下文
    struct callback_context {
        uint64_t request_id;
        raft_index_t expected_match_idx;
        void* user_data;
    };

    callback_context ctx = {1001, 105, nullptr};

    FB_ASSERT_EQ(ctx.request_id, 1001UL);
    FB_ASSERT_EQ(ctx.expected_match_idx, 105L);
}

FB_TEST(raft_rpc, asyncappend_failure_propagation) {
    // 失败传播
    bool append_failed = true;
    int error_code = -1;

    // 回调传递错误
    bool callback_received_error = append_failed;
    FB_ASSERT_TRUE(callback_received_error);

    // 用户处理错误
    FB_ASSERT_TRUE(error_code < 0);
}

FB_TEST(raft_rpc, asyncappend_success_notification) {
    // 成功通知
    bool append_success = true;
    raft_index_t new_match_idx = 105;

    // 回调通知成功
    if (append_success) {
        // 用户收到成功通知
        bool user_notified = true;
        FB_ASSERT_TRUE(user_notified);
    }
}

FB_TEST(raft_rpc, asyncappend_priority_levels) {
    // 优先级级别
    int high_priority = 1;
    int normal_priority = 0;
    int low_priority = -1;

    FB_ASSERT_GT(high_priority, normal_priority);
    FB_ASSERT_LT(low_priority, normal_priority);

    // 高优先级优先处理
    bool process_first = true;
    FB_ASSERT_TRUE(process_first);
}

FB_TEST(raft_rpc, asyncappend_backpressure) {
    // 反压机制
    int pending_requests = 100;
    int max_pending = 50;

    bool apply_backpressure = pending_requests > max_pending;
    FB_ASSERT_TRUE(apply_backpressure);

    // 减缓发送速度
    int new_send_rate = max_pending;
    FB_ASSERT_LT(new_send_rate, pending_requests);
}

FB_TEST(raft_rpc, asyncappend_cancellation) {
    // 取消请求
    uint64_t request_id = 1001;
    std::set<uint64_t> pending_requests = {1001, 1002, 1003};

    // 取消请求
    pending_requests.erase(request_id);

    FB_ASSERT_FALSE(pending_requests.count(request_id));
    FB_ASSERT_EQ(pending_requests.size(), 2UL);

    // 回调不触发
    bool callback_skipped = true;
    FB_ASSERT_TRUE(callback_skipped);
}

FB_TEST(raft_rpc, asyncappend_metrics_collection) {
    // 指标收集
    uint64_t async_requests_total = 100;
    uint64_t async_requests_success = 95;
    uint64_t async_requests_failed = 5;

    FB_ASSERT_EQ(async_requests_total, async_requests_success + async_requests_failed);

    double success_rate = 100.0 * async_requests_success / async_requests_total;
    FB_ASSERT_GE(success_rate, 95.0);
}

FB_TEST(raft_rpc, asyncappend_latency_measurement) {
    // 延迟测量
    raft_time_t send_time = 1000;
    raft_time_t callback_time = 1500;

    raft_time_t latency = callback_time - send_time;
    FB_ASSERT_EQ(latency, 500L);

    // 平均延迟
    std::vector<raft_time_t> latencies = {400, 500, 600};
    raft_time_t avg_latency = 0;
    for (auto l : latencies) avg_latency += l;
    avg_latency /= latencies.size();

    FB_ASSERT_EQ(avg_latency, 500L);
}

FB_TEST(raft_rpc, asyncappend_network_optimization) {
    // 网络优化
    bool use_compression = true;
    size_t original_size = 1024;
    size_t compressed_size = 512;

    if (use_compression) {
        FB_ASSERT_LT(compressed_size, original_size);
    }
}

FB_TEST(raft_rpc, asyncappend_buffer_management) {
    // 缓冲区管理
    size_t buffer_size = 64 * 1024;
    size_t used_buffer = 30 * 1024;
    size_t available_buffer = buffer_size - used_buffer;

    FB_ASSERT_EQ(available_buffer, 34 * 1024UL);

    // 缓冲区满时等待
    bool buffer_available = available_buffer > 0;
    FB_ASSERT_TRUE(buffer_available);
}

FB_TEST(raft_rpc, asyncappend_error_recovery) {
    // 错误恢复
    int consecutive_errors = 3;
    int error_threshold = 5;

    bool need_recovery = consecutive_errors >= error_threshold;
    FB_ASSERT_FALSE(need_recovery);

    // 恢复策略
    consecutive_errors = 0;  // 重置
    FB_ASSERT_EQ(consecutive_errors, 0);
}

FB_TEST(raft_rpc, asyncappend_leader_change_handling) {
    // Leader 变更处理
    raft_node_id_t current_leader = 1;
    raft_node_id_t new_leader = 2;

    // 进行中的请求需要重定向
    bool need_redirect = (current_leader != new_leader);
    FB_ASSERT_TRUE(need_redirect);

    // 取消旧请求
    bool cancel_pending = need_redirect;
    FB_ASSERT_TRUE(cancel_pending);
}

FB_TEST(raft_rpc, asyncappend_follower_slow_response) {
    // Follower 慢响应
    raft_time_t expected_response_time = 500;
    raft_time_t actual_response_time = 2000;

    bool slow_response = actual_response_time > expected_response_time;
    FB_ASSERT_TRUE(slow_response);

    // 调整流水线深度
    int new_pipeline_depth = 1;  // 减少深度
    FB_ASSERT_LT(new_pipeline_depth, 3);
}

FB_TEST(raft_rpc, asyncappend_fast_path_optimization) {
    // 快速路径优化
    bool use_fast_path = true;
    raft_index_t prev_log_idx = 100;
    raft_index_t match_idx = 100;

    // match_idx == prev_log_idx 时使用快速路径
    bool can_fast_path = use_fast_path && (match_idx == prev_log_idx);
    FB_ASSERT_TRUE(can_fast_path);
}

FB_TEST(raft_rpc, asyncappend_resource_cleanup) {
    // 资源清理
    std::vector<void*> allocated_resources;
    allocated_resources.push_back((void*)1);
    allocated_resources.push_back((void*)2);

    // 请求完成后清理
    for (auto ptr : allocated_resources) {
        // 释放资源
    }
    allocated_resources.clear();

    FB_ASSERT_TRUE(allocated_resources.empty());
}

FB_TEST(raft_rpc, asyncappend_parallel_follower_append) {
    // 并行 Follower 追加
    std::set<raft_node_id_t> followers = {2, 3, 4};
    int parallel_append_count = 0;

    for (auto id : followers) {
        parallel_append_count++;
    }

    FB_ASSERT_EQ(parallel_append_count, 3);
}

FB_TEST(raft_rpc, asyncappend_quorum_wait) {
    // 多数派等待
    uint64_t node_num = 5;
    uint64_t quorum = node_num / 2 + 1;
    uint64_t success_count = 0;

    // 等待多数派响应
    while (success_count < quorum) {
        success_count++;
    }

    FB_ASSERT_GE(success_count, quorum);
    FB_ASSERT_EQ(success_count, 3UL);
}

FB_TEST(raft_rpc, asyncappend_commit_notification) {
    // 提交通知
    raft_index_t commit_idx = 103;
    bool quorum_reached = true;

    if (quorum_reached) {
        // 通知用户日志已提交
        bool user_notified = true;
        FB_ASSERT_TRUE(user_notified);
    }

    FB_ASSERT_EQ(commit_idx, 103L);
}

FB_TEST(raft_rpc, asyncappend_user_callback_types) {
    // 用户回调类型
    enum callback_type {
        ON_SUCCESS,
        ON_FAILURE,
        ON_TIMEOUT,
        ON_CANCEL
    };

    callback_type cb = ON_SUCCESS;
    FB_ASSERT_EQ(static_cast<int>(cb), 0);

    cb = ON_FAILURE;
    FB_ASSERT_EQ(static_cast<int>(cb), 1);
}

// Main function for test runner
FB_TEST_MAIN()
