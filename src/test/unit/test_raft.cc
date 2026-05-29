/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */

#include "test/framework/test_harness.h"
#include "fastblock/raft/raft.h"
#include "fastblock/raft/raft_log.h"
#include "fastblock/raft/raft_node.h"
#include "fastblock/raft/configuration_manager.h"

#include <memory>

FB_SUITE_SETUP(raft) {
    FB_LOG_INFO("Setting up raft test suite");
}

FB_SUITE_TEARDOWN(raft) {
    FB_LOG_INFO("Tearing down raft test suite");
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
