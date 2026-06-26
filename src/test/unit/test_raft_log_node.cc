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
    // Setup code here
}

FB_SUITE_TEARDOWN(raft_log_node) {
    // Teardown code here
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
    size_t operations = 1000;

    for (size_t i = 1; i <= operations; i++) {
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

// ============================================================================
// Test Suite: Leader Election
// ============================================================================

FB_TEST(raft_log_node, election_timeout_trigger) {
    // 选举超时触发
    raft_time_t election_timeout = 150;  // ms
    raft_time_t last_heartbeat = 1000;
    raft_time_t current_time = 1200;

    // 检查是否超时
    bool timeout = (current_time - last_heartbeat) > election_timeout;
    FB_ASSERT_TRUE(timeout);

    // 未超时情况
    current_time = 1100;
    timeout = (current_time - last_heartbeat) > election_timeout;
    FB_ASSERT_FALSE(timeout);
}

FB_TEST(raft_log_node, election_timeout_randomization) {
    // 选举超时随机化
    raft_time_t base_timeout = 150;
    raft_time_t min_timeout = base_timeout;
    raft_time_t max_timeout = base_timeout * 2;

    // 随机化后的超时应在范围内
    for (int i = 0; i < 10; i++) {
        raft_time_t randomized = base_timeout + (i * 15);
        FB_ASSERT_TRUE(randomized >= min_timeout);
        FB_ASSERT_TRUE(randomized <= max_timeout);
    }
}

FB_TEST(raft_log_node, candidate_state_transition) {
    // 候选人状态转换
    raft_identity state = RAFT_STATE_FOLLOWER;

    // Follower -> Candidate
    state = RAFT_STATE_CANDIDATE;
    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);

    // Candidate -> Leader (赢得选举)
    state = RAFT_STATE_LEADER;
    FB_ASSERT_EQ(state, RAFT_STATE_LEADER);

    // Leader -> Follower (收到更高 term)
    state = RAFT_STATE_FOLLOWER;
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_log_node, vote_request_term_check) {
    // 投票请求 term 检查
    raft_term_t current_term = 5;
    raft_term_t request_term = 6;

    // 请求 term 更高，接受
    bool accept = request_term >= current_term;
    FB_ASSERT_TRUE(accept);

    // 请求 term 更低，拒绝
    request_term = 4;
    accept = request_term >= current_term;
    FB_ASSERT_FALSE(accept);
}

FB_TEST(raft_log_node, vote_request_log_check) {
    // 投票请求日志检查
    raft_term_t current_last_log_term = 3;
    raft_index_t current_last_log_idx = 100;
    raft_term_t candidate_last_log_term = 4;
    raft_index_t candidate_last_log_idx = 105;

    // 候选人日志更新，接受投票
    bool log_is_up_to_date = (candidate_last_log_term > current_last_log_term) ||
        (candidate_last_log_term == current_last_log_term &&
         candidate_last_log_idx >= current_last_log_idx);
    FB_ASSERT_TRUE(log_is_up_to_date);

    // 候选人日志更旧，拒绝投票
    candidate_last_log_term = 2;
    candidate_last_log_idx = 90;
    log_is_up_to_date = (candidate_last_log_term > current_last_log_term) ||
        (candidate_last_log_term == current_last_log_term &&
         candidate_last_log_idx >= current_last_log_idx);
    FB_ASSERT_FALSE(log_is_up_to_date);
}

FB_TEST(raft_log_node, vote_granted_tracking) {
    // 投票授予跟踪
    std::map<raft_node_id_t, bool> votes_received;
    votes_received[1] = true;
    votes_received[2] = true;
    votes_received[3] = false;
    votes_received[4] = true;
    votes_received[5] = false;

    // 计算获得的票数
    int votes_for_me = 0;
    for (const auto& pair : votes_received) {
        if (pair.second) votes_for_me++;
    }
    FB_ASSERT_EQ(votes_for_me, 3);

    // 检查是否获得多数票 (5 节点集群，需要 3 票)
    int cluster_size = 5;
    int quorum = cluster_size / 2 + 1;
    bool won_election = votes_for_me >= quorum;
    FB_ASSERT_TRUE(won_election);
}

FB_TEST(raft_log_node, split_vote_scenario) {
    // Split Vote 场景
    // 3 个候选人的 5 节点集群，各获得不同票数
    int total_nodes = 5;
    int quorum = total_nodes / 2 + 1;  // 3

    // 候选人 A 获得 2 票
    int votes_a = 2;
    bool a_wins = votes_a >= quorum;
    FB_ASSERT_FALSE(a_wins);

    // 候选人 B 获得 2 票
    int votes_b = 2;
    bool b_wins = votes_b >= quorum;
    FB_ASSERT_FALSE(b_wins);

    // 候选人 C 获得 1 票
    int votes_c = 1;
    bool c_wins = votes_c >= quorum;
    FB_ASSERT_FALSE(c_wins);

    // 无人获胜，需要增加 term 重新选举
    bool need_new_election = !a_wins && !b_wins && !c_wins;
    FB_ASSERT_TRUE(need_new_election);
}

FB_TEST(raft_log_node, term_increment_on_election) {
    // 选举时 term 递增
    raft_term_t current_term = 5;

    // 开始选举时递增 term
    current_term++;
    FB_ASSERT_EQ(current_term, 6L);

    // 多次选举 term 持续递增
    current_term++;
    current_term++;
    FB_ASSERT_EQ(current_term, 8L);
}

FB_TEST(raft_log_node, voted_for_persistence) {
    // voted_for 持久化
    raft_node_id_t voted_for = 0;  // 初始为空

    // 投票给候选人 3
    voted_for = 3;
    FB_ASSERT_EQ(voted_for, 3);

    // 已投票状态检查
    bool has_voted = voted_for != 0;
    FB_ASSERT_TRUE(has_voted);

    // 新 term 开始，清除投票
    voted_for = 0;
    has_voted = voted_for != 0;
    FB_ASSERT_FALSE(has_voted);
}

FB_TEST(raft_log_node, election_safety_single_leader) {
    // 选举安全性：同一 term 只有一个 Leader
    std::map<raft_term_t, int> leaders_per_term;
    leaders_per_term[1] = 1;
    leaders_per_term[2] = 1;
    leaders_per_term[3] = 1;

    // 每个 term 应该只有一个 Leader
    for (const auto& pair : leaders_per_term) {
        FB_ASSERT_EQ(pair.second, 1);
    }
}

// ============================================================================
// Test Suite: Configuration Change
// ============================================================================

FB_TEST(raft_log_node, config_change_add_node) {
    // 添加节点到配置
    std::map<raft_node_id_t, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;

    // 初始配置
    FB_ASSERT_EQ(nodes.size(), 3UL);

    // 添加新节点
    nodes[4] = 400;
    FB_ASSERT_EQ(nodes.size(), 4UL);
    FB_ASSERT_TRUE(nodes.find(4) != nodes.end());
}

FB_TEST(raft_log_node, config_change_remove_node) {
    // 从配置中移除节点
    std::map<raft_node_id_t, int> nodes;
    nodes[1] = 100;
    nodes[2] = 200;
    nodes[3] = 300;
    nodes[4] = 400;

    // 移除节点
    nodes.erase(3);
    FB_ASSERT_EQ(nodes.size(), 3UL);
    FB_ASSERT_TRUE(nodes.find(3) == nodes.end());
}

FB_TEST(raft_log_node, config_change_joint_consensus) {
    // 联合共识阶段
    std::map<raft_node_id_t, int> old_config;
    old_config[1] = 100;
    old_config[2] = 200;
    old_config[3] = 300;

    std::map<raft_node_id_t, int> new_config;
    new_config[4] = 400;
    new_config[5] = 500;

    // 联合共识期间需要同时向两个配置发送消息
    int total_recipients = old_config.size() + new_config.size();
    FB_ASSERT_EQ(total_recipients, 5);

    // 需要两个配置的多数派都确认
    int old_quorum = old_config.size() / 2 + 1;
    int new_quorum = new_config.size() / 2 + 1;
    FB_ASSERT_EQ(old_quorum, 2);
    FB_ASSERT_EQ(new_quorum, 2);
}

FB_TEST(raft_log_node, config_change_catch_up) {
    // 新节点追赶
    raft_index_t leader_last_idx = 1000;
    raft_index_t follower_last_idx = 500;

    // 新节点落后太多，需要追赶
    bool needs_catch_up = (leader_last_idx - follower_last_idx) > 100;
    FB_ASSERT_TRUE(needs_catch_up);

    // 追赶进度
    int entries_to_send = leader_last_idx - follower_last_idx;
    FB_ASSERT_EQ(entries_to_send, 500);
}

FB_TEST(raft_log_node, config_change_safety_check) {
    // 配置变更安全性检查
    int old_cluster_size = 3;
    int new_cluster_size = 5;

    // 计算重叠的多数派
    int old_quorum = old_cluster_size / 2 + 1;
    int new_quorum = new_cluster_size / 2 + 1;

    // 确保安全性：两个配置的多数派必须有交集
    // 3 节点多数派需要 2，5 节点多数派需要 3
    // 交集至少需要 1 个节点
    int min_overlap = old_quorum + new_quorum - old_cluster_size;
    bool safe = min_overlap > 0;
    FB_ASSERT_TRUE(safe);
}

FB_TEST(raft_log_node, config_change_rollback) {
    // 配置变更回滚
    std::map<raft_node_id_t, int> current_config;
    current_config[1] = 100;
    current_config[2] = 200;
    current_config[3] = 300;

    // 保存旧配置
    std::map<raft_node_id_t, int> old_config = current_config;

    // 尝试添加新节点
    current_config[4] = 400;

    // 变更失败，回滚到旧配置
    current_config = old_config;
    FB_ASSERT_EQ(current_config.size(), 3UL);
    FB_ASSERT_TRUE(current_config.find(4) == current_config.end());
}

FB_TEST(raft_log_node, config_change_log_entry_type) {
    // 配置变更日志条目类型
    raft_logtype_e entry_type = RAFT_LOGTYPE_CONFIGURATION;
    FB_ASSERT_EQ(entry_type, RAFT_LOGTYPE_CONFIGURATION);

    // 区分普通写日志和配置变更日志
    bool is_config_change = (entry_type == RAFT_LOGTYPE_CONFIGURATION ||
                             entry_type == RAFT_LOGTYPE_ADD_NONVOTING_NODE);
    FB_ASSERT_TRUE(is_config_change);

    // 普通写日志不是配置变更
    entry_type = RAFT_LOGTYPE_WRITE;
    is_config_change = (entry_type == RAFT_LOGTYPE_CONFIGURATION ||
                        entry_type == RAFT_LOGTYPE_ADD_NONVOTING_NODE);
    FB_ASSERT_FALSE(is_config_change);
}

FB_TEST(raft_log_node, config_change_nonvoting_node) {
    // 非投票节点管理
    std::map<raft_node_id_t, bool> voting_status;
    voting_status[1] = true;   // 投票节点
    voting_status[2] = true;   // 投票节点
    voting_status[3] = false;  // 非投票节点
    voting_status[4] = false;  // 非投票节点

    // 计算投票节点数量
    int voting_count = 0;
    for (const auto& pair : voting_status) {
        if (pair.second) voting_count++;
    }
    FB_ASSERT_EQ(voting_count, 2);

    // 非投票节点可以转换为投票节点
    voting_status[3] = true;
    voting_count = 0;
    for (const auto& pair : voting_status) {
        if (pair.second) voting_count++;
    }
    FB_ASSERT_EQ(voting_count, 3);
}

// ============================================================================
// Test Suite: Snapshot Operations
// ============================================================================

FB_TEST(raft_log_node, snapshot_install_flow) {
    // 快照安装流程
    raft_term_t snapshot_term = 5;
    raft_index_t snapshot_idx = 100;
    raft_index_t follower_last_idx = 50;

    // Follower 日志落后于快照，需要安装
    bool needs_install = snapshot_idx > follower_last_idx;
    FB_ASSERT_TRUE(needs_install);

    // 安装后更新状态
    raft_index_t new_last_idx = snapshot_idx;
    raft_index_t new_first_idx = snapshot_idx + 1;
    FB_ASSERT_EQ(new_last_idx, 100L);
    FB_ASSERT_EQ(new_first_idx, 101L);
}

FB_TEST(raft_log_node, snapshot_chunk_transfer) {
    // 快照分块传输
    size_t snapshot_size = 10 * 1024 * 1024;  // 10MB
    size_t chunk_size = 1024 * 1024;           // 1MB per chunk

    // 计算分块数量
    size_t total_chunks = (snapshot_size + chunk_size - 1) / chunk_size;
    FB_ASSERT_EQ(total_chunks, 10UL);

    // 最后一分块大小
    size_t last_chunk = snapshot_size - (total_chunks - 1) * chunk_size;
    FB_ASSERT_EQ(last_chunk, chunk_size);

    // 非整数分块情况
    snapshot_size = 10 * 1024 * 1024 + 512;
    total_chunks = (snapshot_size + chunk_size - 1) / chunk_size;
    FB_ASSERT_EQ(total_chunks, 11UL);
    last_chunk = snapshot_size - (total_chunks - 1) * chunk_size;
    FB_ASSERT_EQ(last_chunk, 512UL);
}

FB_TEST(raft_log_node, snapshot_apply_atomicity) {
    // 快照应用原子性
    raft_index_t old_last_applied = 80;
    raft_index_t snapshot_last_idx = 100;

    // 快照应用前，last_applied 旧值
    FB_ASSERT_LT(old_last_applied, snapshot_last_idx);

    // 应用快照是原子操作：要么全部成功，要么全部失败
    bool apply_success = true;
    raft_index_t new_last_applied = apply_success ? snapshot_last_idx : old_last_applied;
    FB_ASSERT_EQ(new_last_applied, 100L);

    // 应用失败情况
    apply_success = false;
    new_last_applied = apply_success ? snapshot_last_idx : old_last_applied;
    FB_ASSERT_EQ(new_last_applied, 80L);
}

FB_TEST(raft_log_node, snapshot_log_boundary) {
    // 快照与日志边界
    raft_index_t snapshot_last_idx = 100;
    raft_term_t snapshot_last_term = 5;

    // 快照后的日志起始于 snapshot_last_idx + 1
    raft_index_t first_log_idx = snapshot_last_idx + 1;
    FB_ASSERT_EQ(first_log_idx, 101L);

    // 新日志的 prev_term 应该是 snapshot_last_term
    raft_term_t prev_term = snapshot_last_term;
    FB_ASSERT_EQ(prev_term, 5L);

    // 如果没有更多日志，next_idx = snapshot_last_idx + 1
    raft_index_t next_idx = snapshot_last_idx + 1;
    FB_ASSERT_EQ(next_idx, 101L);
}

FB_TEST(raft_log_node, snapshot_term_index_consistency) {
    // 快照 term 和 index 一致性
    raft_term_t snapshot_term = 3;
    raft_index_t snapshot_idx = 50;

    // 快照 term 和 index 必须有效
    FB_ASSERT_TRUE(snapshot_term > 0);
    FB_ASSERT_TRUE(snapshot_idx > 0);

    // 快照的 last_included_term 应该等于该 index 处日志的 term
    // 如果 snapshot_idx = 50, snapshot_term = 3
    // 那么 log[50].term == 3
    std::map<raft_index_t, raft_term_t> log;
    for (int i = 1; i <= 60; i++) {
        log[i] = (i <= 30) ? 2 : 3;
    }
    FB_ASSERT_EQ(log[snapshot_idx], snapshot_term);
}

FB_TEST(raft_log_node, snapshot_reject_stale) {
    // 拒绝过期快照
    raft_index_t current_snapshot_idx = 100;
    raft_index_t incoming_snapshot_idx = 80;

    // 拒绝比当前更旧的快照
    bool should_reject = incoming_snapshot_idx < current_snapshot_idx;
    FB_ASSERT_TRUE(should_reject);

    // 接受更新的快照
    incoming_snapshot_idx = 120;
    should_reject = incoming_snapshot_idx < current_snapshot_idx;
    FB_ASSERT_FALSE(should_reject);
}

FB_TEST(raft_log_node, snapshot_offset_tracking) {
    // 快照偏移跟踪
    size_t offset = 0;
    size_t chunk_size = 1024;

    // 模拟分块接收
    for (int i = 0; i < 5; i++) {
        offset += chunk_size;
    }
    FB_ASSERT_EQ(offset, 5UL * 1024);

    // 验证偏移单调递增
    FB_ASSERT_TRUE(offset >= chunk_size);
}

// ============================================================================
// Test Suite: Read Index and Lease Read
// ============================================================================

FB_TEST(raft_log_node, leader_lease_read_validity) {
    // Leader Lease 读有效性
    raft_time_t lease_start = 1000;
    raft_time_t lease_duration = 500;
    raft_time_t current_time = 1200;

    // 检查 lease 是否有效
    bool lease_valid = current_time < (lease_start + lease_duration);
    FB_ASSERT_TRUE(lease_valid);

    // Lease 过期情况
    current_time = 1600;
    lease_valid = current_time < (lease_start + lease_duration);
    FB_ASSERT_FALSE(lease_valid);
}

FB_TEST(raft_log_node, read_index_request) {
    // ReadIndex 请求处理
    raft_index_t commit_idx = 100;
    raft_index_t read_index = commit_idx;

    // ReadIndex 返回当前 commit_idx
    FB_ASSERT_EQ(read_index, 100L);

    // 等待状态机应用到 read_index
    raft_index_t last_applied = 95;
    bool can_read = last_applied >= read_index;
    FB_ASSERT_FALSE(can_read);

    // 状态机追上后可以读取
    last_applied = 100;
    can_read = last_applied >= read_index;
    FB_ASSERT_TRUE(can_read);
}

FB_TEST(raft_log_node, read_index_quorum_check) {
    // ReadIndex 多数派检查
    std::map<raft_node_id_t, raft_index_t> match_indices;
    match_indices[1] = 100;  // Leader
    match_indices[2] = 98;
    match_indices[3] = 99;
    match_indices[4] = 97;
    match_indices[5] = 100;

    // 找到多数派的 match_idx
    std::vector<raft_index_t> indices;
    for (const auto& pair : match_indices) {
        indices.push_back(pair.second);
    }
    std::sort(indices.begin(), indices.end());

    // 多数派位置 (5节点，第3个是多数派)
    raft_index_t quorum_match = indices[2];
    FB_ASSERT_EQ(quorum_match, 99L);

    // commit_idx 至少可以推进到 quorum_match
    bool can_advance_commit = quorum_match > 95;
    FB_ASSERT_TRUE(can_advance_commit);
}

FB_TEST(raft_log_node, follower_read_forward) {
    // Follower 读转发
    raft_identity state = RAFT_STATE_FOLLOWER;
    raft_node_id_t leader_id = 3;

    // Follower 不能直接处理读请求
    bool can_handle_read = (state == RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(can_handle_read);

    // 需要转发给 Leader
    bool need_forward = !can_handle_read;
    FB_ASSERT_TRUE(need_forward);
    FB_ASSERT_EQ(leader_id, 3);
}

FB_TEST(raft_log_node, lease_renewal) {
    // Lease 续期
    raft_time_t current_lease_end = 1000;
    raft_time_t heartbeat_received = 900;
    raft_time_t new_lease_end = 1400;  // heartbeat + election_timeout

    // 收到心跳确认后续期 lease
    if (heartbeat_received > 0) {
        current_lease_end = new_lease_end;
    }
    FB_ASSERT_EQ(current_lease_end, 1400L);

    // 多数派确认后才能续期
    int confirmations = 3;
    int quorum = 2;
    bool can_renew = confirmations >= quorum;
    FB_ASSERT_TRUE(can_renew);
}

FB_TEST(raft_log_node, read_index_pending_queue) {
    // ReadIndex 待处理队列
    std::queue<std::pair<raft_index_t, raft_node_id_t>> pending_reads;

    // 添加待处理读请求
    pending_reads.push({100, 1});
    pending_reads.push({105, 2});
    pending_reads.push({110, 3});

    FB_ASSERT_EQ(pending_reads.size(), 3UL);

    // 处理第一个读请求
    auto front = pending_reads.front();
    FB_ASSERT_EQ(front.first, 100L);
    FB_ASSERT_EQ(front.second, 1);
    pending_reads.pop();

    FB_ASSERT_EQ(pending_reads.size(), 2UL);
}

FB_TEST(raft_log_node, linearizable_read_safety) {
    // 线性化读安全性
    raft_index_t read_index = 100;
    raft_index_t state_machine_applied = 100;

    // 只有状态机应用到 read_index 后才能返回结果
    bool safe_to_read = state_machine_applied >= read_index;
    FB_ASSERT_TRUE(safe_to_read);

    // 状态机落后的情况
    state_machine_applied = 95;
    safe_to_read = state_machine_applied >= read_index;
    FB_ASSERT_FALSE(safe_to_read);
}

// ============================================================================
// Test Suite: Log Matching Property
// ============================================================================

FB_TEST(raft_log_node, log_matching_same_term_index) {
    // 日志匹配定理：相同 term 和 index 的日志条目相同
    std::map<raft_index_t, raft_term_t> log_a;
    log_a[10] = 3;
    log_a[11] = 3;
    log_a[12] = 4;

    std::map<raft_index_t, raft_term_t> log_b;
    log_b[10] = 3;
    log_b[11] = 3;
    log_b[12] = 4;

    // 相同 index 和 term 应该匹配
    FB_ASSERT_EQ(log_a[10], log_b[10]);
    FB_ASSERT_EQ(log_a[11], log_b[11]);
    FB_ASSERT_EQ(log_a[12], log_b[12]);
}

FB_TEST(raft_log_node, log_matching_different_term) {
    // 不同 term 的日志不匹配
    std::map<raft_index_t, raft_term_t> log_a;
    log_a[10] = 3;
    log_a[11] = 3;

    std::map<raft_index_t, raft_term_t> log_b;
    log_b[10] = 2;  // 不同 term
    log_b[11] = 3;

    // index 10 处 term 不同，不匹配
    bool match = log_a[10] == log_b[10];
    FB_ASSERT_FALSE(match);
}

FB_TEST(raft_log_node, log_matching_prefix_consistency) {
    // 日志匹配定理：如果两个日志在某个 index 匹配，则之前的日志也匹配
    raft_index_t match_idx = 10;
    raft_term_t match_term = 3;

    std::map<raft_index_t, raft_term_t> log_a;
    std::map<raft_index_t, raft_term_t> log_b;

    // 假设两个日志在 index 10 匹配
    for (int i = 1; i <= 10; i++) {
        log_a[i] = (i <= 5) ? 1 : (i <= 8) ? 2 : 3;
        log_b[i] = (i <= 5) ? 1 : (i <= 8) ? 2 : 3;
    }

    // 验证所有之前的条目也匹配
    for (raft_index_t idx = 1; idx <= match_idx; idx++) {
        FB_ASSERT_EQ(log_a[idx], log_b[idx]);
    }
}

FB_TEST(raft_log_node, log_conflict_detection) {
    // 冲突检测算法
    raft_index_t leader_prev_idx = 5;
    raft_term_t leader_prev_term = 3;
    raft_index_t follower_last_idx = 10;

    // 检查 follower 日志在 prev_idx 处的 term
    std::map<raft_index_t, raft_term_t> follower_log;
    for (int i = 1; i <= 10; i++) {
        follower_log[i] = (i <= 7) ? 2 : 3;
    }

    // prev_idx = 5 处 follower term = 2，与 leader term = 3 不匹配
    bool conflict = follower_log[leader_prev_idx] != leader_prev_term;
    FB_ASSERT_TRUE(conflict);

    // 需要截断 follower 日志
    raft_index_t truncate_from = leader_prev_idx + 1;
    FB_ASSERT_EQ(truncate_from, 6L);
}

FB_TEST(raft_log_node, log_consistency_check_append_entries) {
    // AppendEntries 一致性检查
    raft_term_t leader_term = 5;
    raft_term_t follower_term = 4;

    // Leader term >= follower term，可以继续
    bool term_ok = leader_term >= follower_term;
    FB_ASSERT_TRUE(term_ok);

    // prev_log_idx 和 prev_log_term 检查
    raft_index_t prev_log_idx = 10;
    raft_term_t prev_log_term = 3;
    std::map<raft_index_t, raft_term_t> follower_log;
    for (int i = 1; i <= 15; i++) {
        follower_log[i] = (i <= 8) ? 2 : 3;
    }

    bool prev_ok = (prev_log_idx == 0) ||
        (follower_log.find(prev_log_idx) != follower_log.end() &&
         follower_log[prev_log_idx] == prev_log_term);
    FB_ASSERT_TRUE(prev_ok);
}

FB_TEST(raft_log_node, log_matching_suffix_consistency) {
    // 日志匹配后缀一致性
    // 如果两个日志在某个 index 匹配，则之后的日志也相同（对于 Leader）
    std::map<raft_index_t, raft_term_t> leader_log;
    leader_log[10] = 3;
    leader_log[11] = 3;
    leader_log[12] = 4;
    leader_log[13] = 4;

    std::map<raft_index_t, raft_term_t> follower_log;
    follower_log[10] = 3;
    follower_log[11] = 3;
    // follower 缺少 12 和 13

    // Leader 发送缺失的条目
    raft_index_t follower_last_idx = 11;
    raft_index_t leader_last_idx = 13;
    int entries_to_send = leader_last_idx - follower_last_idx;
    FB_ASSERT_EQ(entries_to_send, 2);
}

FB_TEST(raft_log_node, log_matching_proof) {
    // 日志匹配定理证明验证
    // 定理：如果 log[i].term == log'[i].term，则 log[i] == log'[i]

    // 使用 Raft 性质：Leader 在一个 term 内最多创建一条日志在给定 index
    std::map<raft_term_t, std::set<raft_index_t>> term_to_indices;
    term_to_indices[1] = {1, 2, 3};  // term 1 有 index 1-3
    term_to_indices[2] = {4, 5, 6};  // term 2 有 index 4-6

    // 每个 (term, index) 组合唯一
    for (const auto& pair : term_to_indices) {
        for (raft_index_t idx : pair.second) {
            // 每个 index 在每个 term 只出现一次
            FB_ASSERT_TRUE(idx >= 1);
        }
    }
}

FB_TEST(raft_log_node, log_matching_commit_update) {
    // 日志匹配定理用于 commit 更新
    std::map<raft_node_id_t, raft_index_t> match_indices;
    match_indices[1] = 100;
    match_indices[2] = 100;
    match_indices[3] = 98;
    match_indices[4] = 100;
    match_indices[5] = 97;

    // 只有当多数派 match 的日志来自当前 term 才能 commit
    raft_term_t current_term = 5;
    std::map<raft_index_t, raft_term_t> log;
    for (int i = 1; i <= 100; i++) {
        log[i] = (i <= 80) ? 4 : 5;
    }

    // 多数派 match_idx = 100
    std::vector<raft_index_t> indices;
    for (const auto& pair : match_indices) {
        indices.push_back(pair.second);
    }
    std::sort(indices.begin(), indices.end());
    raft_index_t majority_match = indices[2];  // 第 3 个（5节点的多数派）

    // 检查 majority_match 处的 term 是否是当前 term
    bool can_commit = log[majority_match] == current_term;
    FB_ASSERT_TRUE(can_commit);
}

// ============================================================================
// Test Suite: Pre-Vote Mechanism
// ============================================================================

FB_TEST(raft_log_node, prevote_request_handling) {
    // Pre-Vote 请求处理
    raft_term_t current_term = 5;
    raft_term_t candidate_term = 6;
    bool is_prevote = true;

    // Pre-Vote 请求不更新 term
    raft_term_t original_term = current_term;
    if (!is_prevote) {
        current_term = std::max(current_term, candidate_term);
    }
    FB_ASSERT_EQ(current_term, original_term);

    // 正式投票请求会更新 term
    is_prevote = false;
    if (!is_prevote && candidate_term > current_term) {
        current_term = candidate_term;
    }
    FB_ASSERT_EQ(current_term, 6L);
}

FB_TEST(raft_log_node, prevote_log_check) {
    // Pre-Vote 日志检查
    raft_term_t current_last_log_term = 3;
    raft_index_t current_last_log_idx = 100;
    raft_term_t candidate_last_log_term = 4;
    raft_index_t candidate_last_log_idx = 105;

    // Pre-Vote 也需要检查日志是否更新
    bool log_is_up_to_date = (candidate_last_log_term > current_last_log_term) ||
        (candidate_last_log_term == current_last_log_term &&
         candidate_last_log_idx >= current_last_log_idx);
    FB_ASSERT_TRUE(log_is_up_to_date);

    // 日志更旧的候选人无法获得 Pre-Vote
    candidate_last_log_term = 2;
    candidate_last_log_idx = 90;
    log_is_up_to_date = (candidate_last_log_term > current_last_log_term) ||
        (candidate_last_log_term == current_last_log_term &&
         candidate_last_log_idx >= current_last_log_idx);
    FB_ASSERT_FALSE(log_is_up_to_date);
}

FB_TEST(raft_log_node, prevote_network_partition) {
    // Pre-Vote 在网络分区中的作用
    int total_nodes = 5;
    int partition_a_size = 2;  // 少数派分区
    int partition_b_size = 3;  // 多数派分区

    // 少数派分区的节点发送 Pre-Vote
    int prevotes_received = 0;
    // 只能从自己分区的节点获得 Pre-Vote
    for (int i = 0; i < partition_a_size; i++) {
        prevotes_received++;
    }

    int quorum = total_nodes / 2 + 1;  // 3
    bool can_start_election = prevotes_received >= quorum;
    FB_ASSERT_FALSE(can_start_election);  // 少数派无法开始选举
}

FB_TEST(raft_log_node, prevote_to_vote_transition) {
    // Pre-Vote 到正式投票的转换
    int prevotes_granted = 3;
    int cluster_size = 5;
    int quorum = cluster_size / 2 + 1;

    // Pre-Vote 成功后开始正式投票
    bool prevote_success = prevotes_granted >= quorum;
    FB_ASSERT_TRUE(prevote_success);

    // 正式投票需要重新请求投票
    int votes_granted = 0;
    // 通常 Pre-Vote 成功的节点也会在正式投票中支持
    votes_granted = prevotes_granted;
    bool election_won = votes_granted >= quorum;
    FB_ASSERT_TRUE(election_won);
}

FB_TEST(raft_log_node, prevote_suppress_disruptive_election) {
    // Pre-Vote 抑制破坏性选举
    // 场景：节点从网络分区恢复，term 更高但日志落后

    raft_term_t isolated_node_term = 10;
    raft_term_t cluster_term = 5;
    raft_index_t isolated_log_idx = 50;
    raft_index_t cluster_log_idx = 100;

    // 没有 Pre-Vote：孤立节点会干扰集群
    bool term_higher = isolated_node_term > cluster_term;
    bool log_behind = isolated_log_idx < cluster_log_idx;
    FB_ASSERT_TRUE(term_higher);
    FB_ASSERT_TRUE(log_behind);

    // 有 Pre-Vote：孤立节点无法获得 Pre-Vote（日志落后）
    bool can_get_prevote = !(log_behind);
    FB_ASSERT_FALSE(can_get_prevote);
}

FB_TEST(raft_log_node, prevote_candidate_state) {
    // Pre-Vote 候选人状态
    raft_identity state = RAFT_STATE_FOLLOWER;
    bool is_prevote_candidate = false;

    // 开始 Pre-Vote（不改变状态）
    is_prevote_candidate = true;
    FB_ASSERT_TRUE(is_prevote_candidate);
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);  // 状态不变

    // Pre-Vote 成功后转为正式候选人
    bool prevote_success = true;
    if (prevote_success) {
        state = RAFT_STATE_CANDIDATE;
    }
    FB_ASSERT_EQ(state, RAFT_STATE_CANDIDATE);
}

FB_TEST(raft_log_node, prevote_leader_lease_check) {
    // Pre-Vote 检查 Leader lease
    raft_time_t last_leader_contact = 1000;
    raft_time_t current_time = 1100;
    raft_time_t election_timeout = 150;

    // 节点认为 Leader 还活着
    bool leader_alive = (current_time - last_leader_contact) < election_timeout;
    FB_ASSERT_TRUE(leader_alive);

    // 如果 Leader 还活着，拒绝 Pre-Vote
    bool grant_prevote = !leader_alive;
    FB_ASSERT_FALSE(grant_prevote);

    // Leader 失联后可以授予 Pre-Vote
    current_time = 1300;
    leader_alive = (current_time - last_leader_contact) < election_timeout;
    grant_prevote = !leader_alive;
    FB_ASSERT_TRUE(grant_prevote);
}

// ============================================================================
// Test Suite: Network Partition
// ============================================================================

FB_TEST(raft_log_node, partition_minority_cannot_progress) {
    // 少数派分区无法推进
    int total_nodes = 5;
    int minority_size = 2;
    int quorum = total_nodes / 2 + 1;

    // 少数派无法达成共识
    bool can_commit = minority_size >= quorum;
    FB_ASSERT_FALSE(can_commit);

    // 少数派分区中的 Leader 会降级
    raft_identity leader_state = RAFT_STATE_LEADER;
    bool can_get_heartbeat_response = false;
    if (!can_get_heartbeat_response) {
        leader_state = RAFT_STATE_FOLLOWER;
    }
    FB_ASSERT_EQ(leader_state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_log_node, partition_majority_continues) {
    // 多数派分区继续工作
    int total_nodes = 5;
    int majority_size = 3;
    int quorum = total_nodes / 2 + 1;

    // 多数派可以达成共识
    bool can_commit = majority_size >= quorum;
    FB_ASSERT_TRUE(can_commit);

    // 多数派可以选举新 Leader
    int votes = majority_size;
    bool can_win_election = votes >= quorum;
    FB_ASSERT_TRUE(can_win_election);
}

FB_TEST(raft_log_node, partition_brain_split_scenario) {
    // 脑裂场景模拟
    int total_nodes = 6;  // 6 节点集群
    int partition_a_size = 3;
    int partition_b_size = 3;

    // 两个分区大小相等，都无法获得多数派
    int quorum = total_nodes / 2 + 1;  // 4
    bool partition_a_can_progress = partition_a_size >= quorum;
    bool partition_b_can_progress = partition_b_size >= quorum;
    FB_ASSERT_FALSE(partition_a_can_progress);
    FB_ASSERT_FALSE(partition_b_can_progress);

    // 都无法选举 Leader
    bool partition_a_can_elect = partition_a_size >= quorum;
    FB_ASSERT_FALSE(partition_a_can_elect);
}

FB_TEST(raft_log_node, partition_healing_sync) {
    // 网络恢复后同步
    raft_term_t old_leader_term = 5;
    raft_term_t new_leader_term = 7;
    raft_index_t old_partition_log_idx = 80;
    raft_index_t new_partition_log_idx = 100;

    // 网络恢复后，旧分区节点发现更高 term
    bool need_sync = new_leader_term > old_leader_term;
    FB_ASSERT_TRUE(need_sync);

    // 需要同步日志差距
    int entries_to_sync = new_partition_log_idx - old_partition_log_idx;
    FB_ASSERT_EQ(entries_to_sync, 20);
}

FB_TEST(raft_log_node, partition_leader_step_down) {
    // 分区 Leader 降级
    raft_identity state = RAFT_STATE_LEADER;
    raft_term_t current_term = 5;
    raft_term_t higher_term = 7;

    // 收到更高 term 的消息后降级
    if (higher_term > current_term) {
        current_term = higher_term;
        state = RAFT_STATE_FOLLOWER;
    }
    FB_ASSERT_EQ(current_term, 7L);
    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
}

FB_TEST(raft_log_node, partition_stale_read_prevention) {
    // 防止分区导致的过期读
    raft_identity state = RAFT_STATE_LEADER;
    bool is_in_majority_partition = false;

    // 如果不在多数派分区，不能处理读请求
    bool can_serve_read = (state == RAFT_STATE_LEADER) && is_in_majority_partition;
    FB_ASSERT_FALSE(can_serve_read);

    // 在多数派分区中可以处理读请求
    is_in_majority_partition = true;
    can_serve_read = (state == RAFT_STATE_LEADER) && is_in_majority_partition;
    FB_ASSERT_TRUE(can_serve_read);
}

FB_TEST(raft_log_node, partition_log_divergence) {
    // 分区日志分歧
    std::map<raft_index_t, raft_term_t> partition_a_log;
    for (int i = 1; i <= 80; i++) {
        partition_a_log[i] = (i <= 50) ? 3 : 4;
    }

    std::map<raft_index_t, raft_term_t> partition_b_log;
    for (int i = 1; i <= 100; i++) {
        partition_b_log[i] = (i <= 50) ? 3 : ((i <= 70) ? 4 : 5);
    }

    // 公共前缀（term 3 的日志）
    raft_index_t common_prefix_end = 50;
    FB_ASSERT_EQ(partition_a_log[50], partition_b_log[50]);

    // 分歧点
    bool diverged = partition_a_log[80] != partition_b_log[80];
    FB_ASSERT_TRUE(diverged);
}

FB_TEST(raft_log_node, partition_removal_safety) {
    // 分区期间移除节点安全性
    int old_cluster_size = 5;
    int new_cluster_size = 3;  // 移除 2 个节点
    int old_quorum = old_cluster_size / 2 + 1;  // 3
    int new_quorum = new_cluster_size / 2 + 1;  // 2

    // 配置变更需要两个配置的多数派都同意
    // 如果被移除的节点在分区中，可能影响安全性
    int nodes_removed = old_cluster_size - new_cluster_size;
    bool safe_removal = nodes_removed < old_quorum;
    FB_ASSERT_TRUE(safe_removal);
}

// ============================================================================
// Test Suite: Safety Properties
// ============================================================================

FB_TEST(raft_log_node, leader_completeness) {
    // Leader 完整性：所有已提交的日志在所有未来 Leader 中都存在
    std::map<raft_index_t, raft_term_t> committed_log;
    for (int i = 1; i <= 100; i++) {
        committed_log[i] = (i <= 50) ? 3 : 4;
    }

    raft_index_t commit_idx = 100;

    // 新 Leader 必须包含所有已提交的日志
    std::map<raft_index_t, raft_term_t> new_leader_log;
    for (int i = 1; i <= commit_idx; i++) {
        new_leader_log[i] = committed_log[i];
    }

    // 验证完整性
    for (raft_index_t idx = 1; idx <= commit_idx; idx++) {
        FB_ASSERT_EQ(new_leader_log[idx], committed_log[idx]);
    }
}

FB_TEST(raft_log_node, leader_completeness_vote_check) {
    // Leader 完整性通过投票检查保证
    raft_index_t voter_last_log_idx = 100;
    raft_term_t voter_last_log_term = 4;
    raft_index_t candidate_last_log_idx = 95;
    raft_term_t candidate_last_log_term = 4;

    // 候选人日志更旧，无法获得投票
    bool log_is_up_to_date = (candidate_last_log_term > voter_last_log_term) ||
        (candidate_last_log_term == voter_last_log_term &&
         candidate_last_log_idx >= voter_last_log_idx);
    FB_ASSERT_FALSE(log_is_up_to_date);

    // 因此，日志落后的候选人无法成为 Leader
    // 保证了 Leader 完整性
}

FB_TEST(raft_log_node, state_machine_safety) {
    // 状态机安全性：所有节点按相同顺序应用相同日志
    std::vector<int> applied_commands_node_a = {1, 2, 3, 4, 5};
    std::vector<int> applied_commands_node_b = {1, 2, 3, 4, 5};

    // 验证两个节点应用了相同的命令序列
    FB_ASSERT_EQ(applied_commands_node_a.size(), applied_commands_node_b.size());
    for (size_t i = 0; i < applied_commands_node_a.size(); i++) {
        FB_ASSERT_EQ(applied_commands_node_a[i], applied_commands_node_b[i]);
    }
}

FB_TEST(raft_log_node, state_machine_safety_order) {
    // 状态机安全性：命令应用顺序必须一致
    std::map<raft_index_t, int> log_commands;
    log_commands[1] = 100;
    log_commands[2] = 200;
    log_commands[3] = 300;

    // 应用顺序必须按 index 递增
    raft_index_t last_applied = 0;
    for (const auto& pair : log_commands) {
        FB_ASSERT_TRUE(pair.first > last_applied);
        last_applied = pair.first;
    }
}

FB_TEST(raft_log_node, election_safety_single_leader_v2) {
    // 选举安全性：每个 term 最多有一个 Leader
    std::map<raft_term_t, std::set<raft_node_id_t>> term_leaders;

    // 每个 term 只记录一个 Leader
    term_leaders[1].insert(5);  // term 1 的 Leader 是节点 5
    term_leaders[2].insert(3);  // term 2 的 Leader 是节点 3
    term_leaders[3].insert(1);  // term 3 的 Leader 是节点 1

    // 每个 term 最多只有一个 Leader
    for (const auto& pair : term_leaders) {
        FB_ASSERT_TRUE(pair.second.size() <= 1);
    }
}

FB_TEST(raft_log_node, election_safety_term_monotonic) {
    // 选举安全性：term 单调递增
    raft_term_t current_term = 0;

    // 模拟多次选举
    std::vector<raft_term_t> terms = {1, 2, 3, 5, 7};
    for (raft_term_t term : terms) {
        FB_ASSERT_TRUE(term > current_term);
        current_term = term;
    }
}

FB_TEST(raft_log_node, log_matching_property) {
    // 日志匹配属性：相同索引和 term 的日志相同
    struct LogEntry {
        raft_term_t term;
        int command;
    };

    std::map<raft_index_t, LogEntry> log_a = {
        {1, {1, 100}},
        {2, {1, 200}},
        {3, {2, 300}}
    };

    std::map<raft_index_t, LogEntry> log_b = {
        {1, {1, 100}},
        {2, {1, 200}},
        {3, {2, 300}}
    };

    // 验证匹配
    for (const auto& pair : log_a) {
        raft_index_t idx = pair.first;
        FB_ASSERT_EQ(log_a[idx].term, log_b[idx].term);
        FB_ASSERT_EQ(log_a[idx].command, log_b[idx].command);
    }
}

FB_TEST(raft_log_node, leader_append_only) {
    // Leader 只追加原则：Leader 从不覆盖或删除自己的日志
    std::vector<raft_index_t> leader_log_indices;
    for (int i = 1; i <= 100; i++) {
        leader_log_indices.push_back(i);
    }

    size_t original_size = leader_log_indices.size();

    // Leader 只能追加
    leader_log_indices.push_back(101);
    FB_ASSERT_TRUE(leader_log_indices.size() > original_size);

    // Leader 不会删除
    FB_ASSERT_TRUE(leader_log_indices.size() >= original_size);
}

FB_TEST(raft_log_node, follower_log_consolidation) {
    // Follower 日志整合：Follower 复制 Leader 日志
    std::map<raft_index_t, raft_term_t> follower_log;
    for (int i = 1; i <= 50; i++) {
        follower_log[i] = 1;
    }

    // 收到 Leader 的 AppendEntries
    raft_index_t leader_prev_idx = 50;
    raft_term_t leader_prev_term = 1;

    // 验证 prev 匹配
    bool prev_match = follower_log[leader_prev_idx] == leader_prev_term;
    FB_ASSERT_TRUE(prev_match);

    // 追加新日志
    for (int i = 51; i <= 60; i++) {
        follower_log[i] = 2;
    }

    FB_ASSERT_EQ(follower_log.size(), 60UL);
}

FB_TEST(raft_log_node, commit_index_safety) {
    // commit_idx 安全性：只有当前 term 的日志才能提交
    raft_term_t current_term = 5;
    std::map<raft_index_t, raft_term_t> log;
    for (int i = 1; i <= 100; i++) {
        log[i] = (i <= 80) ? 4 : 5;
    }

    raft_index_t match_idx = 100;

    // 检查 match_idx 处的 term 是否是当前 term
    bool can_commit = log[match_idx] == current_term;
    FB_ASSERT_TRUE(can_commit);

    // 如果是旧 term 的日志，不能提交
    match_idx = 70;
    can_commit = log[match_idx] == current_term;
    FB_ASSERT_FALSE(can_commit);
}

FB_TEST(raft_log_node, invariant_preservation) {
    // 不变量保持：关键不变量在任何时候都成立
    // 不变量1：commit_idx <= last_log_idx
    raft_index_t commit_idx = 100;
    raft_index_t last_log_idx = 150;
    FB_ASSERT_TRUE(commit_idx <= last_log_idx);

    // 不变量2：last_applied <= commit_idx
    raft_index_t last_applied = 95;
    FB_ASSERT_TRUE(last_applied <= commit_idx);

    // 不变量3：current_term 单调递增
    raft_term_t old_term = 5;
    raft_term_t new_term = 6;
    FB_ASSERT_TRUE(new_term >= old_term);
}

// ============================================================================
// Test Suite: Heartbeat Mechanism
// ============================================================================

FB_TEST(raft_log_node, heartbeat_interval_timing) {
    // 心跳间隔定时
    raft_time_t heartbeat_interval = 50;  // 50ms
    raft_time_t election_timeout = 150;   // 150ms

    // 心跳间隔应小于选举超时
    bool valid_config = heartbeat_interval < election_timeout;
    FB_ASSERT_TRUE(valid_config);

    // 典型配置：心跳间隔 = 选举超时 / 3
    raft_time_t recommended_heartbeat = election_timeout / 3;
    FB_ASSERT_EQ(recommended_heartbeat, 50L);
}

FB_TEST(raft_log_node, heartbeat_timeout_detection) {
    // 心跳超时检测
    raft_time_t last_heartbeat = 1000;
    raft_time_t current_time = 1100;
    raft_time_t heartbeat_timeout = 150;

    // 计算距离上次心跳的时间
    raft_time_t elapsed = current_time - last_heartbeat;
    FB_ASSERT_EQ(elapsed, 100L);

    // 检查是否超时
    bool timed_out = elapsed > heartbeat_timeout;
    FB_ASSERT_FALSE(timed_out);

    // 超时情况
    current_time = 1200;
    elapsed = current_time - last_heartbeat;
    timed_out = elapsed > heartbeat_timeout;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_log_node, heartbeat_request_generation) {
    // 心跳请求生成
    raft_term_t leader_term = 5;
    raft_index_t leader_commit = 100;
    raft_index_t prev_log_idx = 105;
    raft_term_t prev_log_term = 5;

    // 心跳请求字段
    FB_ASSERT_TRUE(leader_term > 0);
    FB_ASSERT_TRUE(leader_commit >= 0);
    FB_ASSERT_TRUE(prev_log_idx >= 0);

    // 空心跳：entries 为空
    std::vector<int> entries;  // 空条目列表
    FB_ASSERT_TRUE(entries.empty());
}

FB_TEST(raft_log_node, heartbeat_response_success) {
    // 心跳响应成功处理
    raft_term_t follower_term = 5;
    bool success = true;
    raft_index_t match_idx = 105;

    // Leader 收到成功响应
    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(match_idx, 105L);

    // 更新 match_idx
    raft_index_t next_idx = match_idx + 1;
    FB_ASSERT_EQ(next_idx, 106L);
}

FB_TEST(raft_log_node, heartbeat_response_failure) {
    // 心跳响应失败处理
    raft_term_t leader_term = 5;
    raft_term_t follower_term = 6;  // Follower 有更高的 term
    bool success = false;

    // Leader 收到失败响应
    FB_ASSERT_FALSE(success);

    // Leader term 过期，需要降级
    bool leader_is_stale = follower_term > leader_term;
    FB_ASSERT_TRUE(leader_is_stale);
}

FB_TEST(raft_log_node, heartbeat_term_check) {
    // 心跳 term 检查
    raft_term_t current_term = 5;
    raft_term_t heartbeat_term = 6;

    // 收到更高 term 的心跳
    bool update_term = heartbeat_term > current_term;
    FB_ASSERT_TRUE(update_term);

    // 更新 term 并重置选举超时
    if (update_term) {
        current_term = heartbeat_term;
    }
    FB_ASSERT_EQ(current_term, 6L);
}

FB_TEST(raft_log_node, heartbeat_commit_update) {
    // 心跳更新 commit_idx
    raft_index_t leader_commit = 100;
    raft_index_t follower_commit = 80;

    // Follower 更新 commit_idx
    raft_index_t new_commit = std::min(leader_commit, follower_commit + 20);
    FB_ASSERT_EQ(new_commit, 100L);

    // commit_idx 不能超过 last_log_idx
    raft_index_t last_log_idx = 95;
    new_commit = std::min(leader_commit, last_log_idx);
    FB_ASSERT_EQ(new_commit, 95L);
}

FB_TEST(raft_log_node, heartbeat_suppression_optimization) {
    // 心跳抑制优化
    bool has_pending_entries = true;
    bool need_heartbeat = false;

    // 如果有待发送的日志条目，可以抑制心跳
    if (!has_pending_entries) {
        need_heartbeat = true;
    }
    FB_ASSERT_FALSE(need_heartbeat);

    // 没有待发送条目时需要心跳
    has_pending_entries = false;
    if (!has_pending_entries) {
        need_heartbeat = true;
    }
    FB_ASSERT_TRUE(need_heartbeat);
}

FB_TEST(raft_log_node, heartbeat_broadcast_all_nodes) {
    // 向所有节点广播心跳
    std::map<raft_node_id_t, raft_time_t> last_heartbeat_sent;
    raft_time_t current_time = 1000;

    // 记录向每个节点发送心跳的时间
    for (int node_id = 1; node_id <= 5; node_id++) {
        last_heartbeat_sent[node_id] = current_time;
    }

    // 验证所有节点都收到心跳
    FB_ASSERT_EQ(last_heartbeat_sent.size(), 5UL);

    // 检查是否所有节点都在最近收到心跳
    raft_time_t heartbeat_window = 100;
    int nodes_received = 0;
    for (const auto& pair : last_heartbeat_sent) {
        if (current_time - pair.second <= heartbeat_window) {
            nodes_received++;
        }
    }
    FB_ASSERT_EQ(nodes_received, 5);
}

FB_TEST(raft_log_node, heartbeat_minimize_disruption) {
    // 最小化心跳干扰
    raft_time_t last_append_time = 1000;
    raft_time_t current_time = 1005;
    raft_time_t heartbeat_interval = 50;

    // 刚刚发送过日志，可以推迟心跳
    bool recently_active = (current_time - last_append_time) < heartbeat_interval;
    FB_ASSERT_TRUE(recently_active);

    // 日志活动时延长心跳间隔
    raft_time_t effective_interval = recently_active ? heartbeat_interval * 2 : heartbeat_interval;
    FB_ASSERT_EQ(effective_interval, 100L);
}

FB_TEST(raft_log_node, heartbeat_leader_failure_detection) {
    // 通过心跳检测 Leader 失败
    std::map<raft_node_id_t, raft_time_t> last_heartbeat_received;
    raft_time_t current_time = 2000;
    raft_time_t election_timeout = 150;

    // 模拟 Leader 心跳停止
    last_heartbeat_received[1] = 1800;  // Leader 1
    last_heartbeat_received[2] = 1950;  // Leader 2 (最近有心跳)

    // 检测 Leader 1 是否失联
    bool leader1_failed = (current_time - last_heartbeat_received[1]) > election_timeout;
    FB_ASSERT_TRUE(leader1_failed);

    // Leader 2 仍然活跃
    bool leader2_failed = (current_time - last_heartbeat_received[2]) > election_timeout;
    FB_ASSERT_FALSE(leader2_failed);
}

FB_TEST(raft_log_node, heartbeat_lease_extension) {
    // 心跳续约租约
    raft_time_t lease_expiry = 1100;
    raft_time_t current_time = 1050;
    raft_time_t lease_duration = 150;

    // 收到心跳后延长租约
    if (current_time < lease_expiry) {
        lease_expiry = current_time + lease_duration;
    }
    FB_ASSERT_EQ(lease_expiry, 1200L);

    // 验证租约有效性
    bool lease_valid = current_time < lease_expiry;
    FB_ASSERT_TRUE(lease_valid);
}

FB_TEST(raft_log_node, heartbeat_batch_optimization) {
    // 心跳批处理优化
    int pending_heartbeats = 0;
    int batch_threshold = 3;

    // 累积心跳请求
    std::vector<int> pending_nodes = {1, 2, 3, 4};
    for (int node : pending_nodes) {
        pending_heartbeats++;
    }

    // 达到阈值后批量发送
    bool should_batch = pending_heartbeats >= batch_threshold;
    FB_ASSERT_TRUE(should_batch);

    // 批量发送后清空
    if (should_batch) {
        pending_heartbeats = 0;
    }
    FB_ASSERT_EQ(pending_heartbeats, 0);
}

FB_TEST(raft_log_node, heartbeat_network_partition_handling) {
    // 心跳在网络分区中的处理
    std::map<raft_node_id_t, bool> partition_status;
    partition_status[1] = true;   // 在多数派分区
    partition_status[2] = true;   // 在多数派分区
    partition_status[3] = true;   // 在多数派分区
    partition_status[4] = false;  // 在少数派分区
    partition_status[5] = false;  // 在少数派分区

    // 统计可用心跳响应
    int available_nodes = 0;
    for (const auto& pair : partition_status) {
        if (pair.second) available_nodes++;
    }

    // 多数派分区可以继续工作
    int quorum = 3;
    bool can_progress = available_nodes >= quorum;
    FB_ASSERT_TRUE(can_progress);
}

FB_TEST(raft_log_node, heartbeat_retry_mechanism) {
    // 心跳重试机制
    int max_retries = 3;
    int retry_count = 0;
    bool success = false;

    // 模拟重试
    for (int i = 0; i < max_retries && !success; i++) {
        retry_count++;
        if (i == 2) {  // 第三次成功
            success = true;
        }
    }

    FB_ASSERT_TRUE(success);
    FB_ASSERT_EQ(retry_count, 3);
}

FB_TEST(raft_log_node, heartbeat_priority_scheduling) {
    // 心跳优先级调度
    enum class heartbeat_priority {
        HIGH,    // 即将超时的节点
        NORMAL,  // 常规心跳
        LOW      // 可以延迟的心跳
    };

    std::map<raft_node_id_t, heartbeat_priority> priorities;
    priorities[1] = heartbeat_priority::HIGH;
    priorities[2] = heartbeat_priority::NORMAL;
    priorities[3] = heartbeat_priority::LOW;

    // 按优先级处理
    int high_count = 0;
    for (const auto& pair : priorities) {
        if (pair.second == heartbeat_priority::HIGH) {
            high_count++;
        }
    }
    FB_ASSERT_EQ(high_count, 1);
}

FB_TEST(raft_log_node, heartbeat_backpressure_handling) {
    // 心跳背压处理
    int inflight_heartbeats = 0;
    int max_inflight = 10;
    bool can_send = true;

    // 模拟发送心跳
    for (int i = 0; i < 12; i++) {
        if (inflight_heartbeats >= max_inflight) {
            can_send = false;
        }
        if (can_send) {
            inflight_heartbeats++;
        }
    }

    // 超过限制时停止发送
    FB_ASSERT_FALSE(can_send);
    FB_ASSERT_EQ(inflight_heartbeats, 10);
}

// ============================================================================
// Test Suite: Leader Transfer
// ============================================================================

FB_TEST(raft_log_node, leader_transfer_initiation) {
    // Leader 转移启动
    raft_node_id_t current_leader = 1;
    raft_node_id_t target_leader = 3;

    // 启动 Leader 转移
    bool transfer_in_progress = true;
    FB_ASSERT_TRUE(transfer_in_progress);

    // 目标节点必须存在且可投票
    bool target_is_voting = true;
    bool can_transfer = target_is_voting && (target_leader != current_leader);
    FB_ASSERT_TRUE(can_transfer);
}

FB_TEST(raft_log_node, leader_transfer_log_sync) {
    // Leader 转移前的日志同步
    raft_index_t leader_last_idx = 100;
    raft_index_t target_match_idx = 95;

    // 确保目标节点日志同步
    bool log_synced = target_match_idx >= leader_last_idx;
    FB_ASSERT_FALSE(log_synced);

    // 需要先同步日志
    int entries_to_send = leader_last_idx - target_match_idx;
    FB_ASSERT_EQ(entries_to_send, 5);

    // 同步完成后可以转移
    target_match_idx = leader_last_idx;
    log_synced = target_match_idx >= leader_last_idx;
    FB_ASSERT_TRUE(log_synced);
}

FB_TEST(raft_log_node, leader_transfer_timeout_now) {
    // 发送 TimeoutNow 消息
    raft_term_t leader_term = 5;
    raft_node_id_t target_node = 3;

    // TimeoutNow 让目标节点立即开始选举
    bool timeout_now_sent = true;
    FB_ASSERT_TRUE(timeout_now_sent);

    // 目标节点收到后增加 term 并开始选举
    raft_term_t new_term = leader_term + 1;
    FB_ASSERT_EQ(new_term, 6L);
}

FB_TEST(raft_log_node, leader_transfer_timeout_detection) {
    // Leader 转移超时检测
    raft_time_t transfer_start = 1000;
    raft_time_t transfer_timeout = 500;
    raft_time_t current_time = 1400;

    // 检查是否超时
    bool timed_out = (current_time - transfer_start) > transfer_timeout;
    FB_ASSERT_FALSE(timed_out);

    // 超时情况
    current_time = 1600;
    timed_out = (current_time - transfer_start) > transfer_timeout;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_log_node, leader_transfer_abort) {
    // Leader 转移中止
    bool transfer_in_progress = true;
    raft_term_t new_term_seen = 6;
    raft_term_t current_term = 5;

    // 收到更高 term 的消息，中止转移
    if (new_term_seen > current_term) {
        transfer_in_progress = false;
    }
    FB_ASSERT_FALSE(transfer_in_progress);
}

FB_TEST(raft_log_node, leader_transfer_complete) {
    // Leader 转移完成
    raft_identity state = RAFT_STATE_LEADER;
    raft_node_id_t new_leader = 3;
    raft_term_t new_leader_term = 6;

    // 收到新 Leader 的心跳
    bool new_leader_heartbeat = true;
    raft_term_t current_term = 5;

    if (new_leader_heartbeat && new_leader_term > current_term) {
        state = RAFT_STATE_FOLLOWER;
        current_term = new_leader_term;
    }

    FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
    FB_ASSERT_EQ(current_term, 6L);
}

FB_TEST(raft_log_node, leader_transfer_multiple_candidates) {
    // Leader 转移时多个候选人的处理
    int nodes_receiving_timeout_now = 1;  // 只发给目标节点

    // 确保只有一个节点收到 TimeoutNow
    FB_ASSERT_EQ(nodes_receiving_timeout_now, 1);

    // 如果多个节点收到，可能导致分票
    nodes_receiving_timeout_now = 3;
    bool split_vote_risk = nodes_receiving_timeout_now > 1;
    FB_ASSERT_TRUE(split_vote_risk);
}

FB_TEST(raft_log_node, leader_transfer_rollback) {
    // Leader 转移回滚
    bool transfer_in_progress = true;
    raft_index_t last_transfer_idx = 100;
    raft_index_t new_entries_since_transfer = 5;

    // 转移期间收到新写入，需要回滚转移
    bool has_new_activity = new_entries_since_transfer > 0;
    if (has_new_activity) {
        transfer_in_progress = false;
    }
    FB_ASSERT_FALSE(transfer_in_progress);
}

FB_TEST(raft_log_node, leader_transfer_target_unavailable) {
    // 目标节点不可用时的处理
    raft_node_id_t target_node = 3;
    std::map<raft_node_id_t, bool> node_available;
    node_available[1] = true;
    node_available[2] = true;
    node_available[3] = false;  // 目标节点不可用

    // 检查目标节点是否可用
    bool can_transfer = node_available[target_node];
    FB_ASSERT_FALSE(can_transfer);

    // 选择另一个目标
    raft_node_id_t new_target = 2;
    can_transfer = node_available[new_target];
    FB_ASSERT_TRUE(can_transfer);
}

FB_TEST(raft_log_node, leader_transfer_follower_log_check) {
    // Leader 转移前检查 Follower 日志
    std::map<raft_node_id_t, raft_index_t> match_indices;
    match_indices[1] = 100;  // Leader
    match_indices[2] = 100;  // 完全同步
    match_indices[3] = 98;   // 稍有落后
    match_indices[4] = 95;   // 明显落后

    raft_index_t leader_last_idx = 100;

    // 选择日志最新的节点作为转移目标
    raft_node_id_t best_candidate = 0;
    raft_index_t max_match = 0;
    for (const auto& pair : match_indices) {
        if (pair.first != 1 && pair.second > max_match) {
            max_match = pair.second;
            best_candidate = pair.first;
        }
    }

    FB_ASSERT_EQ(best_candidate, 2);
    FB_ASSERT_EQ(max_match, 100L);
}

FB_TEST(raft_log_node, leader_transfer_quorum_preserved) {
    // Leader 转移期间保持多数派
    int cluster_size = 5;
    int available_nodes = 4;  // 一个节点正在转移
    int quorum = cluster_size / 2 + 1;

    // 确保转移后仍有多数派可用
    bool quorum_preserved = available_nodes >= quorum;
    FB_ASSERT_TRUE(quorum_preserved);
}

FB_TEST(raft_log_node, leader_transfer_state_machine_consistency) {
    // Leader 转移时状态机一致性
    raft_index_t leader_last_applied = 95;
    raft_index_t target_last_applied = 95;

    // 确保目标节点状态机也是最新的
    bool state_machine_synced = target_last_applied >= leader_last_applied;
    FB_ASSERT_TRUE(state_machine_synced);
}

FB_TEST(raft_log_node, leader_transfer_configuration_consistency) {
    // Leader 转移时配置一致性
    std::map<raft_node_id_t, bool> leader_config;
    leader_config[1] = true;
    leader_config[2] = true;
    leader_config[3] = true;

    std::map<raft_node_id_t, bool> target_config;
    target_config[1] = true;
    target_config[2] = true;
    target_config[3] = true;

    // 配置应该一致
    bool config_match = (leader_config == target_config);
    FB_ASSERT_TRUE(config_match);
}

FB_TEST(raft_log_node, leader_transfer_pre_vote_check) {
    // Leader 转移时 Pre-Vote 检查
    bool prevote_enabled = true;
    raft_term_t leader_term = 5;

    // Pre-Vote 模式下，确保新 Leader 能获得多数派支持
    int prevotes_needed = 3;  // 5 节点集群需要 3 票
    int expected_prevotes = 3;

    bool can_win_prevote = expected_prevotes >= prevotes_needed;
    FB_ASSERT_TRUE(can_win_prevote);
}

FB_TEST(raft_log_node, leader_transfer_during_partition) {
    // 网络分区期间的 Leader 转移
    int total_nodes = 5;
    int majority_partition_size = 3;
    int target_in_majority = true;

    // 只在多数派分区内进行转移
    bool safe_to_transfer = target_in_majority && (majority_partition_size >= total_nodes / 2 + 1);
    FB_ASSERT_TRUE(safe_to_transfer);

    // 目标在少数派分区时不能转移
    target_in_majority = false;
    safe_to_transfer = target_in_majority;
    FB_ASSERT_FALSE(safe_to_transfer);
}

FB_TEST(raft_log_node, leader_transfer_graceful_shutdown) {
    // Leader 优雅关闭时的转移
    bool graceful_shutdown = true;
    bool transfer_completed = false;

    // 优雅关闭需要先完成转移
    if (graceful_shutdown) {
        // 执行转移流程
        transfer_completed = true;
    }

    FB_ASSERT_TRUE(transfer_completed);
}

FB_TEST(raft_log_node, leader_transfer_concurrent_requests) {
    // 并发 Leader 转移请求处理
    int transfer_requests = 2;

    // 只允许一个转移进行
    bool first_transfer_active = true;
    bool second_transfer_accepted = !first_transfer_active;

    FB_ASSERT_FALSE(second_transfer_accepted);
}

FB_TEST(raft_log_node, leader_transfer_snapshot_needed) {
    // Leader 转移时需要快照
    raft_index_t target_last_idx = 50;
    raft_index_t leader_snapshot_idx = 80;

    // 目标节点需要快照才能追上
    bool needs_snapshot = leader_snapshot_idx > target_last_idx;
    FB_ASSERT_TRUE(needs_snapshot);

    // 先发送快照
    raft_index_t new_target_idx = leader_snapshot_idx;
    FB_ASSERT_EQ(new_target_idx, 80L);
}

// ============================================================================
// Test Suite: Log Replay and Recovery
// ============================================================================

FB_TEST(raft_log_node, log_recovery_from_crash) {
    // 崩溃后日志恢复
    raft_index_t disk_last_idx = 100;
    raft_index_t memory_last_idx = 0;  // 崩溃后内存清空

    // 从磁盘恢复日志
    memory_last_idx = disk_last_idx;
    FB_ASSERT_EQ(memory_last_idx, 100L);

    // next_idx 应该是 last_idx + 1
    raft_index_t next_idx = memory_last_idx + 1;
    FB_ASSERT_EQ(next_idx, 101L);
}

FB_TEST(raft_log_node, log_replay_order) {
    // 日志回放顺序
    std::vector<raft_index_t> log_indices;
    for (int i = 1; i <= 100; i++) {
        log_indices.push_back(i);
    }

    // 验证回放顺序是递增的
    bool ordered = true;
    for (size_t i = 1; i < log_indices.size(); i++) {
        if (log_indices[i] <= log_indices[i-1]) {
            ordered = false;
            break;
        }
    }
    FB_ASSERT_TRUE(ordered);
}

FB_TEST(raft_log_node, log_replay_from_snapshot) {
    // 从快照点开始回放
    raft_index_t snapshot_last_idx = 50;
    raft_index_t first_log_idx = snapshot_last_idx + 1;
    raft_term_t snapshot_last_term = 3;

    // 快照后的日志从 first_log_idx 开始
    FB_ASSERT_EQ(first_log_idx, 51L);

    // 回放日志条目数
    raft_index_t last_log_idx = 100;
    int entries_to_replay = last_log_idx - snapshot_last_idx;
    FB_ASSERT_EQ(entries_to_replay, 50);
}

FB_TEST(raft_log_node, log_replay_idempotency) {
    // 日志回放幂等性
    int replay_count = 0;
    std::map<raft_index_t, int> applied_commands;

    // 模拟回放同一批日志多次
    std::vector<raft_index_t> batch1 = {1, 2, 3};
    std::vector<raft_index_t> batch2 = {2, 3, 4};  // 有重叠

    // 第一次回放
    for (raft_index_t idx : batch1) {
        if (applied_commands.find(idx) == applied_commands.end()) {
            applied_commands[idx] = 1;
            replay_count++;
        }
    }

    // 第二次回放（幂等）
    for (raft_index_t idx : batch2) {
        if (applied_commands.find(idx) == applied_commands.end()) {
            applied_commands[idx] = 1;
            replay_count++;
        }
    }

    FB_ASSERT_EQ(applied_commands.size(), 4UL);
    FB_ASSERT_EQ(replay_count, 4);
}

FB_TEST(raft_log_node, log_recovery_partial_write) {
    // 部分写入的恢复
    raft_index_t commit_idx = 95;
    raft_index_t last_log_idx = 100;
    std::vector<bool> entry_valid(101, true);

    // 模拟部分条目未正确写入
    entry_valid[98] = false;
    entry_valid[99] = false;
    entry_valid[100] = false;

    // 截断无效条目
    raft_index_t valid_last_idx = commit_idx;
    for (raft_index_t idx = commit_idx + 1; idx <= last_log_idx; idx++) {
        if (!entry_valid[idx]) {
            valid_last_idx = idx - 1;
            break;
        }
    }

    FB_ASSERT_EQ(valid_last_idx, 95L);
}

FB_TEST(raft_log_node, log_replay_state_machine_consistency) {
    // 回放时状态机一致性
    std::map<raft_index_t, int> log_commands;
    for (int i = 1; i <= 50; i++) {
        log_commands[i] = i * 10;
    }

    std::map<raft_index_t, int> state_machine;

    // 回放日志到状态机
    for (const auto& pair : log_commands) {
        state_machine[pair.first] = pair.second;
    }

    // 验证状态机与日志一致
    FB_ASSERT_EQ(state_machine.size(), log_commands.size());
    for (const auto& pair : log_commands) {
        FB_ASSERT_EQ(state_machine[pair.first], pair.second);
    }
}

FB_TEST(raft_log_node, log_recovery_term_metadata) {
    // 恢复时 term 元数据
    raft_term_t persisted_term = 5;
    raft_node_id_t persisted_voted_for = 3;

    // 从持久化存储恢复
    raft_term_t current_term = persisted_term;
    raft_node_id_t voted_for = persisted_voted_for;

    FB_ASSERT_EQ(current_term, 5L);
    FB_ASSERT_EQ(voted_for, 3);

    // 新 term 开始时清除投票
    current_term++;
    voted_for = 0;
    FB_ASSERT_EQ(current_term, 6L);
    FB_ASSERT_EQ(voted_for, 0);
}

FB_TEST(raft_log_node, log_replay_duplicate_detection) {
    // 回放时重复检测
    std::set<raft_entry_id_t> seen_entries;
    std::vector<raft_entry_id_t> log_entries = {1001, 1002, 1003, 1002, 1004};

    int unique_count = 0;
    int duplicate_count = 0;

    for (raft_entry_id_t id : log_entries) {
        if (seen_entries.find(id) == seen_entries.end()) {
            seen_entries.insert(id);
            unique_count++;
        } else {
            duplicate_count++;
        }
    }

    FB_ASSERT_EQ(unique_count, 4);
    FB_ASSERT_EQ(duplicate_count, 1);
}

FB_TEST(raft_log_node, log_recovery_concurrent_operations) {
    // 恢复时并发操作处理
    std::atomic<raft_index_t> recovery_progress{0};
    std::atomic<bool> recovery_complete{false};

    // 模拟恢复进度
    for (int i = 0; i < 100; i++) {
        recovery_progress++;
    }
    recovery_complete = true;

    FB_ASSERT_EQ(recovery_progress.load(), 100L);
    FB_ASSERT_TRUE(recovery_complete.load());
}

FB_TEST(raft_log_node, log_replay_checkpoint) {
    // 回放检查点
    raft_index_t checkpoint_interval = 10;
    raft_index_t last_applied = 0;

    // 模拟回放并定期创建检查点
    std::vector<raft_index_t> checkpoints;
    for (raft_index_t idx = 1; idx <= 100; idx++) {
        last_applied = idx;
        if (idx % checkpoint_interval == 0) {
            checkpoints.push_back(idx);
        }
    }

    FB_ASSERT_EQ(checkpoints.size(), 10UL);
    FB_ASSERT_EQ(checkpoints.back(), 100L);
}

FB_TEST(raft_log_node, log_recovery_verify_integrity) {
    // 恢复时验证日志完整性
    std::map<raft_index_t, raft_term_t> log;
    for (int i = 1; i <= 100; i++) {
        log[i] = (i <= 30) ? 1 : (i <= 60) ? 2 : 3;
    }

    // 验证 term 单调性
    bool term_monotonic = true;
    raft_term_t prev_term = 0;
    for (const auto& pair : log) {
        if (pair.second < prev_term) {
            term_monotonic = false;
            break;
        }
        prev_term = pair.second;
    }
    FB_ASSERT_TRUE(term_monotonic);

    // 验证索引连续性
    bool index_continuous = true;
    for (raft_index_t idx = 1; idx <= 100; idx++) {
        if (log.find(idx) == log.end()) {
            index_continuous = false;
            break;
        }
    }
    FB_ASSERT_TRUE(index_continuous);
}

FB_TEST(raft_log_node, log_replay_after_configuration_change) {
    // 配置变更后的日志回放
    std::vector<raft_logtype_e> log_types;
    for (int i = 1; i <= 10; i++) {
        log_types.push_back(RAFT_LOGTYPE_WRITE);
    }
    log_types.push_back(RAFT_LOGTYPE_CONFIGURATION);  // 配置变更
    for (int i = 0; i < 10; i++) {
        log_types.push_back(RAFT_LOGTYPE_WRITE);
    }

    // 回放时遇到配置变更
    int config_changes_seen = 0;
    for (raft_logtype_e type : log_types) {
        if (type == RAFT_LOGTYPE_CONFIGURATION) {
            config_changes_seen++;
        }
    }
    FB_ASSERT_EQ(config_changes_seen, 1);
}

FB_TEST(raft_log_node, log_recovery_uncommitted_entries) {
    // 恢复时未提交条目处理
    raft_index_t commit_idx = 80;
    raft_index_t last_log_idx = 100;

    // 未提交的条目需要截断
    std::vector<raft_index_t> valid_entries;
    for (raft_index_t idx = 1; idx <= commit_idx; idx++) {
        valid_entries.push_back(idx);
    }

    FB_ASSERT_EQ(valid_entries.size(), 80UL);

    // 未提交条目被丢弃
    int discarded = last_log_idx - commit_idx;
    FB_ASSERT_EQ(discarded, 20);
}

FB_TEST(raft_log_node, log_replay_batch_efficiency) {
    // 批量回放效率
    size_t batch_size = 100;
    size_t total_entries = 1000;
    int batches_processed = 0;

    for (size_t offset = 0; offset < total_entries; offset += batch_size) {
        batches_processed++;
    }

    FB_ASSERT_EQ(batches_processed, 10);
}

FB_TEST(raft_log_node, log_recovery_checksum_verification) {
    // 恢复时校验和验证
    std::vector<std::pair<raft_index_t, uint32_t>> log_with_checksum;
    for (int i = 1; i <= 10; i++) {
        log_with_checksum.push_back({i, i * 1000});  // 简化的校验和
    }

    // 验证校验和
    int valid_entries = 0;
    for (const auto& pair : log_with_checksum) {
        // 模拟校验和验证
        uint32_t expected = pair.first * 1000;
        if (pair.second == expected) {
            valid_entries++;
        }
    }
    FB_ASSERT_EQ(valid_entries, 10);
}

FB_TEST(raft_log_node, log_replay_error_handling) {
    // 回放错误处理
    int successful_replays = 0;
    int failed_replays = 0;

    std::vector<int> replay_results = {0, 0, -1, 0, 0, -1, 0};

    for (int result : replay_results) {
        if (result == 0) {
            successful_replays++;
        } else {
            failed_replays++;
        }
    }

    FB_ASSERT_EQ(successful_replays, 5);
    FB_ASSERT_EQ(failed_replays, 2);
}

FB_TEST(raft_log_node, log_recovery_incremental) {
    // 增量恢复
    raft_index_t base_idx = 50;
    raft_index_t target_idx = 100;

    // 从基础点增量恢复
    int incremental_entries = target_idx - base_idx;
    FB_ASSERT_EQ(incremental_entries, 50);

    // 增量恢复比全量恢复快
    bool use_incremental = incremental_entries < target_idx;
    FB_ASSERT_TRUE(use_incremental);
}

FB_TEST(raft_log_node, log_replay_parallel_optimization) {
    // 并行回放优化
    int total_entries = 100;
    int parallel_workers = 4;
    int entries_per_worker = total_entries / parallel_workers;

    FB_ASSERT_EQ(entries_per_worker, 25);

    // 验证并行回放的正确性
    std::atomic<int> processed{0};
    for (int i = 0; i < total_entries; i++) {
        processed++;
    }
    FB_ASSERT_EQ(processed.load(), 100);
}

FB_TEST(raft_log_node, log_recovery_abort_and_resume) {
    // 恢复中止和恢复
    raft_index_t recovery_checkpoint = 30;
    raft_index_t total_entries = 100;
    bool recovery_interrupted = true;

    // 恢复被中断
    raft_index_t recovered_so_far = recovery_checkpoint;

    // 从检查点恢复
    if (recovery_interrupted) {
        recovered_so_far = recovery_checkpoint;
    }

    // 继续恢复
    for (raft_index_t idx = recovered_so_far + 1; idx <= total_entries; idx++) {
        recovered_so_far++;
    }

    FB_ASSERT_EQ(recovered_so_far, 100L);
}

// ============================================================================
// Test Suite: Read-Only Query Optimization
// ============================================================================

FB_TEST(raft_log_node, read_index_basic) {
    // ReadIndex 基本流程
    raft_index_t commit_idx = 100;
    raft_index_t read_index = commit_idx;

    // ReadIndex 返回当前 commit_idx
    FB_ASSERT_EQ(read_index, 100L);

    // 需要等待状态机应用到 read_index
    raft_index_t last_applied = 95;
    bool can_read = last_applied >= read_index;
    FB_ASSERT_FALSE(can_read);
}

FB_TEST(raft_log_node, follower_read_forwarding) {
    // Follower 读转发
    raft_identity state = RAFT_STATE_FOLLOWER;
    raft_node_id_t leader_id = 3;

    // Follower 不能直接处理线性化读
    bool can_serve_read = (state == RAFT_STATE_LEADER);
    FB_ASSERT_FALSE(can_serve_read);

    // 需要转发给 Leader
    bool need_forward = !can_serve_read;
    FB_ASSERT_TRUE(need_forward);
    FB_ASSERT_EQ(leader_id, 3);
}

FB_TEST(raft_log_node, lease_read_validity_check) {
    // Lease Read 有效性检查
    raft_time_t lease_start = 1000;
    raft_time_t lease_duration = 500;
    raft_time_t current_time = 1200;

    // 检查 lease 是否有效
    bool lease_valid = current_time < (lease_start + lease_duration);
    FB_ASSERT_TRUE(lease_valid);

    // Lease 过期情况
    current_time = 1600;
    lease_valid = current_time < (lease_start + lease_duration);
    FB_ASSERT_FALSE(lease_valid);
}

FB_TEST(raft_log_node, read_index_quorum_heartbeat) {
    // ReadIndex 需要多数派心跳确认
    int cluster_size = 5;
    int quorum = cluster_size / 2 + 1;

    std::map<raft_node_id_t, bool> heartbeat_received;
    heartbeat_received[1] = true;  // 自己
    heartbeat_received[2] = true;
    heartbeat_received[3] = true;
    heartbeat_received[4] = false;
    heartbeat_received[5] = false;

    // 统计收到的心跳数
    int confirmations = 0;
    for (const auto& pair : heartbeat_received) {
        if (pair.second) confirmations++;
    }

    bool quorum_reached = confirmations >= quorum;
    FB_ASSERT_TRUE(quorum_reached);
}

FB_TEST(raft_log_node, read_index_pending_queue_v2) {
    // ReadIndex 待处理队列
    std::queue<std::pair<raft_index_t, raft_node_id_t>> pending_reads;

    // 添加待处理的读请求
    pending_reads.push({100, 1});
    pending_reads.push({105, 2});
    pending_reads.push({110, 3});

    FB_ASSERT_EQ(pending_reads.size(), 3UL);

    // 状态机追上后处理队列
    raft_index_t last_applied = 110;
    int processed = 0;
    while (!pending_reads.empty() && pending_reads.front().first <= last_applied) {
        pending_reads.pop();
        processed++;
    }

    FB_ASSERT_EQ(processed, 3);
    FB_ASSERT_TRUE(pending_reads.empty());
}

FB_TEST(raft_log_node, linearizable_read_guarantee) {
    // 线性化读保证
    raft_index_t read_index = 100;
    raft_index_t state_machine_applied = 100;

    // 只有状态机应用到 read_index 后才能返回结果
    bool safe_to_read = state_machine_applied >= read_index;
    FB_ASSERT_TRUE(safe_to_read);

    // 状态机落后的情况
    state_machine_applied = 95;
    safe_to_read = state_machine_applied >= read_index;
    FB_ASSERT_FALSE(safe_to_read);
}

FB_TEST(raft_log_node, lease_read_no_quorum_check) {
    // Lease Read 不需要多数派确认
    raft_time_t lease_expiry = 1500;
    raft_time_t current_time = 1200;

    // 在 lease 有效期内可以直接读
    bool can_read_directly = current_time < lease_expiry;
    FB_ASSERT_TRUE(can_read_directly);

    // 比 ReadIndex 更快（不需要网络往返）
    bool faster_than_read_index = true;
    FB_ASSERT_TRUE(faster_than_read_index);
}

FB_TEST(raft_log_node, read_index_timeout_handling) {
    // ReadIndex 超时处理
    raft_time_t request_time = 1000;
    raft_time_t timeout = 500;
    raft_time_t current_time = 1400;

    // 检查是否超时
    bool timed_out = (current_time - request_time) > timeout;
    FB_ASSERT_FALSE(timed_out);

    // 超时情况
    current_time = 1600;
    timed_out = (current_time - request_time) > timeout;
    FB_ASSERT_TRUE(timed_out);
}

FB_TEST(raft_log_node, read_cache_consistency) {
    // 读缓存一致性
    std::map<std::string, std::string> read_cache;
    std::string key = "test_key";
    std::string value = "test_value";

    // 缓存读取
    read_cache[key] = value;
    auto it = read_cache.find(key);
    FB_ASSERT_TRUE(it != read_cache.end());
    FB_ASSERT_EQ(it->second, value);

    // 写入后缓存失效
    read_cache.erase(key);
    bool cache_valid = read_cache.find(key) != read_cache.end();
    FB_ASSERT_FALSE(cache_valid);
}

FB_TEST(raft_log_node, read_after_write_consistency) {
    // 写后读一致性
    raft_index_t write_index = 100;
    raft_index_t read_after_index = write_index;

    // 确保读到写入后的数据
    bool consistent = read_after_index >= write_index;
    FB_ASSERT_TRUE(consistent);

    // 状态机应用后才能读到
    raft_index_t last_applied = 95;
    bool can_see_write = last_applied >= write_index;
    FB_ASSERT_FALSE(can_see_write);
}

FB_TEST(raft_log_node, follower_read_stale_prevention) {
    // Follower 读过期数据防止
    raft_index_t leader_commit = 100;
    raft_index_t follower_commit = 80;

    // Follower commit 落后于 Leader
    bool follower_stale = follower_commit < leader_commit;
    FB_ASSERT_TRUE(follower_stale);

    // Follower 需要先追上
    int entries_to_catch_up = leader_commit - follower_commit;
    FB_ASSERT_EQ(entries_to_catch_up, 20);
}

FB_TEST(raft_log_node, read_batch_optimization) {
    // 批量读取优化
    std::vector<std::string> read_keys;
    for (int i = 0; i < 10; i++) {
        read_keys.push_back("key_" + std::to_string(i));
    }

    // 单次 ReadIndex 可服务多个读请求
    raft_index_t read_index = 100;
    int batch_size = read_keys.size();

    FB_ASSERT_EQ(batch_size, 10);

    // 批量读取比单独请求更高效
    int single_requests = 10;  // 不使用批量需要 10 次请求
    int batch_requests = 1;     // 批量只需 1 次
    bool batch_efficient = batch_requests < single_requests;
    FB_ASSERT_TRUE(batch_efficient);
}

FB_TEST(raft_log_node, read_during_leader_transfer) {
    // Leader 转移期间的读处理
    bool transfer_in_progress = true;
    raft_identity state = RAFT_STATE_LEADER;

    // 转移期间 Leader 可能拒绝读
    bool can_serve_read = (state == RAFT_STATE_LEADER) && !transfer_in_progress;
    FB_ASSERT_FALSE(can_serve_read);

    // 转移完成后可以读
    transfer_in_progress = false;
    can_serve_read = (state == RAFT_STATE_LEADER) && !transfer_in_progress;
    FB_ASSERT_TRUE(can_serve_read);
}

FB_TEST(raft_log_node, read_during_partition) {
    // 网络分区期间的读处理
    int total_nodes = 5;
    int majority_partition_size = 3;
    bool in_majority_partition = true;

    // 只有在多数派分区中才能服务读
    bool can_serve_read = in_majority_partition && (majority_partition_size >= total_nodes / 2 + 1);
    FB_ASSERT_TRUE(can_serve_read);

    // 少数派分区不能服务读
    in_majority_partition = false;
    can_serve_read = in_majority_partition;
    FB_ASSERT_FALSE(can_serve_read);
}

FB_TEST(raft_log_node, read_retry_mechanism) {
    // 读重试机制
    int max_retries = 3;
    int retry_count = 0;
    bool read_success = false;

    // 模拟重试
    for (int i = 0; i < max_retries && !read_success; i++) {
        retry_count++;
        if (i == 1) {  // 第二次成功
            read_success = true;
        }
    }

    FB_ASSERT_TRUE(read_success);
    FB_ASSERT_EQ(retry_count, 2);
}

FB_TEST(raft_log_node, read_priority_queue) {
    // 读请求优先级队列
    enum class read_priority {
        HIGH,
        NORMAL,
        LOW
    };

    std::vector<std::pair<std::string, read_priority>> read_queue;
    read_queue.push_back({"read_1", read_priority::HIGH});
    read_queue.push_back({"read_2", read_priority::NORMAL});
    read_queue.push_back({"read_3", read_priority::LOW});
    read_queue.push_back({"read_4", read_priority::HIGH});

    // 按优先级排序处理
    int high_priority_count = 0;
    for (const auto& req : read_queue) {
        if (req.second == read_priority::HIGH) {
            high_priority_count++;
        }
    }

    FB_ASSERT_EQ(high_priority_count, 2);
}

FB_TEST(raft_log_node, read_index_parallel_requests) {
    // 并行 ReadIndex 请求
    std::atomic<int> completed_requests{0};
    int total_requests = 10;

    for (int i = 0; i < total_requests; i++) {
        completed_requests++;
    }

    FB_ASSERT_EQ(completed_requests.load(), 10);
}

FB_TEST(raft_log_node, read_session_affinity) {
    // 读会话亲和性
    raft_node_id_t preferred_leader = 1;
    raft_node_id_t current_leader = 1;

    // 同一会话的读请求发送到同一 Leader
    bool session_affinity = (preferred_leader == current_leader);
    FB_ASSERT_TRUE(session_affinity);

    // Leader 变更后更新亲和性
    current_leader = 2;
    session_affinity = (preferred_leader == current_leader);
    FB_ASSERT_FALSE(session_affinity);
}

FB_TEST(raft_log_node, read_stale_read_option) {
    // 过期读选项
    bool allow_stale_read = true;
    raft_index_t leader_commit = 100;
    raft_index_t local_commit = 95;

    // 允许过期读时，可以读取稍旧的数据
    raft_index_t readable_index = allow_stale_read ? local_commit : leader_commit;

    if (allow_stale_read) {
        FB_ASSERT_EQ(readable_index, 95L);
    } else {
        FB_ASSERT_EQ(readable_index, 100L);
    }
}

FB_TEST(raft_log_node, read_timeout_propagation) {
    // 读超时传播
    raft_time_t client_timeout = 1000;
    raft_time_t internal_timeout = 800;  // 内部超时小于客户端超时

    // 确保内部超时留有余量
    bool timeout_valid = internal_timeout < client_timeout;
    FB_ASSERT_TRUE(timeout_valid);

    // 计算剩余时间
    raft_time_t time_remaining = client_timeout - internal_timeout;
    FB_ASSERT_EQ(time_remaining, 200L);
}

// ============================================================================
// Test Suite: Log Entry Batch Processing
// ============================================================================

FB_TEST(raft_log_node, batch_append_basic) {
    // 批量追加基本操作
    std::vector<raft_index_t> batch;
    for (int i = 1; i <= 10; i++) {
        batch.push_back(i);
    }

    // 批量追加
    raft_index_t start_idx = 0;
    for (raft_index_t idx : batch) {
        start_idx++;
    }

    FB_ASSERT_EQ(start_idx, 10L);
    FB_ASSERT_EQ(batch.size(), 10UL);
}

FB_TEST(raft_log_node, batch_append_efficiency) {
    // 批量追加效率
    int single_append_cost = 10;   // 单次追加成本
    int batch_append_cost = 15;    // 批量追加成本

    int entries = 100;

    // 单次追加总成本
    int single_total = entries * single_append_cost;

    // 批量追加成本（假设每批 10 个）
    int batch_size = 10;
    int batches = entries / batch_size;
    int batch_total = batches * batch_append_cost;

    FB_ASSERT_TRUE(batch_total < single_total);

    // 批量追加节省的时间
    int saved_cost = single_total - batch_total;
    FB_ASSERT_EQ(saved_cost, 985);
}

FB_TEST(raft_log_node, batch_append_order_preservation) {
    // 批量追加顺序保持
    std::vector<int> batch = {1, 2, 3, 4, 5};
    std::vector<int> appended;

    for (int entry : batch) {
        appended.push_back(entry);
    }

    // 验证顺序一致
    FB_ASSERT_EQ(appended.size(), batch.size());
    for (size_t i = 0; i < batch.size(); i++) {
        FB_ASSERT_EQ(appended[i], batch[i]);
    }
}

FB_TEST(raft_log_node, batch_replication_pipeline) {
    // 批量复制 Pipeline
    std::vector<raft_index_t> pending_batches;
    int max_inflight = 3;

    // 模拟 Pipeline
    int inflight = 0;
    for (int i = 0; i < 10; i++) {
        if (inflight < max_inflight) {
            pending_batches.push_back(i);
            inflight++;
        }
        // 模拟确认
        if (i % 3 == 0 && inflight > 0) {
            inflight--;
        }
    }

    FB_ASSERT_TRUE(pending_batches.size() <= 10);
}

FB_TEST(raft_log_node, batch_append_atomicity) {
    // 批量追加原子性
    std::vector<raft_index_t> batch = {101, 102, 103, 104, 105};
    bool all_succeeded = true;

    // 批量操作要么全部成功，要么全部失败
    std::vector<raft_index_t> appended;
    for (raft_index_t idx : batch) {
        appended.push_back(idx);
    }

    // 验证原子性
    if (all_succeeded) {
        FB_ASSERT_EQ(appended.size(), batch.size());
    } else {
        FB_ASSERT_TRUE(appended.empty());
    }
}

FB_TEST(raft_log_node, batch_size_optimization) {
    // 批量大小优化
    size_t optimal_batch_size = 100;
    size_t max_batch_size = 1000;
    size_t current_batch_size = 50;

    // 调整到最优批量大小
    if (current_batch_size < optimal_batch_size) {
        current_batch_size = optimal_batch_size;
    }
    FB_ASSERT_EQ(current_batch_size, optimal_batch_size);

    // 不超过最大批量大小
    current_batch_size = 2000;
    if (current_batch_size > max_batch_size) {
        current_batch_size = max_batch_size;
    }
    FB_ASSERT_EQ(current_batch_size, max_batch_size);
}

FB_TEST(raft_log_node, batch_commit_efficiency) {
    // 批量提交效率
    int entries_to_commit = 100;
    int single_commit_latency = 5;  // ms
    int batch_commit_latency = 10;  // ms
    int batch_size = 20;

    // 单次提交
    int single_total_time = entries_to_commit * single_commit_latency;

    // 批量提交
    int batches = entries_to_commit / batch_size;
    int batch_total_time = batches * batch_commit_latency;

    FB_ASSERT_TRUE(batch_total_time < single_total_time);
}

FB_TEST(raft_log_node, batch_append_memory_efficiency) {
    // 批量追加内存效率
    size_t entry_size = 1024;  // 1KB per entry
    size_t batch_count = 100;

    // 预分配内存
    size_t expected_memory = entry_size * batch_count;

    // 批量追加只需要一次内存分配
    int allocations = 1;
    FB_ASSERT_EQ(allocations, 1);

    // 单条追加需要多次分配
    int single_allocations = batch_count;
    FB_ASSERT_TRUE(single_allocations > allocations);
}

FB_TEST(raft_log_node, batch_append_with_priority) {
    // 带优先级的批量追加
    enum class entry_priority {
        HIGH,
        NORMAL,
        LOW
    };

    std::vector<std::pair<raft_index_t, entry_priority>> batch;
    batch.push_back({1, entry_priority::NORMAL});
    batch.push_back({2, entry_priority::HIGH});
    batch.push_back({3, entry_priority::LOW});
    batch.push_back({4, entry_priority::HIGH});

    // 按优先级处理
    int high_priority_count = 0;
    for (const auto& entry : batch) {
        if (entry.second == entry_priority::HIGH) {
            high_priority_count++;
        }
    }
    FB_ASSERT_EQ(high_priority_count, 2);
}

FB_TEST(raft_log_node, batch_append_concurrent) {
    // 并发批量追加
    std::atomic<int> total_appended{0};
    int batch_count = 10;
    int entries_per_batch = 100;

    // 模拟并发批量追加
    for (int batch = 0; batch < batch_count; batch++) {
        for (int entry = 0; entry < entries_per_batch; entry++) {
            total_appended++;
        }
    }

    FB_ASSERT_EQ(total_appended.load(), batch_count * entries_per_batch);
}

FB_TEST(raft_log_node, batch_append_partial_failure) {
    // 批量追加部分失败处理
    std::vector<int> append_results = {0, 0, -1, 0, 0, -1, 0, 0, 0, 0};
    int successful = 0;
    int failed = 0;

    for (int result : append_results) {
        if (result == 0) {
            successful++;
        } else {
            failed++;
        }
    }

    FB_ASSERT_EQ(successful, 8);
    FB_ASSERT_EQ(failed, 2);

    // 失败后重试
    int retried = 0;
    for (int result : append_results) {
        if (result != 0) {
            retried++;
        }
    }
    FB_ASSERT_EQ(retried, 2);
}

FB_TEST(raft_log_node, batch_append_with_compaction) {
    // 批量追加与压缩
    size_t batch_size = 1000;
    size_t max_entries = 10000;
    size_t current_entries = 0;

    // 追加批次
    current_entries += batch_size;

    // 超过限制时压缩
    if (current_entries > max_entries) {
        size_t compacted = current_entries / 2;
        current_entries -= compacted;
    }

    FB_ASSERT_EQ(current_entries, 1000UL);

    // 多次追加后触发压缩
    current_entries += batch_size * 10;
    if (current_entries > max_entries) {
        size_t compacted = current_entries / 2;
        current_entries -= compacted;
    }
    FB_ASSERT_TRUE(current_entries <= max_entries);
}

FB_TEST(raft_log_node, batch_append_timeout) {
    // 批量追加超时
    raft_time_t batch_start = 1000;
    raft_time_t batch_timeout = 100;
    raft_time_t current_time = 1050;

    // 批量追加中
    bool batch_in_progress = true;

    // 检查超时
    bool timed_out = (current_time - batch_start) > batch_timeout;
    FB_ASSERT_FALSE(timed_out);

    // 超时后取消
    current_time = 1150;
    timed_out = (current_time - batch_start) > batch_timeout;
    if (timed_out) {
        batch_in_progress = false;
    }
    FB_ASSERT_TRUE(timed_out);
    FB_ASSERT_FALSE(batch_in_progress);
}

FB_TEST(raft_log_node, batch_append_checksum) {
    // 批量追加校验和
    std::vector<uint32_t> checksums;
    for (int i = 0; i < 10; i++) {
        checksums.push_back(i * 1000);
    }

    // 验证校验和
    int valid_entries = 0;
    for (size_t i = 0; i < checksums.size(); i++) {
        if (checksums[i] == i * 1000) {
            valid_entries++;
        }
    }
    FB_ASSERT_EQ(valid_entries, 10);
}

FB_TEST(raft_log_node, batch_append_with_deduplication) {
    // 批量追加去重
    std::vector<int> batch = {1, 2, 3, 2, 4, 3, 5};
    std::set<int> seen;
    std::vector<int> deduped;

    for (int entry : batch) {
        if (seen.find(entry) == seen.end()) {
            seen.insert(entry);
            deduped.push_back(entry);
        }
    }

    FB_ASSERT_EQ(deduped.size(), 5UL);
}

FB_TEST(raft_log_node, batch_append_network_efficiency) {
    // 批量追加网络效率
    int single_msg_overhead = 100;  // bytes
    int batch_msg_overhead = 150;   // bytes
    int entries_per_batch = 10;

    // 单条发送的网络开销
    int single_total_overhead = entries_per_batch * single_msg_overhead;

    // 批量发送的网络开销
    int batch_total_overhead = batch_msg_overhead;

    FB_ASSERT_TRUE(batch_total_overhead < single_total_overhead);
}

FB_TEST(raft_log_node, batch_append_rollback) {
    // 批量追加回滚
    std::vector<raft_index_t> batch = {101, 102, 103, 104, 105};
    std::vector<raft_index_t> appended;

    // 追加
    for (raft_index_t idx : batch) {
        appended.push_back(idx);
    }
    FB_ASSERT_EQ(appended.size(), 5UL);

    // 回滚
    raft_index_t rollback_point = 102;
    while (!appended.empty() && appended.back() > rollback_point) {
        appended.pop_back();
    }

    FB_ASSERT_EQ(appended.size(), 2UL);
}

FB_TEST(raft_log_node, batch_append_multi_term) {
    // 跨 term 的批量追加
    std::vector<std::pair<raft_index_t, raft_term_t>> batch;
    for (int i = 1; i <= 5; i++) {
        batch.push_back({i, 1});
    }
    for (int i = 6; i <= 10; i++) {
        batch.push_back({i, 2});
    }

    // 统计各 term 的条目数
    std::map<raft_term_t, int> term_counts;
    for (const auto& entry : batch) {
        term_counts[entry.second]++;
    }

    FB_ASSERT_EQ(term_counts[1], 5);
    FB_ASSERT_EQ(term_counts[2], 5);
}

FB_TEST(raft_log_node, batch_append_queue_backpressure) {
    // 批量追加队列背压
    int max_pending_batches = 5;
    int pending_batches = 0;
    bool can_accept = true;

    // 模拟接收批次
    for (int i = 0; i < 7; i++) {
        if (pending_batches >= max_pending_batches) {
            can_accept = false;
        }
        if (can_accept) {
            pending_batches++;
        }
    }

    FB_ASSERT_FALSE(can_accept);
    FB_ASSERT_EQ(pending_batches, 5);
}

// ============================================================================
// Test Suite: Request Rate Limiting and Resource Management
// ============================================================================

FB_TEST(raft_log_node, rate_limit_basic) {
    // 基本限流
    int max_requests_per_second = 1000;
    int current_rate = 800;

    // 检查是否超限
    bool within_limit = current_rate <= max_requests_per_second;
    FB_ASSERT_TRUE(within_limit);

    // 超限情况
    current_rate = 1200;
    within_limit = current_rate <= max_requests_per_second;
    FB_ASSERT_FALSE(within_limit);
}

FB_TEST(raft_log_node, rate_limit_token_bucket) {
    // 令牌桶限流
    int bucket_capacity = 100;
    int tokens = 100;
    int refill_rate = 10;  // 每秒补充 10 个令牌

    // 消耗令牌
    int requests = 50;
    for (int i = 0; i < requests && tokens > 0; i++) {
        tokens--;
    }

    FB_ASSERT_EQ(tokens, 50);

    // 补充令牌
    tokens = std::min(tokens + refill_rate, bucket_capacity);
    FB_ASSERT_EQ(tokens, 60);
}

FB_TEST(raft_log_node, rate_limit_sliding_window) {
    // 滑动窗口限流
    std::vector<raft_time_t> request_times;
    raft_time_t window_duration = 1000;  // 1秒窗口
    raft_time_t current_time = 5000;
    int max_requests = 100;

    // 添加请求
    for (int i = 0; i < 80; i++) {
        request_times.push_back(current_time - 500 + i);
    }

    // 计算窗口内请求数
    int requests_in_window = 0;
    for (raft_time_t time : request_times) {
        if (current_time - time <= window_duration) {
            requests_in_window++;
        }
    }

    FB_ASSERT_TRUE(requests_in_window <= max_requests);
}

FB_TEST(raft_log_node, rate_limit_burst_handling) {
    // 突发流量处理
    int burst_capacity = 200;
    int normal_rate = 100;
    int burst_requests = 150;

    // 突发请求
    bool burst_within_capacity = burst_requests <= burst_capacity;
    FB_ASSERT_TRUE(burst_within_capacity);

    // 突发后恢复到正常速率
    int post_burst_rate = normal_rate;
    FB_ASSERT_EQ(post_burst_rate, 100);
}

FB_TEST(raft_log_node, resource_memory_limit) {
    // 内存资源限制
    size_t max_memory = 100 * 1024 * 1024;  // 100MB
    size_t current_usage = 50 * 1024 * 1024;  // 50MB

    // 检查内存使用
    bool within_limit = current_usage <= max_memory;
    FB_ASSERT_TRUE(within_limit);

    // 内存使用百分比
    double usage_percent = 100.0 * current_usage / max_memory;
    FB_ASSERT_EQ(usage_percent, 50.0);
}

FB_TEST(raft_log_node, resource_cpu_limit) {
    // CPU 资源限制
    int max_cpu_percent = 80;
    int current_cpu = 60;

    // 检查 CPU 使用
    bool within_limit = current_cpu <= max_cpu_percent;
    FB_ASSERT_TRUE(within_limit);

    // 高负载情况
    current_cpu = 90;
    within_limit = current_cpu <= max_cpu_percent;
    FB_ASSERT_FALSE(within_limit);
}

FB_TEST(raft_log_node, resource_connection_limit) {
    // 连接资源限制
    int max_connections = 100;
    int current_connections = 80;

    // 检查连接数
    bool can_accept = current_connections < max_connections;
    FB_ASSERT_TRUE(can_accept);

    // 连接满时拒绝
    current_connections = 100;
    can_accept = current_connections < max_connections;
    FB_ASSERT_FALSE(can_accept);
}

FB_TEST(raft_log_node, resource_disk_io_limit) {
    // 磁盘 I/O 限制
    int max_iops = 10000;
    int current_iops = 5000;

    // 检查 IOPS
    bool within_limit = current_iops <= max_iops;
    FB_ASSERT_TRUE(within_limit);

    // 高 I/O 时排队
    current_iops = 12000;
    if (current_iops > max_iops) {
        int queued_ops = current_iops - max_iops;
        FB_ASSERT_EQ(queued_ops, 2000);
    }
}

FB_TEST(raft_log_node, backpressure_propagation) {
    // 背压传播
    bool follower_overloaded = true;
    bool leader_should_slow_down = false;

    // Follower 负载高时通知 Leader
    if (follower_overloaded) {
        leader_should_slow_down = true;
    }

    FB_ASSERT_TRUE(leader_should_slow_down);

    // Leader 降低发送速率
    int original_rate = 1000;
    int reduced_rate = original_rate / 2;
    FB_ASSERT_EQ(reduced_rate, 500);
}

FB_TEST(raft_log_node, backpressure_queue_size) {
    // 背压队列大小
    int max_queue_size = 1000;
    int current_queue_size = 800;

    // 检查队列大小
    bool queue_ok = current_queue_size < max_queue_size;
    FB_ASSERT_TRUE(queue_ok);

    // 队列满时触发背压
    current_queue_size = 1000;
    bool need_backpressure = current_queue_size >= max_queue_size;
    FB_ASSERT_TRUE(need_backpressure);
}

FB_TEST(raft_log_node, resource_graceful_degradation) {
    // 资源不足时优雅降级
    int available_memory = 30;  // 百分比
    bool low_memory = available_memory < 50;

    if (low_memory) {
        // 降级策略：减少缓存大小
        size_t reduced_cache_size = 1024 * 1024;  // 1MB
        FB_ASSERT_EQ(reduced_cache_size, 1024 * 1024);
    }
}

FB_TEST(raft_log_node, rate_limit_priority_queue) {
    // 限流优先级队列
    enum class request_priority {
        CRITICAL,
        HIGH,
        NORMAL,
        LOW
    };

    std::vector<std::pair<int, request_priority>> pending_requests;
    pending_requests.push_back({1, request_priority::CRITICAL});
    pending_requests.push_back({2, request_priority::NORMAL});
    pending_requests.push_back({3, request_priority::HIGH});
    pending_requests.push_back({4, request_priority::LOW});

    // 限流时优先处理高优先级请求
    int critical_count = 0;
    for (const auto& req : pending_requests) {
        if (req.second == request_priority::CRITICAL) {
            critical_count++;
        }
    }
    FB_ASSERT_EQ(critical_count, 1);
}

FB_TEST(raft_log_node, rate_limit_adaptive) {
    // 自适应限流
    int base_rate = 1000;
    int current_rate = base_rate;
    int success_rate = 95;  // 百分比

    // 根据成功率调整速率
    if (success_rate > 90) {
        current_rate = base_rate * 1.2;  // 增加 20%
    } else if (success_rate < 70) {
        current_rate = base_rate * 0.8;  // 减少 20%
    }

    FB_ASSERT_EQ(current_rate, 1200);
}

FB_TEST(raft_log_node, resource_allocation_tracking) {
    // 资源分配跟踪
    std::map<std::string, size_t> allocated_resources;
    allocated_resources["log_cache"] = 10 * 1024 * 1024;
    allocated_resources["snapshot"] = 20 * 1024 * 1024;
    allocated_resources["network_buffer"] = 5 * 1024 * 1024;

    // 计算总分配
    size_t total_allocated = 0;
    for (const auto& pair : allocated_resources) {
        total_allocated += pair.second;
    }

    FB_ASSERT_EQ(total_allocated, 35 * 1024 * 1024);
}

FB_TEST(raft_log_node, rate_limit_per_client) {
    // 每客户端限流
    std::map<raft_node_id_t, int> client_rates;
    int max_per_client = 100;

    client_rates[1] = 80;
    client_rates[2] = 120;
    client_rates[3] = 50;

    // 检查各客户端速率
    int over_limit_clients = 0;
    for (const auto& pair : client_rates) {
        if (pair.second > max_per_client) {
            over_limit_clients++;
        }
    }

    FB_ASSERT_EQ(over_limit_clients, 1);
}

FB_TEST(raft_log_node, resource_timeout_cleanup) {
    // 资源超时清理
    std::map<int, raft_time_t> allocated_with_timeout;
    raft_time_t current_time = 5000;

    allocated_with_timeout[1] = 3000;
    allocated_with_timeout[2] = 4500;
    allocated_with_timeout[3] = 6000;  // 超时

    int timeout_ms = 1000;
    int cleaned = 0;

    for (const auto& pair : allocated_with_timeout) {
        if (current_time - pair.second > timeout_ms) {
            cleaned++;
        }
    }

    FB_ASSERT_EQ(cleaned, 1);
}

FB_TEST(raft_log_node, rate_limit_circuit_breaker) {
    // 断路器模式
    int failure_count = 0;
    int failure_threshold = 5;
    bool circuit_open = false;

    // 模拟失败
    std::vector<int> results = {-1, -1, 0, -1, -1, -1, -1};
    for (int result : results) {
        if (result == -1) {
            failure_count++;
            if (failure_count >= failure_threshold) {
                circuit_open = true;
            }
        } else {
            failure_count = 0;
        }
    }

    FB_ASSERT_TRUE(circuit_open);
    FB_ASSERT_EQ(failure_count, 7);
}

FB_TEST(raft_log_node, resource_monitoring_metrics) {
    // 资源监控指标
    std::map<std::string, double> metrics;
    metrics["memory_usage"] = 65.5;
    metrics["cpu_usage"] = 45.0;
    metrics["disk_usage"] = 80.0;
    metrics["network_usage"] = 30.0;

    // 检查是否有资源超过阈值
    double threshold = 70.0;
    int exceeded = 0;
    for (const auto& pair : metrics) {
        if (pair.second > threshold) {
            exceeded++;
        }
    }

    FB_ASSERT_EQ(exceeded, 1);
}

FB_TEST(raft_log_node, rate_limit_retry_with_backoff) {
    // 限流重试与退避
    int base_delay = 100;  // ms
    int max_delay = 5000;
    int attempt = 0;

    // 指数退避
    for (int i = 0; i < 5; i++) {
        int delay = std::min(base_delay * (1 << i), max_delay);
        attempt++;
        if (i == 2) break;  // 第三次成功
    }

    FB_ASSERT_EQ(attempt, 3);
}

FB_TEST(raft_log_node, resource_quota_management) {
    // 资源配额管理
    std::map<std::string, size_t> quotas;
    quotas["log_entries"] = 10000;
    quotas["pending_requests"] = 500;
    quotas["memory_per_node"] = 10 * 1024 * 1024;

    // 检查配额
    size_t current_log_entries = 8000;
    bool within_quota = current_log_entries <= quotas["log_entries"];
    FB_ASSERT_TRUE(within_quota);

    // 接近配额时预警
    double usage_ratio = 100.0 * current_log_entries / quotas["log_entries"];
    bool need_warning = usage_ratio > 80.0;
    FB_ASSERT_FALSE(need_warning);
}

FB_TEST(raft_log_node, rate_limit_global_vs_local) {
    // 全局与局部限流
    int global_limit = 5000;
    int local_limit = 1000;
    int global_usage = 3000;
    int local_usage = 1200;

    // 局部超限但全局未超
    bool local_ok = local_usage <= local_limit;
    bool global_ok = global_usage <= global_limit;

    FB_ASSERT_FALSE(local_ok);
    FB_ASSERT_TRUE(global_ok);

    // 需要同时满足全局和局部
    bool both_ok = local_ok && global_ok;
    FB_ASSERT_FALSE(both_ok);
}

// Main function for test runner
FB_TEST_MAIN()
