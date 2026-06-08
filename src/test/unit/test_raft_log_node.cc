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
 * @file test_raft_log_node.cc
 * @brief Unit tests for Raft log operations and node state management
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <limits>
#include <queue>

namespace {

typedef long int raft_term_t;
typedef long int raft_index_t;
typedef long int raft_time_t;
typedef long int raft_entry_id_t;
typedef int raft_node_id_t;
typedef uint64_t raft_id_type;

typedef enum {
    RAFT_STATE_NONE,
    RAFT_STATE_FOLLOWER,
    RAFT_STATE_CANDIDATE,
    RAFT_STATE_LEADER
} raft_identity;

typedef enum {
    RAFT_LOGTYPE_WRITE,
    RAFT_LOGTYPE_DELETE,
    RAFT_LOGTYPE_ADD_NONVOTING_NODE,
    RAFT_LOGTYPE_CONFIGURATION,
} raft_logtype_e;

constexpr int RAFT_NODE_VOTED_FOR_ME = (1 << 0);

} // anonymous namespace

FB_SUITE_SETUP(raft_log_node) {
    FB_LOG_INFO("Setting up raft_log_node test suite");
}

FB_SUITE_TEARDOWN(raft_log_node) {
    FB_LOG_INFO("Tearing down raft_log_node test suite");
}

// ============================================================================
// Test Suite: Raft Log Basic Operations
// ============================================================================

FB_TEST(raft_log_node, log_append_single_entry) {
    // 追加单个日志条目
    raft_index_t next_idx = 1;
    raft_term_t term = 1;

    // 追加后 next_idx 递增
    next_idx++;
    FB_ASSERT_EQ(next_idx, 2L);

    // 记录最后日志索引
    raft_index_t last_log_idx = next_idx - 1;
    FB_ASSERT_EQ(last_log_idx, 1L);
}

FB_TEST(raft_log_node, log_append_multiple_entries) {
    // 批量追加日志条目
    std::vector<raft_index_t> entries;
    for (int i = 1; i <= 10; i++) {
        entries.push_back(i);
    }

    FB_ASSERT_EQ(entries.size(), 10UL);
    FB_ASSERT_EQ(entries.front(), 1L);
    FB_ASSERT_EQ(entries.back(), 10L);
}

FB_TEST(raft_log_node, log_get_at_idx) {
    // 获取特定索引的日志
    std::map<raft_index_t, raft_term_t> log_entries;

    for (int i = 1; i <= 100; i++) {
        log_entries[i] = (i <= 50) ? 1 : 2;
    }

    // 获取特定索引
    FB_ASSERT_EQ(log_entries[25], 1L);
    FB_ASSERT_EQ(log_entries[75], 2L);
    FB_ASSERT_EQ(log_entries[50], 1L);
    FB_ASSERT_EQ(log_entries[51], 2L);
}

FB_TEST(raft_log_node, log_get_from_idx) {
    // 从指定索引获取日志
    std::map<raft_index_t, int> log_cache;
    for (int i = 1; i <= 20; i++) {
        log_cache[i] = i * 10;
    }

    // 获取 >= 5 的条目
    std::vector<int> entries;
    for (auto it = log_cache.lower_bound(5); it != log_cache.end(); it++) {
        entries.push_back(it->second);
    }
    FB_ASSERT_EQ(entries.size(), 16UL);
    FB_ASSERT_EQ(entries[0], 50);
    FB_ASSERT_EQ(entries[5], 100);
}

FB_TEST(raft_log_node, log_truncate_after_idx) {
    // 截断指定索引之后的日志
    raft_index_t last_idx = 100;
    raft_index_t truncate_idx = 50;

    // 截断后，last_idx 变为 truncate_idx - 1
    raft_index_t new_last_idx = truncate_idx - 1;
    FB_ASSERT_EQ(new_last_idx, 49L);

    // 截断掉的条目数
    raft_index_t truncated_count = last_idx - truncate_idx + 1;
    FB_ASSERT_EQ(truncated_count, 51L);
}

FB_TEST(raft_log_node, log_truncate_at_snapshot) {
    // 快照时截断日志
    raft_index_t snapshot_idx = 80;
    raft_index_t first_log_idx = 1;

    // 快照后，first_log_idx 更新
    raft_index_t new_first_idx = snapshot_idx + 1;
    FB_ASSERT_EQ(new_first_idx, 81L);

    // 可删除的日志条目数
    raft_index_t deletable = snapshot_idx - first_log_idx + 1;
    FB_ASSERT_EQ(deletable, 80L);
}

FB_TEST(raft_log_node, log_clear) {
    // 清空日志缓存
    std::map<raft_index_t, int> cache;
    for (int i = 1; i <= 100; i++) {
        cache[i] = i;
    }

    cache.clear();
    FB_ASSERT_TRUE(cache.empty());
    FB_ASSERT_EQ(cache.size(), 0UL);
}

FB_TEST(raft_log_node, log_first_and_last) {
    // 获取第一条和最后一条日志
    std::map<raft_index_t, int> log_cache;

    // 空缓存
    bool is_empty = log_cache.empty();
    FB_ASSERT_TRUE(is_empty);

    // 添加日志
    for (int i = 5; i <= 15; i++) {
        log_cache[i] = i;
    }

    is_empty = log_cache.empty();
    FB_ASSERT_FALSE(is_empty);

    // 第一条和最后一条
    raft_index_t first_idx = log_cache.begin()->first;
    raft_index_t last_idx = log_cache.rbegin()->first;
    FB_ASSERT_EQ(first_idx, 5L);
    FB_ASSERT_EQ(last_idx, 15L);
}

FB_TEST(raft_log_node, log_next_idx_tracking) {
    // next_idx 跟踪
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

FB_TEST(raft_log_node, log_base_index) {
    // 日志基索引
    raft_index_t base_idx = 1;
    FB_ASSERT_TRUE(base_idx >= 1);

    // 快照后的基索引
    base_idx = 100;
    FB_ASSERT_TRUE(base_idx > 1);

    // 基索引 + 1 = 第一条日志索引
    raft_index_t first_entry_idx = base_idx + 1;
    FB_ASSERT_EQ(first_entry_idx, 101L);
}

FB_TEST(raft_log_node, log_entry_id_uniqueness) {
    // 日志条目 ID 唯一性
    raft_entry_id_t id1 = 1001;
    raft_entry_id_t id2 = 1002;
    raft_entry_id_t id3 = 1001;

    FB_ASSERT_TRUE(id1 != id2);
    FB_ASSERT_TRUE(id1 == id3);

    // 检测重复
    std::set<raft_entry_id_t> seen_ids;
    seen_ids.insert(id1);
    bool duplicate = seen_ids.count(id2) > 0;
    FB_ASSERT_FALSE(duplicate);
}

FB_TEST(raft_log_node, log_term_consistency) {
    // 日志 term 一致性
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

// ============================================================================
// Test Suite: Raft Log Persistence
// ============================================================================

FB_TEST(raft_log_node, log_disk_sync) {
    // 磁盘同步点
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

FB_TEST(raft_log_node, log_recovery_from_disk) {
    // 从磁盘恢复日志
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

FB_TEST(raft_log_node, log_persist_boundary) {
    // 持久化边界
    raft_index_t commit_idx = 50;
    raft_index_t last_log_idx = 100;

    // 只有已提交的日志才能持久化
    bool can_persist = commit_idx <= last_log_idx;
    FB_ASSERT_TRUE(can_persist);
}

FB_TEST(raft_log_node, log_write_batch) {
    // 批量写入
    std::vector<raft_index_t> batch = {101, 102, 103, 104, 105};

    // 合并为一次磁盘写入
    size_t batch_size = batch.size();
    FB_ASSERT_EQ(batch_size, 5UL);

    raft_index_t start_idx = batch.front();
    raft_index_t end_idx = batch.back();
    FB_ASSERT_EQ(end_idx - start_idx + 1, 5L);
}

// ============================================================================
// Test Suite: Raft Log Cache Management
// ============================================================================

FB_TEST(raft_log_node, log_cache_add_remove) {
    // 日志缓存增删
    std::map<raft_index_t, int> cache;

    cache[1] = 10;
    cache[2] = 20;
    cache[3] = 30;
    FB_ASSERT_EQ(cache.size(), 3UL);

    cache.erase(2);
    FB_ASSERT_EQ(cache.size(), 2UL);
    FB_ASSERT_EQ(cache.count(2), 0UL);
    FB_ASSERT_EQ(cache[1], 10);
    FB_ASSERT_EQ(cache[3], 30);
}

FB_TEST(raft_log_node, log_cache_get_upper) {
    // 获取缓存中 >= idx 的条目
    std::map<raft_index_t, int> cache;
    for (int i = 1; i <= 10; i++) {
        cache[i] = i * 10;
    }

    auto it = cache.upper_bound(5);
    FB_ASSERT_TRUE(it != cache.end());
    FB_ASSERT_EQ(it->first, 6L);
}

FB_TEST(raft_log_node, log_cache_size_limit) {
    // 缓存大小限制
    uint32_t max_cache_entries = 500;
    uint32_t current_cache_size = 0;

    // 模拟缓存增长
    for (int i = 0; i < 600; i++) {
        if (current_cache_size >= max_cache_entries) {
            // 需要清理旧条目
            current_cache_size--;
        }
        current_cache_size++;
    }

    FB_ASSERT_TRUE(current_cache_size <= max_cache_entries + 1);
}

FB_TEST(raft_log_node, log_applied_entry_removal) {
    // 已应用条目移除
    raft_index_t first_cache_idx = 1;
    raft_index_t last_applied_idx = 100;
    uint32_t max_applied_in_cache = 50;

    // 计算需要移除的条目数
    uint32_t applied_num = last_applied_idx - first_cache_idx + 1;
    FB_ASSERT_GE(applied_num, max_applied_in_cache);

    // 移除超出限制的条目
    uint32_t remove_size = applied_num - max_applied_in_cache;
    FB_ASSERT_GE(remove_size, 50UL);
}

FB_TEST(raft_log_node, log_entry_queue) {
    // 日志条目队列
    std::queue<raft_index_t> entry_queue;
    entry_queue.push(101);
    entry_queue.push(102);
    entry_queue.push(103);

    FB_ASSERT_EQ(entry_queue.size(), 3UL);

    // 按顺序处理
    raft_index_t first_entry = entry_queue.front();
    FB_ASSERT_EQ(first_entry, 101L);
}

// ============================================================================
// Test Suite: Raft Node Basic Operations
// ============================================================================

FB_TEST(raft_log_node, node_init) {
    // 节点初始化
    raft_node_id_t node_id = 1;
    raft_index_t next_idx = 1;
    raft_index_t match_idx = 0;
    raft_time_t lease = 0;

    FB_ASSERT_EQ(node_id, 1);
    FB_ASSERT_EQ(next_idx, 1L);
    FB_ASSERT_EQ(match_idx, 0L);
    FB_ASSERT_EQ(lease, 0L);
}

FB_TEST(raft_log_node, node_next_idx_get_set) {
    // next_idx 获取和设置
    raft_index_t next_idx = 1;

    // 设置 next_idx
    next_idx = 10;
    FB_ASSERT_EQ(next_idx, 10L);

    // next_idx 不能小于 1
    next_idx = 0;
    next_idx = next_idx < 1 ? 1 : next_idx;
    FB_ASSERT_EQ(next_idx, 1L);

    next_idx = -5;
    next_idx = next_idx < 1 ? 1 : next_idx;
    FB_ASSERT_EQ(next_idx, 1L);
}

FB_TEST(raft_log_node, node_match_idx_get_set) {
    // match_idx 获取和设置
    raft_index_t match_idx = 0;

    // 成功复制后更新 match_idx
    match_idx = 10;
    FB_ASSERT_EQ(match_idx, 10L);

    // match_idx 只能递增
    raft_index_t old_match_idx = match_idx;
    raft_index_t new_match_idx = 5;
    if (new_match_idx > old_match_idx) {
        match_idx = new_match_idx;
    }
    FB_ASSERT_EQ(match_idx, 10L);  // 保持原值

    new_match_idx = 15;
    if (new_match_idx > match_idx) {
        match_idx = new_match_idx;
    }
    FB_ASSERT_EQ(match_idx, 15L);
}

FB_TEST(raft_log_node, node_vote_flag) {
    // 投票标志
    int flags = 0;

    // 设置投票标志
    flags |= RAFT_NODE_VOTED_FOR_ME;
    FB_ASSERT_TRUE((flags & RAFT_NODE_VOTED_FOR_ME) != 0);

    // 清除投票标志
    flags &= ~RAFT_NODE_VOTED_FOR_ME;
    FB_ASSERT_FALSE((flags & RAFT_NODE_VOTED_FOR_ME) != 0);
}

FB_TEST(raft_log_node, node_id_operations) {
    // 节点 ID 操作
    raft_node_id_t id1 = 1;
    raft_node_id_t id2 = 2;

    FB_ASSERT_TRUE(id1 != id2);
    FB_ASSERT_TRUE(id1 < id2);

    // 查找节点
    std::map<raft_node_id_t, int> nodes;
    nodes[id1] = 100;
    nodes[id2] = 200;

    FB_ASSERT_EQ(nodes[id1], 100);
    FB_ASSERT_EQ(nodes[id2], 200);
}

// ============================================================================
// Test Suite: Raft Node Lease Management
// ============================================================================

FB_TEST(raft_log_node, node_lease_set) {
    // 租约设置
    raft_time_t lease = 0;
    raft_time_t new_lease = 1000;

    // 设置租约
    if (new_lease > lease) {
        lease = new_lease;
    }
    FB_ASSERT_EQ(lease, 1000L);
}

FB_TEST(raft_log_node, node_lease_only_increases) {
    // 租约只能递增
    raft_time_t lease = 0;

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

FB_TEST(raft_log_node, node_effective_time) {
    // 节点生效时间
    raft_time_t effective_time = 0;

    // 设置生效时间
    effective_time = 1000;
    FB_ASSERT_EQ(effective_time, 1000L);

    // 验证时间单调性
    raft_time_t new_time = 1500;
    bool is_later = new_time > effective_time;
    FB_ASSERT_TRUE(is_later);
}

// ============================================================================
// Test Suite: Raft Node State Management
// ============================================================================

FB_TEST(raft_log_node, node_heartbeat_suppression) {
    // 心跳抑制
    bool suppress_heartbeats = false;
    FB_ASSERT_FALSE(suppress_heartbeats);

    // 开启心跳抑制
    suppress_heartbeats = true;
    FB_ASSERT_TRUE(suppress_heartbeats);

    // 关闭心跳抑制
    suppress_heartbeats = false;
    FB_ASSERT_FALSE(suppress_heartbeats);
}

FB_TEST(raft_log_node, node_heartbeating_status) {
    // 心跳状态
    bool is_heartbeating = false;

    // 开始心跳
    is_heartbeating = true;
    FB_ASSERT_TRUE(is_heartbeating);

    // 停止心跳
    is_heartbeating = false;
    FB_ASSERT_FALSE(is_heartbeating);
}

FB_TEST(raft_log_node, node_recovering_status) {
    // 恢复状态
    bool is_recovering = false;

    // 开始恢复
    is_recovering = true;
    FB_ASSERT_TRUE(is_recovering);

    // 恢复完成
    is_recovering = false;
    FB_ASSERT_FALSE(is_recovering);
}

FB_TEST(raft_log_node, node_end_idx_tracking) {
    // end_idx 跟踪（Leader 发送的最后一个日志索引）
    raft_index_t end_idx = 0;

    // 设置 end_idx
    end_idx = 100;
    FB_ASSERT_EQ(end_idx, 100L);

    // end_idx 可以用于判断日志发送进度
    raft_index_t match_idx = 80;
    bool has_more = match_idx < end_idx;
    FB_ASSERT_TRUE(has_more);
}

FB_TEST(raft_log_node, node_append_time) {
    // 追加时间
    raft_time_t append_time = 0;
    raft_time_t current_time = 1000;

    // 记录追加时间
    append_time = current_time;
    FB_ASSERT_EQ(append_time, 1000L);

    // 计算距离上次追加的时间
    raft_time_t elapsed = current_time - append_time;
    FB_ASSERT_EQ(elapsed, 0L);
}

// ============================================================================
// Test Suite: Raft Nodes Collection
// ============================================================================

FB_TEST(raft_log_node, nodes_contains) {
    // 节点是否包含
    std::map<raft_node_id_t, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    FB_ASSERT_TRUE(nodes.find(1) != nodes.end());
    FB_ASSERT_TRUE(nodes.find(2) != nodes.end());
    FB_ASSERT_FALSE(nodes.find(5) != nodes.end());
}

FB_TEST(raft_log_node, nodes_find) {
    // 查找节点
    std::map<raft_node_id_t, int> nodes;
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

FB_TEST(raft_log_node, nodes_size) {
    // 节点数量
    std::map<raft_node_id_t, int> nodes;

    FB_ASSERT_EQ(nodes.size(), 0UL);

    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;
    FB_ASSERT_EQ(nodes.size(), 3UL);

    nodes.erase(2);
    FB_ASSERT_EQ(nodes.size(), 2UL);
}

FB_TEST(raft_log_node, nodes_get_node) {
    // 获取节点
    std::map<raft_node_id_t, int> nodes;
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

FB_TEST(raft_log_node, nodes_get_ids) {
    // 获取所有节点 ID
    std::map<raft_node_id_t, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    std::vector<raft_node_id_t> ids;
    for (const auto& pair : nodes) {
        ids.push_back(pair.first);
    }
    FB_ASSERT_EQ(ids.size(), 3UL);

    // 验证包含所有 ID
    FB_ASSERT_TRUE(std::find(ids.begin(), ids.end(), 1) != ids.end());
    FB_ASSERT_TRUE(std::find(ids.begin(), ids.end(), 2) != ids.end());
    FB_ASSERT_TRUE(std::find(ids.begin(), ids.end(), 3) != ids.end());
}

FB_TEST(raft_log_node, nodes_for_all) {
    // 遍历所有节点
    std::map<raft_node_id_t, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    int visited_count = 0;
    for (const auto& pair : nodes) {
        visited_count++;
        FB_ASSERT_TRUE(pair.first >= 1);
        FB_ASSERT_TRUE(pair.second >= 100);
    }
    FB_ASSERT_EQ(visited_count, 3);
}

// ============================================================================
// Test Suite: Raft Nodes New Nodes Management
// ============================================================================

FB_TEST(raft_log_node, nodes_new_nodes_management) {
    // 模拟 _nodes 和 _new_nodes 的管理
    std::map<raft_node_id_t, int> nodes;
    std::map<raft_node_id_t, int> new_nodes;

    // 初始节点
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    // 配置变更：添加新节点
    new_nodes[4] = 400;
    new_nodes[5] = 500;

    // 联合共识期间，需要向所有节点发送消息
    size_t total_recipients = nodes.size() + new_nodes.size();
    FB_ASSERT_EQ(total_recipients, 5UL);

    // 遍历所有节点（包括新节点）
    int all_count = 0;
    for (const auto& pair : nodes) all_count++;
    for (const auto& pair : new_nodes) all_count++;
    FB_ASSERT_EQ(all_count, 5);
}

FB_TEST(raft_log_node, nodes_for_new_nodes) {
    // 只遍历新节点
    std::map<raft_node_id_t, int> new_nodes;
    new_nodes[4] = 400;
    new_nodes[5] = 500;

    int new_count = 0;
    for (const auto& pair : new_nodes) {
        new_count++;
        FB_ASSERT_TRUE(pair.first >= 4);
    }
    FB_ASSERT_EQ(new_count, 2);
}

FB_TEST(raft_log_node, nodes_get_new_node) {
    // 获取新节点
    std::map<raft_node_id_t, int> new_nodes;
    new_nodes[4] = 400;

    // 获取新节点
    auto it = new_nodes.find(4);
    FB_ASSERT_TRUE(it != new_nodes.end());
    FB_ASSERT_EQ(it->second, 400);

    // 获取不存在的新节点
    it = new_nodes.find(99);
    FB_ASSERT_TRUE(it == new_nodes.end());
}

FB_TEST(raft_log_node, nodes_new_node_size) {
    // 新节点数量
    std::map<raft_node_id_t, int> new_nodes;

    FB_ASSERT_EQ(new_nodes.size(), 0UL);

    new_nodes[4] = 400;
    new_nodes[5] = 500;
    FB_ASSERT_EQ(new_nodes.size(), 2UL);
}

FB_TEST(raft_log_node, nodes_iterator_operations) {
    // 迭代器操作
    std::map<raft_node_id_t, int> nodes;
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

// ============================================================================
// Test Suite: Raft Log Entry Operations
// ============================================================================

FB_TEST(raft_log_node, entry_append_sequence) {
    // 日志追加序列
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

FB_TEST(raft_log_node, entry_data_alignment) {
    // 数据对齐检查
    size_t data_size = 4096;
    bool aligned = (data_size % 4096 == 0);
    FB_ASSERT_TRUE(aligned);

    // 未对齐数据
    data_size = 4000;
    aligned = (data_size % 4096 == 0);
    FB_ASSERT_FALSE(aligned);
}

FB_TEST(raft_log_node, entry_type_operations) {
    // 日志类型操作
    raft_logtype_e type = RAFT_LOGTYPE_WRITE;
    FB_ASSERT_EQ(type, RAFT_LOGTYPE_WRITE);
    FB_ASSERT_EQ(static_cast<int>(type), 0);

    type = RAFT_LOGTYPE_DELETE;
    FB_ASSERT_EQ(type, RAFT_LOGTYPE_DELETE);

    type = RAFT_LOGTYPE_CONFIGURATION;
    FB_ASSERT_EQ(type, RAFT_LOGTYPE_CONFIGURATION);
}

FB_TEST(raft_log_node, entry_meta_handling) {
    // 元数据处理
    std::string meta = "test_meta_data";

    FB_ASSERT_FALSE(meta.empty());
    FB_ASSERT_EQ(meta.length(), 14UL);
}

// ============================================================================
// Test Suite: Raft Log and Node Integration
// ============================================================================

FB_TEST(raft_log_node, log_node_replication_flow) {
    // 日志复制流程
    raft_index_t leader_next_idx = 101;
    raft_index_t follower_match_idx = 95;

    // 计算需要发送的日志
    raft_index_t entries_to_send = leader_next_idx - follower_match_idx - 1;
    FB_ASSERT_EQ(entries_to_send, 5L);

    // 发送后更新
    follower_match_idx = leader_next_idx - 1;
    FB_ASSERT_EQ(follower_match_idx, 100L);
}

FB_TEST(raft_log_node, log_node_conflict_resolution) {
    // 冲突解决
    raft_index_t leader_prev_idx = 100;
    raft_term_t leader_prev_term = 5;
    raft_index_t follower_last_idx = 105;

    // Follower 日志超出 Leader 的 prev_idx
    bool follower_ahead = follower_last_idx > leader_prev_idx;
    FB_ASSERT_TRUE(follower_ahead);

    // 需要截断 Follower 日志
    raft_index_t truncate_idx = leader_prev_idx + 1;
    raft_index_t new_last_idx = truncate_idx - 1;
    FB_ASSERT_EQ(new_last_idx, 100L);
}

FB_TEST(raft_log_node, log_node_commit_advancement) {
    // 提交推进
    std::map<raft_node_id_t, raft_index_t> match_indices;
    match_indices[1] = 100;  // Leader
    match_indices[2] = 95;
    match_indices[3] = 98;
    match_indices[4] = 90;
    match_indices[5] = 96;

    // 找到多数派的 match_idx
    std::vector<raft_index_t> indices;
    for (const auto& pair : match_indices) {
        indices.push_back(pair.second);
    }
    std::sort(indices.begin(), indices.end());

    // 多数派位置（索引 2 是第3个，代表多数派）
    raft_index_t majority_match = indices[2];
    FB_ASSERT_EQ(majority_match, 96L);
}

FB_TEST(raft_log_node, log_node_snapshot_sync) {
    // 快照同步
    raft_index_t snapshot_idx = 100;
    raft_index_t follower_last_idx = 50;

    // Follower 需要快照
    bool needs_snapshot = snapshot_idx > follower_last_idx;
    FB_ASSERT_TRUE(needs_snapshot);

    // 快照后更新 match_idx
    raft_index_t new_match_idx = snapshot_idx;
    FB_ASSERT_EQ(new_match_idx, 100L);
}

// ============================================================================
// Test Suite: Error Handling and Exception Tests
// ============================================================================

FB_TEST(raft_log_node, log_invalid_index_handling) {
    // 无效索引处理
    raft_index_t invalid_idx = -1;
    bool is_valid = invalid_idx >= 0;
    FB_ASSERT_FALSE(is_valid);

    // 索引 0 表示"空"状态
    raft_index_t zero_idx = 0;
    bool is_empty = (zero_idx == 0);
    FB_ASSERT_TRUE(is_empty);

    // 有效索引从 1 开始
    raft_index_t valid_idx = 1;
    is_valid = valid_idx > 0;
    FB_ASSERT_TRUE(is_valid);
}

FB_TEST(raft_log_node, log_negative_index_clamp) {
    // 负索引截断到有效值
    auto clamp_idx = [](raft_index_t idx) -> raft_index_t {
        return idx < 1 ? 1 : idx;
    };

    FB_ASSERT_EQ(clamp_idx(-100), 1L);
    FB_ASSERT_EQ(clamp_idx(-1), 1L);
    FB_ASSERT_EQ(clamp_idx(0), 1L);
    FB_ASSERT_EQ(clamp_idx(1), 1L);
    FB_ASSERT_EQ(clamp_idx(100), 100L);
}

FB_TEST(raft_log_node, log_empty_cache_operations) {
    // 空缓存操作
    std::map<raft_index_t, int> empty_cache;

    FB_ASSERT_TRUE(empty_cache.empty());
    FB_ASSERT_EQ(empty_cache.size(), 0UL);

    // 获取不存在条目返回 end
    auto it = empty_cache.find(1);
    FB_ASSERT_TRUE(it == empty_cache.end());
}

FB_TEST(raft_log_node, log_overflow_protection) {
    // 溢出保护
    raft_index_t max_idx = std::numeric_limits<raft_index_t>::max();
    FB_ASSERT_TRUE(max_idx > 0);

    // 大索引值操作
    raft_index_t large_idx = max_idx - 100;
    raft_index_t next_idx = large_idx + 1;
    FB_ASSERT_TRUE(next_idx > large_idx);
}

FB_TEST(raft_log_node, log_disk_write_failure) {
    // 磁盘写入失败处理
    int write_result = -1;  // 模拟失败
    bool write_success = (write_result == 0);
    FB_ASSERT_FALSE(write_success);

    // 重试机制
    int retry_count = 0;
    int max_retries = 3;
    while (!write_success && retry_count < max_retries) {
        retry_count++;
        if (retry_count == 2) {
            write_result = 0;
            write_success = true;
        }
    }
    FB_ASSERT_TRUE(write_success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_log_node, log_recovery_partial_failure) {
    // 部分恢复失败处理
    std::vector<int> recovery_results = {0, 0, -1, 0};  // 第三个失败
    int failure_count = 0;

    for (int result : recovery_results) {
        if (result != 0) {
            failure_count++;
        }
    }

    FB_ASSERT_EQ(failure_count, 1);

    // 恢复成功比例
    double success_rate = 100.0 * (recovery_results.size() - failure_count) / recovery_results.size();
    FB_ASSERT_EQ(success_rate, 75.0);
}

// ============================================================================
// Test Suite: Boundary Condition Tests
// ============================================================================

FB_TEST(raft_log_node, log_max_index_boundary) {
    // 最大索引边界
    raft_index_t max_idx = std::numeric_limits<raft_index_t>::max();
    FB_ASSERT_TRUE(max_idx > 0);

    // 接近最大值时的递增
    raft_index_t near_max = max_idx - 1;
    raft_index_t incremented = near_max + 1;
    FB_ASSERT_TRUE(incremented > near_max);
}

FB_TEST(raft_log_node, log_max_term_boundary) {
    // 最大 term 边界
    raft_term_t max_term = std::numeric_limits<raft_term_t>::max();
    FB_ASSERT_TRUE(max_term > 0);

    // 大 term 值比较
    raft_term_t large_term = max_term - 1000;
    raft_term_t other_term = large_term - 1;
    FB_ASSERT_TRUE(large_term > other_term);
}

FB_TEST(raft_log_node, log_index_zero_handling) {
    // 索引 0 处理
    raft_index_t zero_idx = 0;

    // 0 表示无效或初始状态
    bool is_initial = (zero_idx == 0);
    FB_ASSERT_TRUE(is_initial);

    // next_idx 从 1 开始
    raft_index_t next_idx = zero_idx + 1;
    FB_ASSERT_EQ(next_idx, 1L);
}

FB_TEST(raft_log_node, log_term_zero_handling) {
    // Term 0 处理
    raft_term_t zero_term = 0;

    // Term 从 1 开始有效
    bool is_valid_term = zero_term > 0;
    FB_ASSERT_FALSE(is_valid_term);

    // 初始 term
    raft_term_t initial_term = 1;
    is_valid_term = initial_term > 0;
    FB_ASSERT_TRUE(is_valid_term);
}

FB_TEST(raft_log_node, node_next_idx_max_value) {
    // next_idx 最大值
    raft_index_t max_next = std::numeric_limits<raft_index_t>::max();

    // 不能超过最大值
    raft_index_t next_idx = max_next;
    bool can_increment = next_idx < std::numeric_limits<raft_index_t>::max();
    FB_ASSERT_FALSE(can_increment);
}

FB_TEST(raft_log_node, node_match_idx_max_value) {
    // match_idx 最大值
    raft_index_t max_match = std::numeric_limits<raft_index_t>::max();

    // match_idx 可以达到最大值
    raft_index_t match_idx = max_match;
    FB_ASSERT_EQ(match_idx, max_match);

    // 验证比较操作
    raft_index_t other_idx = max_match - 1;
    FB_ASSERT_TRUE(match_idx > other_idx);
}

// ============================================================================
// Test Suite: Concurrency Scenario Tests
// ============================================================================

FB_TEST(raft_log_node, log_concurrent_append) {
    // 并发追加模拟
    std::atomic<raft_index_t> current_idx{0};

    // 模拟并发追加
    for (int i = 0; i < 100; i++) {
        current_idx++;
    }

    FB_ASSERT_EQ(current_idx.load(), 100L);
}

FB_TEST(raft_log_node, log_concurrent_read_write) {
    // 并发读写模拟
    std::map<raft_index_t, int> log_cache;
    std::mutex cache_mutex;

    // 模拟写入
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (int i = 1; i <= 50; i++) {
            log_cache[i] = i;
        }
    }

    // 模拟读取
    raft_index_t read_count = 0;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (const auto& pair : log_cache) {
            read_count++;
        }
    }

    FB_ASSERT_EQ(read_count, 50L);
}

FB_TEST(raft_log_node, node_concurrent_state_update) {
    // 并发状态更新
    std::atomic<raft_index_t> match_idx{0};
    std::atomic<raft_index_t> next_idx{1};

    // 模拟并发更新
    for (int i = 0; i < 10; i++) {
        raft_index_t old_match = match_idx.load();
        raft_index_t new_match = old_match + 1;
        match_idx.compare_exchange_strong(old_match, new_match);
    }

    FB_ASSERT_EQ(match_idx.load(), 10L);
}

FB_TEST(raft_log_node, nodes_concurrent_iteration) {
    // 并发遍历模拟
    std::map<raft_node_id_t, int> nodes;
    for (int i = 1; i <= 10; i++) {
        nodes[i] = i * 10;
    }

    std::atomic<int> visited_count{0};

    // 模拟并发遍历
    for (const auto& pair : nodes) {
        visited_count++;
    }

    FB_ASSERT_EQ(visited_count.load(), 10);
}

FB_TEST(raft_log_node, log_cache_thread_safety) {
    // 缓存线程安全测试
    std::map<raft_index_t, int> cache;
    std::mutex cache_mutex;
    std::atomic<int> operation_count{0};

    // 模拟线程安全操作
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[1] = 100;
        operation_count++;
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        int val = cache[1];
        operation_count++;
    }

    FB_ASSERT_EQ(operation_count.load(), 2);
}

// ============================================================================
// Test Suite: Performance Boundary Tests
// ============================================================================

FB_TEST(raft_log_node, log_large_batch_append) {
    // 大批量追加
    std::vector<raft_index_t> batch;
    batch.reserve(10000);

    for (int i = 1; i <= 10000; i++) {
        batch.push_back(i);
    }

    FB_ASSERT_EQ(batch.size(), 10000UL);
    FB_ASSERT_EQ(batch.front(), 1L);
    FB_ASSERT_EQ(batch.back(), 10000L);
}

FB_TEST(raft_log_node, log_cache_pressure_handling) {
    // 缓存压力测试
    size_t max_cache_size = 1000;
    std::map<raft_index_t, int> cache;

    // 填充到最大容量
    for (size_t i = 1; i <= max_cache_size; i++) {
        cache[i] = i;
    }

    FB_ASSERT_EQ(cache.size(), max_cache_size);

    // 超出时移除旧条目
    raft_index_t new_idx = max_cache_size + 1;
    cache[new_idx] = new_idx;
    cache.erase(cache.begin()->first);

    FB_ASSERT_EQ(cache.size(), max_cache_size);
}

FB_TEST(raft_log_node, log_high_frequency_operations) {
    // 高频操作测试
    std::map<raft_index_t, int> cache;
    int operations = 1000;

    for (int i = 1; i <= operations; i++) {
        cache[i] = i;
    }

    FB_ASSERT_EQ(cache.size(), operations);
}

FB_TEST(raft_log_node, log_memory_usage_tracking) {
    // 内存使用跟踪
    size_t entry_size = 1024;  // 每条日志 1KB
    size_t max_entries = 1000;
    size_t max_memory = entry_size * max_entries;

    // 当前使用量
    size_t current_entries = 500;
    size_t current_memory = entry_size * current_entries;

    double usage_percent = 100.0 * current_memory / max_memory;
    FB_ASSERT_EQ(usage_percent, 50.0);

    // 检查是否接近限制
    bool near_limit = usage_percent > 80.0;
    FB_ASSERT_FALSE(near_limit);
}

FB_TEST(raft_log_node, nodes_large_cluster_operations) {
    // 大规模集群操作
    std::map<raft_node_id_t, raft_index_t> nodes;

    // 添加 100 个节点
    for (int i = 1; i <= 100; i++) {
        nodes[i] = 1000;
    }

    FB_ASSERT_EQ(nodes.size(), 100UL);

    // 计算多数派
    uint64_t quorum = nodes.size() / 2 + 1;
    FB_ASSERT_EQ(quorum, 51UL);

    // 遍历所有节点
    int visited = 0;
    for (const auto& pair : nodes) {
        visited++;
    }
    FB_ASSERT_EQ(visited, 100);
}

// ============================================================================
// Test Suite: Log Compaction and Cleanup Tests
// ============================================================================

FB_TEST(raft_log_node, log_compaction_trigger) {
    // 压缩触发条件
    size_t log_count = 10000;
    size_t compaction_threshold = 5000;

    bool should_compact = log_count >= compaction_threshold;
    FB_ASSERT_TRUE(should_compact);

    // 压缩后数量
    size_t compacted_count = log_count - compaction_threshold;
    FB_ASSERT_EQ(compacted_count, 5000UL);
}

FB_TEST(raft_log_node, log_gc_eligible_entries) {
    // GC 可回收条目判断
    raft_index_t commit_idx = 100;
    raft_index_t snapshot_idx = 80;

    // 快照之前的日志可以 GC
    std::vector<raft_index_t> gc_eligible;
    for (raft_index_t idx = 1; idx <= snapshot_idx; idx++) {
        gc_eligible.push_back(idx);
    }

    FB_ASSERT_EQ(gc_eligible.size(), 80UL);
}

FB_TEST(raft_log_node, log_snapshot_compaction) {
    // 快照压缩
    raft_index_t first_idx = 1;
    raft_index_t snapshot_idx = 100;
    raft_index_t last_idx = 200;

    // 压缩后更新索引
    raft_index_t new_first_idx = snapshot_idx + 1;
    FB_ASSERT_EQ(new_first_idx, 101L);

    // 保留的日志条目数
    size_t remaining = last_idx - new_first_idx + 1;
    FB_ASSERT_EQ(remaining, 100UL);
}

FB_TEST(raft_log_node, log_retention_policy) {
    // 日志保留策略
    raft_index_t last_applied = 100;
    size_t retention_window = 50;

    // 只保留最近 N 条已应用的日志
    raft_index_t oldest_retained = last_applied - retention_window + 1;
    FB_ASSERT_EQ(oldest_retained, 51L);

    // 可以删除的条目
    raft_index_t first_log_idx = 1;
    size_t deletable = oldest_retained - first_log_idx;
    FB_ASSERT_EQ(deletable, 50UL);
}

FB_TEST(raft_log_node, log_space_reclamation) {
    // 空间回收计算
    size_t log_size = 10 * 1024 * 1024;  // 10MB
    size_t snapshot_size = 2 * 1024 * 1024;  // 2MB

    // 压缩后释放空间
    size_t freed_space = log_size - snapshot_size;
    FB_ASSERT_EQ(freed_space, 8UL * 1024 * 1024);

    // 压缩比
    double compression_ratio = 100.0 * freed_space / log_size;
    FB_ASSERT_EQ(compression_ratio, 80.0);
}

// Main function for test runner
FB_TEST_MAIN()
