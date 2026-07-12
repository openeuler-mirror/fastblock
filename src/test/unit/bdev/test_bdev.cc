/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/**
 * @file test_bdev.cc
 * @brief Unit tests for bdev module data contracts (mirrored locally).
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <algorithm>
#include <limits>

// ============================================================================
// Local mirrors of bdev data contracts
// ============================================================================

namespace {

// ---------- Constants (mirrored from bdev_fastblock.cc) --------------------
constexpr uint32_t SPDK_FASTBLOCK_QUEUE_DEPTH = 128;
constexpr uint32_t MAX_EVENTS_PER_POLL = 128;

// ---------- bdev_fastblock structure (mirrored) ---------------------------
struct bdev_fastblock_mirror {
    std::string name;
    std::string image_name;
    std::string monitor_address;
    uint64_t pool_id{0};
    std::string pool_name;
    uint64_t image_size{0};
    uint32_t block_size{0};
    uint64_t object_size{0};
};

// ---------- RPC request structures (mirrored from bdev_fastblock_rpc.cc) --
struct rpc_create_fastblock_mirror {
    std::string name;
    uint64_t pool_id{0};
    std::string pool_name;
    std::string image_name;
    uint64_t image_size{0};
    uint64_t object_size{0};
    uint32_t block_size{0};
    std::string monitor_address;
};

struct rpc_bdev_fastblock_delete_mirror {
    std::string name;
};

struct rpc_bdev_fastblock_resize_mirror {
    std::string name;
    uint64_t new_size{0};
};

// ---------- Configuration structures (mirrored) ----------------------------
struct bdev_config_entry {
    std::string key;
    std::string value;
};

struct bdev_config {
    std::vector<bdev_config_entry> entries;

    void add(const std::string& k, const std::string& v) {
        entries.push_back({k, v});
    }

    std::optional<std::string> get(const std::string& k) const {
        for (const auto& e : entries) {
            if (e.key == k) return e.value;
        }
        return std::nullopt;
    }

    size_t count() const { return entries.size(); }

    void clear() { entries.clear(); }
};

// ---------- Global configuration variables (mirrored from common.cc) ------
struct bdev_global_config_mirror {
    std::string mon_cluster_endpoints;
    std::string conf_path;
    int core_num{1};
    std::string app_name;
    bool app_stop{false};
};

// ---------- app_stop_context state machine (mirrored from common.cc) -----
enum class app_stop_state {
    running = 1,
    monitor_stopped,
    connect_cache_stopped,
    stopping_block_clients,
    stopping_spdk_threads
};

struct app_stop_context_mirror {
    app_stop_state current_state{app_stop_state::running};
    int64_t counter{0};

    void advance() {
        switch (current_state) {
            case app_stop_state::running:
                current_state = app_stop_state::monitor_stopped;
                break;
            case app_stop_state::monitor_stopped:
                current_state = app_stop_state::connect_cache_stopped;
                break;
            case app_stop_state::connect_cache_stopped:
                current_state = app_stop_state::stopping_block_clients;
                break;
            case app_stop_state::stopping_block_clients:
                current_state = app_stop_state::stopping_spdk_threads;
                break;
            case app_stop_state::stopping_spdk_threads:
                break;
        }
    }
};

// ---------- Command-line option definitions (mirrored from common.cc) -----
constexpr char BLOCK_OPTION_CONF = 'C';
constexpr char BLOCK_OPTION_NUMA_NODE = 'N';
constexpr char BLOCK_OPTION_CORE_NUM = 'S';

struct cmdline_option_mirror {
    std::string name;
    bool has_arg{false};
    int val{0};
};

// ---------- IO completion status codes (mirrored from SPDK) ---------------
enum class bdev_io_status {
    SUCCESS = 0,
    FAILED = 1,
    PENDING = 2,
    RESET = 3,
    ABORTED = 4,
    NOMEDIA = 5,
    DATA_OVERRUN = 6,
    UNALLOCATED = 7,
    CHECKSUM_ERROR = 8,
    OUT_OF_RANGE = 9,
};

// ---------- Image info structure (mirrored from client) -------------------
struct image_info_mirror {
    std::string pool_name;
    std::string image_name;
    uint64_t image_size{0};
    uint64_t object_size{0};
};

// ---------- Default values --------------------------------------------------
constexpr uint32_t DEFAULT_BLOCK_SIZE = 4096;
constexpr uint64_t DEFAULT_OBJECT_SIZE = 4194304;
constexpr uint32_t DEFAULT_QUEUE_DEPTH = SPDK_FASTBLOCK_QUEUE_DEPTH;

} // anonymous namespace


// ============================================================================
// Test Suite: bdev_constants — Constant values
// ============================================================================

FB_SUITE_SETUP(bdev_constants) {}
FB_SUITE_TEARDOWN(bdev_constants) {}

FB_TEST(bdev_constants, queue_depth_value) {
    FB_ASSERT_EQ(SPDK_FASTBLOCK_QUEUE_DEPTH, 128u);
}

FB_TEST(bdev_constants, max_events_per_poll) {
    FB_ASSERT_EQ(MAX_EVENTS_PER_POLL, 128u);
}

FB_TEST(bdev_constants, default_block_size) {
    FB_ASSERT_EQ(DEFAULT_BLOCK_SIZE, 4096u);
}

FB_TEST(bdev_constants, default_object_size) {
    FB_ASSERT_EQ(DEFAULT_OBJECT_SIZE, 4194304u);
    FB_ASSERT_EQ(DEFAULT_OBJECT_SIZE, 4u * 1024 * 1024);
}

FB_TEST(bdev_constants, queue_depth_equals_max_events) {
    FB_ASSERT_EQ(SPDK_FASTBLOCK_QUEUE_DEPTH, MAX_EVENTS_PER_POLL);
}

// ============================================================================
// Test Suite: bdev_fastblock_struct — bdev_fastblock structure
// ============================================================================

FB_SUITE_SETUP(bdev_fastblock_struct) {}
FB_SUITE_TEARDOWN(bdev_fastblock_struct) {}

FB_TEST(bdev_fastblock_struct, default_values) {
    bdev_fastblock_mirror bdev;
    FB_ASSERT_TRUE(bdev.name.empty());
    FB_ASSERT_TRUE(bdev.image_name.empty());
    FB_ASSERT_TRUE(bdev.monitor_address.empty());
    FB_ASSERT_EQ(bdev.pool_id, 0u);
    FB_ASSERT_TRUE(bdev.pool_name.empty());
    FB_ASSERT_EQ(bdev.image_size, 0u);
    FB_ASSERT_EQ(bdev.block_size, 0u);
    FB_ASSERT_EQ(bdev.object_size, 0u);
}

FB_TEST(bdev_fastblock_struct, field_assignment) {
    bdev_fastblock_mirror bdev;
    bdev.name = "fbdev0";
    bdev.image_name = "myimage";
    bdev.monitor_address = "127.0.0.1:3333";
    bdev.pool_id = 1;
    bdev.pool_name = "fb";
    bdev.image_size = 100ull * 1024 * 1024 * 1024;
    bdev.block_size = 4096;
    bdev.object_size = 4 * 1024 * 1024;

    FB_ASSERT_STR_EQ(bdev.name.c_str(), "fbdev0");
    FB_ASSERT_STR_EQ(bdev.image_name.c_str(), "myimage");
    FB_ASSERT_STR_EQ(bdev.monitor_address.c_str(), "127.0.0.1:3333");
    FB_ASSERT_EQ(bdev.pool_id, 1u);
    FB_ASSERT_STR_EQ(bdev.pool_name.c_str(), "fb");
    FB_ASSERT_EQ(bdev.image_size, 100ull * 1024 * 1024 * 1024);
    FB_ASSERT_EQ(bdev.block_size, 4096u);
    FB_ASSERT_EQ(bdev.object_size, 4194304u);
}

FB_TEST(bdev_fastblock_struct, large_image_size) {
    bdev_fastblock_mirror bdev;
    bdev.image_size = std::numeric_limits<uint64_t>::max();
    FB_ASSERT_EQ(bdev.image_size, std::numeric_limits<uint64_t>::max());
}

FB_TEST(bdev_fastblock_struct, block_size_alignment) {
    bdev_fastblock_mirror bdev;
    bdev.block_size = 512;
    FB_ASSERT_EQ(bdev.block_size, 512u);

    bdev.block_size = 4096;
    FB_ASSERT_EQ(bdev.block_size, 4096u);
}

// ============================================================================
// Test Suite: bdev_rpc_create — RPC create request structure
// ============================================================================

FB_SUITE_SETUP(bdev_rpc_create) {}
FB_SUITE_TEARDOWN(bdev_rpc_create) {}

FB_TEST(bdev_rpc_create, default_values) {
    rpc_create_fastblock_mirror req;
    FB_ASSERT_TRUE(req.name.empty());
    FB_ASSERT_EQ(req.pool_id, 0u);
    FB_ASSERT_TRUE(req.pool_name.empty());
    FB_ASSERT_TRUE(req.image_name.empty());
    FB_ASSERT_EQ(req.image_size, 0u);
    FB_ASSERT_EQ(req.object_size, 0u);
    FB_ASSERT_EQ(req.block_size, 0u);
    FB_ASSERT_TRUE(req.monitor_address.empty());
}

FB_TEST(bdev_rpc_create, all_fields_populated) {
    rpc_create_fastblock_mirror req;
    req.name = "bdev0";
    req.pool_id = 42;
    req.pool_name = "mypool";
    req.image_name = "myvol";
    req.image_size = 107374182400;
    req.object_size = 4194304;
    req.block_size = 4096;
    req.monitor_address = "10.0.0.1:3333";

    FB_ASSERT_STR_EQ(req.name.c_str(), "bdev0");
    FB_ASSERT_EQ(req.pool_id, 42u);
    FB_ASSERT_STR_EQ(req.pool_name.c_str(), "mypool");
    FB_ASSERT_STR_EQ(req.image_name.c_str(), "myvol");
    FB_ASSERT_EQ(req.image_size, 107374182400u);
    FB_ASSERT_EQ(req.object_size, 4194304u);
    FB_ASSERT_EQ(req.block_size, 4096u);
    FB_ASSERT_STR_EQ(req.monitor_address.c_str(), "10.0.0.1:3333");
}

FB_TEST(bdev_rpc_create, optional_object_size) {
    rpc_create_fastblock_mirror req;
    req.object_size = 0;
    FB_ASSERT_EQ(req.object_size, 0u);

    req.object_size = DEFAULT_OBJECT_SIZE;
    FB_ASSERT_EQ(req.object_size, DEFAULT_OBJECT_SIZE);
}

// ============================================================================
// Test Suite: bdev_rpc_delete — RPC delete request structure
// ============================================================================

FB_SUITE_SETUP(bdev_rpc_delete) {}
FB_SUITE_TEARDOWN(bdev_rpc_delete) {}

FB_TEST(bdev_rpc_delete, default_values) {
    rpc_bdev_fastblock_delete_mirror req;
    FB_ASSERT_TRUE(req.name.empty());
}

FB_TEST(bdev_rpc_delete, name_assignment) {
    rpc_bdev_fastblock_delete_mirror req;
    req.name = "bdev_to_delete";
    FB_ASSERT_STR_EQ(req.name.c_str(), "bdev_to_delete");
}

// ============================================================================
// Test Suite: bdev_rpc_resize — RPC resize request structure
// ============================================================================

FB_SUITE_SETUP(bdev_rpc_resize) {}
FB_SUITE_TEARDOWN(bdev_rpc_resize) {}

FB_TEST(bdev_rpc_resize, default_values) {
    rpc_bdev_fastblock_resize_mirror req;
    FB_ASSERT_TRUE(req.name.empty());
    FB_ASSERT_EQ(req.new_size, 0u);
}

FB_TEST(bdev_rpc_resize, fields_populated) {
    rpc_bdev_fastblock_resize_mirror req;
    req.name = "bdev0";
    req.new_size = 200;

    FB_ASSERT_STR_EQ(req.name.c_str(), "bdev0");
    FB_ASSERT_EQ(req.new_size, 200u);
}

FB_TEST(bdev_rpc_resize, size_in_mib) {
    rpc_bdev_fastblock_resize_mirror req;
    req.new_size = 1024;
    FB_ASSERT_EQ(req.new_size, 1024u);

    uint64_t bytes = req.new_size * 1024 * 1024;
    FB_ASSERT_EQ(bytes, 1073741824ull);
}

// ============================================================================
// Test Suite: bdev_config — Configuration management
// ============================================================================

FB_SUITE_SETUP(bdev_config) {}
FB_SUITE_TEARDOWN(bdev_config) {}

FB_TEST(bdev_config, empty_config) {
    bdev_config config;
    FB_ASSERT_EQ(config.count(), 0u);
    FB_ASSERT_FALSE(config.get("key").has_value());
}

FB_TEST(bdev_config, add_and_retrieve) {
    bdev_config config;
    config.add("mon_host", "10.0.0.1");
    config.add("rdma_device_name", "mlx5_0");

    FB_ASSERT_EQ(config.count(), 2u);
    FB_ASSERT_TRUE(config.get("mon_host").has_value());
    FB_ASSERT_STR_EQ(config.get("mon_host").value().c_str(), "10.0.0.1");
    FB_ASSERT_STR_EQ(config.get("rdma_device_name").value().c_str(), "mlx5_0");
}

FB_TEST(bdev_config, missing_key) {
    bdev_config config;
    config.add("existing", "value");

    auto result = config.get("nonexistent");
    FB_ASSERT_FALSE(result.has_value());
}

FB_TEST(bdev_config, clear_config) {
    bdev_config config;
    config.add("key1", "value1");
    config.add("key2", "value2");

    FB_ASSERT_EQ(config.count(), 2u);
    config.clear();
    FB_ASSERT_EQ(config.count(), 0u);
}

FB_TEST(bdev_config, duplicate_keys) {
    bdev_config config;
    config.add("mon_host", "10.0.0.1");
    config.add("mon_host", "10.0.0.2");

    FB_ASSERT_EQ(config.count(), 2u);
    FB_ASSERT_STR_EQ(config.get("mon_host").value().c_str(), "10.0.0.1");
}

// ============================================================================
// Test Suite: bdev_global_config — Global configuration
// ============================================================================

FB_SUITE_SETUP(bdev_global_config) {}
FB_SUITE_TEARDOWN(bdev_global_config) {}

FB_TEST(bdev_global_config, default_values) {
    bdev_global_config_mirror cfg;
    FB_ASSERT_TRUE(cfg.mon_cluster_endpoints.empty());
    FB_ASSERT_TRUE(cfg.conf_path.empty());
    FB_ASSERT_EQ(cfg.core_num, 1);
    FB_ASSERT_TRUE(cfg.app_name.empty());
    FB_ASSERT_FALSE(cfg.app_stop);
}

FB_TEST(bdev_global_config, field_assignment) {
    bdev_global_config_mirror cfg;
    cfg.mon_cluster_endpoints = "10.0.0.1,10.0.0.2,10.0.0.3";
    cfg.conf_path = "/etc/fastblock/fastblock.json";
    cfg.core_num = 4;
    cfg.app_name = "fastblock-vhost";
    cfg.app_stop = true;

    FB_ASSERT_STR_EQ(cfg.mon_cluster_endpoints.c_str(), "10.0.0.1,10.0.0.2,10.0.0.3");
    FB_ASSERT_STR_EQ(cfg.conf_path.c_str(), "/etc/fastblock/fastblock.json");
    FB_ASSERT_EQ(cfg.core_num, 4);
    FB_ASSERT_STR_EQ(cfg.app_name.c_str(), "fastblock-vhost");
    FB_ASSERT_TRUE(cfg.app_stop);
}

// ============================================================================
// Test Suite: bdev_app_stop_state — App stop state machine
// ============================================================================

FB_SUITE_SETUP(bdev_app_stop_state) {}
FB_SUITE_TEARDOWN(bdev_app_stop_state) {}

FB_TEST(bdev_app_stop_state, initial_state) {
    app_stop_context_mirror stop_ctx;
    FB_ASSERT_TRUE(stop_ctx.current_state == app_stop_state::running);
    FB_ASSERT_EQ(stop_ctx.counter, 0);
}

FB_TEST(bdev_app_stop_state, state_transitions) {
    app_stop_context_mirror stop_ctx;

    stop_ctx.advance();
    FB_ASSERT_TRUE(stop_ctx.current_state == app_stop_state::monitor_stopped);

    stop_ctx.advance();
    FB_ASSERT_TRUE(stop_ctx.current_state == app_stop_state::connect_cache_stopped);

    stop_ctx.advance();
    FB_ASSERT_TRUE(stop_ctx.current_state == app_stop_state::stopping_block_clients);

    stop_ctx.advance();
    FB_ASSERT_TRUE(stop_ctx.current_state == app_stop_state::stopping_spdk_threads);
}

FB_TEST(bdev_app_stop_state, counter_increments) {
    app_stop_context_mirror stop_ctx;
    stop_ctx.counter = 0;
    stop_ctx.counter++;
    stop_ctx.counter++;
    FB_ASSERT_EQ(stop_ctx.counter, 2);
}

FB_TEST(bdev_app_stop_state, terminal_state_stays) {
    app_stop_context_mirror stop_ctx;
    stop_ctx.current_state = app_stop_state::stopping_spdk_threads;

    stop_ctx.advance();
    FB_ASSERT_TRUE(stop_ctx.current_state == app_stop_state::stopping_spdk_threads);
}

// ============================================================================
// Test Main Entry Point
// ============================================================================

FB_TEST_MAIN()
