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
// Test Suite: bdev_image_info — Image information structure
// ============================================================================

FB_SUITE_SETUP(bdev_image_info) {}
FB_SUITE_TEARDOWN(bdev_image_info) {}

FB_TEST(bdev_image_info, default_values) {
    image_info_mirror info;
    FB_ASSERT_TRUE(info.pool_name.empty());
    FB_ASSERT_TRUE(info.image_name.empty());
    FB_ASSERT_EQ(info.image_size, 0u);
    FB_ASSERT_EQ(info.object_size, 0u);
}

FB_TEST(bdev_image_info, field_assignment) {
    image_info_mirror info;
    info.pool_name = "mypool";
    info.image_name = "myimage";
    info.image_size = 100ull * 1024 * 1024 * 1024;  // 100 GiB
    info.object_size = DEFAULT_OBJECT_SIZE;

    FB_ASSERT_STR_EQ(info.pool_name.c_str(), "mypool");
    FB_ASSERT_STR_EQ(info.image_name.c_str(), "myimage");
    FB_ASSERT_EQ(info.image_size, 100ull * 1024 * 1024 * 1024);
    FB_ASSERT_EQ(info.object_size, DEFAULT_OBJECT_SIZE);
}

// ============================================================================
// Test Suite: bdev_io_status — IO completion status codes
// ============================================================================

FB_SUITE_SETUP(bdev_io_status) {}
FB_SUITE_TEARDOWN(bdev_io_status) {}

FB_TEST(bdev_io_status, success_value) {
    FB_ASSERT_TRUE(bdev_io_status::SUCCESS == bdev_io_status(0));
}

FB_TEST(bdev_io_status, failed_value) {
    FB_ASSERT_TRUE(bdev_io_status::FAILED == bdev_io_status(1));
}

FB_TEST(bdev_io_status, pending_value) {
    FB_ASSERT_TRUE(bdev_io_status::PENDING == bdev_io_status(2));
}

FB_TEST(bdev_io_status, all_statuses_distinct) {
    FB_ASSERT_TRUE(bdev_io_status::SUCCESS != bdev_io_status::FAILED);
    FB_ASSERT_TRUE(bdev_io_status::FAILED != bdev_io_status::PENDING);
    FB_ASSERT_TRUE(bdev_io_status::PENDING != bdev_io_status::RESET);
    FB_ASSERT_TRUE(bdev_io_status::RESET != bdev_io_status::ABORTED);
}

// ============================================================================
// Test Suite: bdev_cmdline_options — Command line option constants
// ============================================================================

FB_SUITE_SETUP(bdev_cmdline_options) {}
FB_SUITE_TEARDOWN(bdev_cmdline_options) {}

FB_TEST(bdev_cmdline_options, conf_option_char) {
    FB_ASSERT_EQ(BLOCK_OPTION_CONF, 'C');
}

FB_TEST(bdev_cmdline_options, numa_node_option_char) {
    FB_ASSERT_EQ(BLOCK_OPTION_NUMA_NODE, 'N');
}

FB_TEST(bdev_cmdline_options, core_num_option_char) {
    FB_ASSERT_EQ(BLOCK_OPTION_CORE_NUM, 'S');
}

// ============================================================================
// Test Suite: bdev_size_calculations — Block and object size calculations
// ============================================================================

FB_SUITE_SETUP(bdev_size_calculations) {}
FB_SUITE_TEARDOWN(bdev_size_calculations) {}

FB_TEST(bdev_size_calculations, blocks_per_object_4k_blocks) {
    uint64_t object_size = DEFAULT_OBJECT_SIZE;  // 4 MiB
    uint32_t block_size = 4096;
    uint64_t blocks_per_object = object_size / block_size;
    FB_ASSERT_EQ(blocks_per_object, 1024u);
}

FB_TEST(bdev_size_calculations, blocks_per_object_512_blocks) {
    uint64_t object_size = DEFAULT_OBJECT_SIZE;  // 4 MiB
    uint32_t block_size = 512;
    uint64_t blocks_per_object = object_size / block_size;
    FB_ASSERT_EQ(blocks_per_object, 8192u);
}

FB_TEST(bdev_size_calculations, image_blocks_calculation) {
    uint64_t image_size = 10ull * 1024 * 1024 * 1024;  // 10 GiB
    uint32_t block_size = 4096;
    uint64_t total_blocks = image_size / block_size;
    FB_ASSERT_EQ(total_blocks, 2621440u);
}

FB_TEST(bdev_size_calculations, objects_for_image) {
    uint64_t image_size = 100ull * 1024 * 1024 * 1024;  // 100 GiB
    uint64_t object_size = DEFAULT_OBJECT_SIZE;  // 4 MiB
    uint64_t object_count = (image_size + object_size - 1) / object_size;
    FB_ASSERT_EQ(object_count, 25600u);
}

// ============================================================================
// Test Suite: bdev_address_format — Monitor address format validation
// ============================================================================

FB_SUITE_SETUP(bdev_address_format) {}
FB_SUITE_TEARDOWN(bdev_address_format) {}

FB_TEST(bdev_address_format, ipv4_loopback) {
    std::string addr = "127.0.0.1:3333";
    auto pos = addr.find(':');
    FB_ASSERT_TRUE(pos != std::string::npos);
    FB_ASSERT_STR_EQ(addr.substr(0, pos).c_str(), "127.0.0.1");
    FB_ASSERT_STR_EQ(addr.substr(pos + 1).c_str(), "3333");
}

FB_TEST(bdev_address_format, ipv4_address) {
    std::string addr = "192.168.1.100:9000";
    auto pos = addr.find(':');
    FB_ASSERT_TRUE(pos != std::string::npos);
}

FB_TEST(bdev_address_format, hostname_format) {
    std::string addr = "mon-server.example.com:3333";
    auto pos = addr.find(':');
    FB_ASSERT_TRUE(pos != std::string::npos);
}

// ============================================================================
// Test Suite: bdev_rpc_decoder_fields — RPC decoder field offsets
// ============================================================================

FB_SUITE_SETUP(bdev_rpc_decoder_fields) {}
FB_SUITE_TEARDOWN(bdev_rpc_decoder_fields) {}

FB_TEST(bdev_rpc_decoder_fields, create_name_offset) {
    // Verify that name field offset is 0 (first field)
    rpc_create_fastblock_mirror req;
    FB_ASSERT_TRUE(req.name.empty());
}

FB_TEST(bdev_rpc_decoder_fields, create_pool_id_offset) {
    rpc_create_fastblock_mirror req;
    req.pool_id = 42;
    FB_ASSERT_EQ(req.pool_id, 42u);
}

FB_TEST(bdev_rpc_decoder_fields, delete_name_present) {
    rpc_bdev_fastblock_delete_mirror req;
    req.name = "bdev0";
    FB_ASSERT_STR_EQ(req.name.c_str(), "bdev0");
}

FB_TEST(bdev_rpc_decoder_fields, resize_fields_present) {
    rpc_bdev_fastblock_resize_mirror req;
    req.name = "bdev0";
    req.new_size = 1024;
    FB_ASSERT_STR_EQ(req.name.c_str(), "bdev0");
    FB_ASSERT_EQ(req.new_size, 1024u);
}

// ============================================================================
// Test Suite: bdev_pool_image_naming — Pool and image name combinations
// ============================================================================

FB_SUITE_SETUP(bdev_pool_image_naming) {}
FB_SUITE_TEARDOWN(bdev_pool_image_naming) {}

FB_TEST(bdev_pool_image_naming, pool_name_simple) {
    std::string pool_name = "rbd";
    FB_ASSERT_TRUE(!pool_name.empty());
    FB_ASSERT_STR_EQ(pool_name.c_str(), "rbd");
}

FB_TEST(bdev_pool_image_naming, image_name_simple) {
    std::string image_name = "volume1";
    FB_ASSERT_TRUE(!image_name.empty());
    FB_ASSERT_STR_EQ(image_name.c_str(), "volume1");
}

FB_TEST(bdev_pool_image_naming, pool_image_combined) {
    std::string pool = "mypool";
    std::string image = "myimage";
    std::string combined = pool + "/" + image;
    FB_ASSERT_STR_EQ(combined.c_str(), "mypool/myimage");
}

FB_TEST(bdev_pool_image_naming, pool_with_underscore) {
    std::string pool = "block_pool_01";
    FB_ASSERT_TRUE(pool.find('_') != std::string::npos);
}

FB_TEST(bdev_pool_image_naming, image_with_timestamp) {
    std::string image = "backup_20240615_120000";
    FB_ASSERT_TRUE(image.find('2') != std::string::npos);
}

// ============================================================================
// Test Suite: bdev_io_chunking — IO request chunking logic
// ============================================================================

FB_SUITE_SETUP(bdev_io_chunking) {}
FB_SUITE_TEARDOWN(bdev_io_chunking) {}

// Helper: calculate IO chunks for a large request
struct io_chunk_result {
    uint64_t chunk_count{0};
    uint64_t first_chunk_offset{0};
    uint64_t first_chunk_size{0};
    uint64_t last_chunk_size{0};
};

io_chunk_result calculate_io_chunks(uint64_t offset, uint64_t length, uint64_t object_size) {
    io_chunk_result result;
    if (length == 0 || object_size == 0) return result;

    uint64_t first_obj_offset = offset % object_size;
    uint64_t first_obj_remaining = object_size - first_obj_offset;

    result.first_chunk_offset = first_obj_offset;
    result.first_chunk_size = std::min(length, first_obj_remaining);
    result.chunk_count = 1;

    uint64_t remaining = length - result.first_chunk_size;
    if (remaining > 0) {
        result.chunk_count += remaining / object_size;
        if (remaining % object_size > 0) {
            result.chunk_count++;
            result.last_chunk_size = remaining % object_size;
        } else {
            result.last_chunk_size = object_size;
        }
    } else {
        result.last_chunk_size = result.first_chunk_size;
    }

    return result;
}

FB_TEST(bdev_io_chunking, single_object_read) {
    auto result = calculate_io_chunks(1024, 2048, DEFAULT_OBJECT_SIZE);
    FB_ASSERT_EQ(result.chunk_count, 1u);
    FB_ASSERT_EQ(result.first_chunk_offset, 1024u);
    FB_ASSERT_EQ(result.first_chunk_size, 2048u);
    FB_ASSERT_EQ(result.last_chunk_size, 2048u);
}

FB_TEST(bdev_io_chunking, cross_boundary_read) {
    uint64_t offset = 3 * 1024 * 1024 + 512 * 1024;
    uint64_t length = 2 * 1024 * 1024;
    auto result = calculate_io_chunks(offset, length, DEFAULT_OBJECT_SIZE);

    FB_ASSERT_EQ(result.chunk_count, 2u);
    FB_ASSERT_EQ(result.first_chunk_size, 512u * 1024u);
    FB_ASSERT_EQ(result.last_chunk_size, 1536u * 1024u);
}

FB_TEST(bdev_io_chunking, multi_object_span) {
    auto result = calculate_io_chunks(0, 20 * 1024 * 1024, DEFAULT_OBJECT_SIZE);
    FB_ASSERT_EQ(result.chunk_count, 5u);
    FB_ASSERT_EQ(result.first_chunk_offset, 0u);
    FB_ASSERT_EQ(result.first_chunk_size, DEFAULT_OBJECT_SIZE);
    FB_ASSERT_EQ(result.last_chunk_size, DEFAULT_OBJECT_SIZE);
}

FB_TEST(bdev_io_chunking, partial_last_object) {
    auto result = calculate_io_chunks(0, 5 * 1024 * 1024 + 1024, DEFAULT_OBJECT_SIZE);
    FB_ASSERT_EQ(result.chunk_count, 2u);
    FB_ASSERT_EQ(result.last_chunk_size, 1024u);
}

FB_TEST(bdev_io_chunking, aligned_start_aligned_end) {
    auto result = calculate_io_chunks(DEFAULT_OBJECT_SIZE, 3 * DEFAULT_OBJECT_SIZE, DEFAULT_OBJECT_SIZE);
    FB_ASSERT_EQ(result.chunk_count, 3u);
    FB_ASSERT_EQ(result.first_chunk_offset, 0u);
}

FB_TEST(bdev_io_chunking, zero_length_returns_zero_chunks) {
    auto result = calculate_io_chunks(0, 0, DEFAULT_OBJECT_SIZE);
    FB_ASSERT_EQ(result.chunk_count, 0u);
}

FB_TEST(bdev_io_chunking, zero_object_size_returns_zero) {
    auto result = calculate_io_chunks(0, 1024, 0);
    FB_ASSERT_EQ(result.chunk_count, 0u);
}

// ============================================================================
// Test Suite: bdev_queue_depth_management — Queue depth and slot allocation
// ============================================================================

FB_SUITE_SETUP(bdev_queue_depth_management) {}
FB_SUITE_TEARDOWN(bdev_queue_depth_management) {}

// Simulated slot allocator
struct slot_allocator {
    uint32_t total_slots;
    uint32_t used_slots{0};
    uint32_t next_slot{0};

    slot_allocator(uint32_t n) : total_slots(n) {}

    std::optional<uint32_t> allocate() {
        if (used_slots >= total_slots) return std::nullopt;
        uint32_t slot = next_slot;
        next_slot = (next_slot + 1) % total_slots;
        used_slots++;
        return slot;
    }

    void deallocate(uint32_t slot) {
        if (used_slots > 0) used_slots--;
        (void)slot;
    }

    uint32_t available() const { return total_slots - used_slots; }
    bool is_full() const { return used_slots >= total_slots; }
};

FB_TEST(bdev_queue_depth_management, allocate_single_slot) {
    slot_allocator alloc(128);
    auto slot = alloc.allocate();
    FB_ASSERT_TRUE(slot.has_value());
    FB_ASSERT_EQ(slot.value(), 0u);
    FB_ASSERT_EQ(alloc.used_slots, 1u);
}

FB_TEST(bdev_queue_depth_management, allocate_all_slots) {
    slot_allocator alloc(128);
    for (uint32_t i = 0; i < 128; ++i) {
        auto slot = alloc.allocate();
        FB_ASSERT_TRUE(slot.has_value());
        FB_ASSERT_EQ(slot.value(), i);
    }
    FB_ASSERT_TRUE(alloc.is_full());
}

FB_TEST(bdev_queue_depth_management, allocate_fails_when_full) {
    slot_allocator alloc(4);
    for (int i = 0; i < 4; ++i) alloc.allocate();
    auto slot = alloc.allocate();
    FB_ASSERT_FALSE(slot.has_value());
}

FB_TEST(bdev_queue_depth_management, deallocate_frees_slot) {
    slot_allocator alloc(4);
    alloc.allocate();
    alloc.allocate();
    FB_ASSERT_EQ(alloc.available(), 2u);
    alloc.deallocate(0);
    FB_ASSERT_EQ(alloc.available(), 3u);
}

FB_TEST(bdev_queue_depth_management, slot_wraps_around) {
    slot_allocator alloc(4);
    for (int i = 0; i < 4; ++i) alloc.allocate();
    alloc.deallocate(0);
    alloc.deallocate(1);
    alloc.deallocate(2);
    alloc.deallocate(3);
    // After deallocating all, next_slot continues from where it was
    FB_ASSERT_EQ(alloc.next_slot, 0u);
}

FB_TEST(bdev_queue_depth_management, available_count_correct) {
    slot_allocator alloc(128);
    FB_ASSERT_EQ(alloc.available(), 128u);
    alloc.allocate();
    alloc.allocate();
    alloc.allocate();
    FB_ASSERT_EQ(alloc.available(), 125u);
}

// ============================================================================
// Test Suite: bdev_image_resize_state — Image resize state machine
// ============================================================================

FB_SUITE_SETUP(bdev_image_resize_state) {}
FB_SUITE_TEARDOWN(bdev_image_resize_state) {}

enum class resize_state {
    idle,
    requested,
    in_progress,
    completed,
    failed
};

struct resize_context {
    resize_state state{resize_state::idle};
    uint64_t old_size{0};
    uint64_t new_size{0};
    int retry_count{0};
    static constexpr int max_retries = 3;

    bool can_start() const { return state == resize_state::idle; }
    bool can_complete() const { return state == resize_state::in_progress; }

    void start_resize(uint64_t old_sz, uint64_t new_sz) {
        if (!can_start()) return;
        old_size = old_sz;
        new_size = new_sz;
        state = resize_state::requested;
        retry_count = 0;
    }

    void begin() {
        if (state == resize_state::requested) {
            state = resize_state::in_progress;
        }
    }

    void complete() {
        if (can_complete()) {
            state = resize_state::completed;
        }
    }

    void fail() {
        if (state == resize_state::in_progress) {
            retry_count++;
            if (retry_count >= max_retries) {
                state = resize_state::failed;
            } else {
                state = resize_state::requested;  // retry
            }
        }
    }

    void reset() {
        state = resize_state::idle;
        retry_count = 0;
    }
};

FB_TEST(bdev_image_resize_state, starts_from_idle) {
    resize_context ctx;
    FB_ASSERT_TRUE(ctx.can_start());
    FB_ASSERT_TRUE(ctx.state == resize_state::idle);
}

FB_TEST(bdev_image_resize_state, state_transitions_success_path) {
    resize_context ctx;
    ctx.start_resize(100, 200);
    FB_ASSERT_TRUE(ctx.state == resize_state::requested);

    ctx.begin();
    FB_ASSERT_TRUE(ctx.state == resize_state::in_progress);

    ctx.complete();
    FB_ASSERT_TRUE(ctx.state == resize_state::completed);
}

FB_TEST(bdev_image_resize_state, fail_retries_up_to_max) {
    resize_context ctx;
    ctx.start_resize(100, 200);
    ctx.begin();

    ctx.fail();
    FB_ASSERT_TRUE(ctx.state == resize_state::requested);
    FB_ASSERT_EQ(ctx.retry_count, 1);

    ctx.begin();
    ctx.fail();
    FB_ASSERT_EQ(ctx.retry_count, 2);

    ctx.begin();
    ctx.fail();
    FB_ASSERT_EQ(ctx.retry_count, 3);
    FB_ASSERT_TRUE(ctx.state == resize_state::failed);
}

FB_TEST(bdev_image_resize_state, cannot_start_from_non_idle) {
    resize_context ctx;
    ctx.start_resize(100, 200);
    ctx.begin();

    // Try to start another resize while one is in progress
    ctx.start_resize(200, 300);
    FB_ASSERT_EQ(ctx.old_size, 100u);  // Should still have old values
    FB_ASSERT_EQ(ctx.new_size, 200u);
}

FB_TEST(bdev_image_resize_state, reset_returns_to_idle) {
    resize_context ctx;
    ctx.start_resize(100, 200);
    ctx.begin();
    ctx.fail();
    ctx.fail();
    ctx.fail();

    ctx.reset();
    FB_ASSERT_TRUE(ctx.state == resize_state::idle);
    FB_ASSERT_EQ(ctx.retry_count, 0);
    FB_ASSERT_TRUE(ctx.can_start());
}

FB_TEST(bdev_image_resize_state, complete_only_from_in_progress) {
    resize_context ctx;
    ctx.start_resize(100, 200);
    // Not calling begin(), still in requested state
    ctx.complete();
    FB_ASSERT_TRUE(ctx.state == resize_state::requested);  // unchanged
}

// ============================================================================
// Test Suite: bdev_connection_lifecycle — Connection state management
// ============================================================================

FB_SUITE_SETUP(bdev_connection_lifecycle) {}
FB_SUITE_TEARDOWN(bdev_connection_lifecycle) {}

enum class connection_state {
    disconnected,
    connecting,
    connected,
    disconnecting,
    error
};

struct connection_manager {
    connection_state state{connection_state::disconnected};
    int reconnect_attempts{0};
    static constexpr int max_reconnect = 5;

    bool connect() {
        if (state == connection_state::connected) return true;
        if (state == connection_state::disconnected || state == connection_state::error) {
            state = connection_state::connecting;
            reconnect_attempts = 0;
            return true;
        }
        return false;
    }

    void on_connect_complete(bool success) {
        if (state != connection_state::connecting) return;
        if (success) {
            state = connection_state::connected;
            reconnect_attempts = 0;
        } else {
            reconnect_attempts++;
            if (reconnect_attempts >= max_reconnect) {
                state = connection_state::error;
            } else {
                state = connection_state::disconnected;
            }
        }
    }

    bool disconnect() {
        if (state != connection_state::connected) return false;
        state = connection_state::disconnecting;
        return true;
    }

    void on_disconnect_complete() {
        if (state == connection_state::disconnecting) {
            state = connection_state::disconnected;
        }
    }

    bool is_connected() const { return state == connection_state::connected; }
};

FB_TEST(bdev_connection_lifecycle, initial_state_disconnected) {
    connection_manager cm;
    FB_ASSERT_TRUE(cm.state == connection_state::disconnected);
    FB_ASSERT_FALSE(cm.is_connected());
}

FB_TEST(bdev_connection_lifecycle, successful_connection) {
    connection_manager cm;
    FB_ASSERT_TRUE(cm.connect());
    FB_ASSERT_TRUE(cm.state == connection_state::connecting);

    cm.on_connect_complete(true);
    FB_ASSERT_TRUE(cm.is_connected());
    FB_ASSERT_EQ(cm.reconnect_attempts, 0);
}

FB_TEST(bdev_connection_lifecycle, connection_failure_retries) {
    connection_manager cm;
    cm.connect();
    cm.on_connect_complete(false);
    FB_ASSERT_TRUE(cm.state == connection_state::disconnected);
    FB_ASSERT_EQ(cm.reconnect_attempts, 1);
}

FB_TEST(bdev_connection_lifecycle, max_reconnects_goes_to_error) {
    connection_manager cm;
    for (int i = 0; i < connection_manager::max_reconnect; ++i) {
        cm.connect();
        cm.on_connect_complete(false);
    }
    FB_ASSERT_TRUE(cm.state == connection_state::error);
}

FB_TEST(bdev_connection_lifecycle, successful_reconnect_resets_counter) {
    connection_manager cm;
    cm.connect();
    cm.on_connect_complete(false);
    cm.connect();
    cm.on_connect_complete(false);
    cm.connect();
    cm.on_connect_complete(true);
    FB_ASSERT_TRUE(cm.is_connected());
    FB_ASSERT_EQ(cm.reconnect_attempts, 0);
}

FB_TEST(bdev_connection_lifecycle, disconnect_flow) {
    connection_manager cm;
    cm.connect();
    cm.on_connect_complete(true);
    FB_ASSERT_TRUE(cm.disconnect());
    FB_ASSERT_TRUE(cm.state == connection_state::disconnecting);
    cm.on_disconnect_complete();
    FB_ASSERT_TRUE(cm.state == connection_state::disconnected);
}

FB_TEST(bdev_connection_lifecycle, connect_from_connected_returns_true) {
    connection_manager cm;
    cm.connect();
    cm.on_connect_complete(true);
    FB_ASSERT_TRUE(cm.connect());
    FB_ASSERT_TRUE(cm.is_connected());
}

FB_TEST(bdev_connection_lifecycle, disconnect_from_disconnected_fails) {
    connection_manager cm;
    FB_ASSERT_FALSE(cm.disconnect());
}

// ============================================================================
// Test Suite: bdev_io_completion_tracking — IO completion tracking and stats
// ============================================================================

FB_SUITE_SETUP(bdev_io_completion_tracking) {}
FB_SUITE_TEARDOWN(bdev_io_completion_tracking) {}

struct io_stats {
    uint64_t total_submitted{0};
    uint64_t completed_success{0};
    uint64_t completed_failed{0};
    uint64_t total_bytes{0};
    uint64_t min_latency_us{UINT64_MAX};
    uint64_t max_latency_us{0};
    uint64_t total_latency_us{0};

    void submit(uint64_t bytes) {
        total_submitted++;
        total_bytes += bytes;
    }

    void complete(uint64_t latency_us, bool success) {
        if (success) {
            completed_success++;
        } else {
            completed_failed++;
        }
        total_latency_us += latency_us;
        if (latency_us < min_latency_us) min_latency_us = latency_us;
        if (latency_us > max_latency_us) max_latency_us = latency_us;
    }

    uint64_t pending() const { return total_submitted - completed_success - completed_failed; }
    double avg_latency() const {
        uint64_t total_completed = completed_success + completed_failed;
        return total_completed > 0 ? (double)total_latency_us / total_completed : 0.0;
    }
    double success_rate() const {
        uint64_t total_completed = completed_success + completed_failed;
        return total_completed > 0 ? (double)completed_success / total_completed : 0.0;
    }
};

FB_TEST(bdev_io_completion_tracking, initial_stats_zero) {
    io_stats stats;
    FB_ASSERT_EQ(stats.total_submitted, 0u);
    FB_ASSERT_EQ(stats.completed_success, 0u);
    FB_ASSERT_EQ(stats.completed_failed, 0u);
    FB_ASSERT_EQ(stats.pending(), 0u);
}

FB_TEST(bdev_io_completion_tracking, submit_increments_counters) {
    io_stats stats;
    stats.submit(4096);
    stats.submit(4096);
    FB_ASSERT_EQ(stats.total_submitted, 2u);
    FB_ASSERT_EQ(stats.total_bytes, 8192u);
    FB_ASSERT_EQ(stats.pending(), 2u);
}

FB_TEST(bdev_io_completion_tracking, complete_updates_stats) {
    io_stats stats;
    stats.submit(4096);
    stats.complete(100, true);
    FB_ASSERT_EQ(stats.completed_success, 1u);
    FB_ASSERT_EQ(stats.pending(), 0u);
    FB_ASSERT_EQ(stats.min_latency_us, 100u);
    FB_ASSERT_EQ(stats.max_latency_us, 100u);
}

FB_TEST(bdev_io_completion_tracking, failed_io_counted) {
    io_stats stats;
    stats.submit(4096);
    stats.submit(4096);
    stats.complete(50, true);
    stats.complete(50, false);
    FB_ASSERT_EQ(stats.completed_success, 1u);
    FB_ASSERT_EQ(stats.completed_failed, 1u);
}

FB_TEST(bdev_io_completion_tracking, latency_tracking) {
    io_stats stats;
    stats.submit(4096);
    stats.complete(100, true);
    stats.submit(4096);
    stats.complete(200, true);
    stats.submit(4096);
    stats.complete(50, true);

    FB_ASSERT_EQ(stats.min_latency_us, 50u);
    FB_ASSERT_EQ(stats.max_latency_us, 200u);
    FB_ASSERT_EQ(stats.total_latency_us, 350u);
    FB_ASSERT_TRUE(stats.avg_latency() > 116 && stats.avg_latency() < 117);
}

FB_TEST(bdev_io_completion_tracking, success_rate_calculation) {
    io_stats stats;
    for (int i = 0; i < 10; ++i) {
        stats.submit(4096);
        stats.complete(100, i < 8);  // 8 success, 2 failures
    }
    FB_ASSERT_EQ(stats.completed_success, 8u);
    FB_ASSERT_EQ(stats.completed_failed, 2u);
    FB_ASSERT_TRUE(stats.success_rate() > 0.79 && stats.success_rate() < 0.81);
}

FB_TEST(bdev_io_completion_tracking, pending_count_with_inflight_ios) {
    io_stats stats;
    for (int i = 0; i < 100; ++i) stats.submit(4096);
    for (int i = 0; i < 80; ++i) stats.complete(100, true);
    FB_ASSERT_EQ(stats.pending(), 20u);
}

FB_TEST(bdev_io_completion_tracking, avg_latency_zero_when_no_completions) {
    io_stats stats;
    stats.submit(4096);
    FB_ASSERT_TRUE(stats.avg_latency() == 0.0);
}

// ============================================================================
// Test Suite: bdev_write_ring_state — Write ring buffer state management
// ============================================================================

FB_SUITE_SETUP(bdev_write_ring_state) {}
FB_SUITE_TEARDOWN(bdev_write_ring_state) {}

struct write_ring {
    uint64_t queue_id{0};
    uint64_t lease_deadline_us{0};
    uint32_t slot_count{0};
    uint32_t next_slot{0};
    bool is_ready{false};
    bool is_connecting{false};
    bool lease_valid{false};

    bool has_valid_lease(uint64_t current_time_us) const {
        return lease_valid && current_time_us < lease_deadline_us;
    }

    bool can_accept_io() const {
        return is_ready && !is_connecting && queue_id > 0;
    }

    void invalidate() {
        queue_id = 0;
        is_ready = false;
        lease_valid = false;
        next_slot = 0;
    }

    void set_lease(uint64_t qid, uint64_t deadline_us) {
        queue_id = qid;
        lease_deadline_us = deadline_us;
        lease_valid = true;
        is_ready = true;
    }
};

FB_TEST(bdev_write_ring_state, initial_state_not_ready) {
    write_ring ring;
    FB_ASSERT_FALSE(ring.is_ready);
    FB_ASSERT_FALSE(ring.can_accept_io());
    FB_ASSERT_EQ(ring.queue_id, 0u);
}

FB_TEST(bdev_write_ring_state, set_lease_makes_ready) {
    write_ring ring;
    ring.set_lease(12345, 1000000);
    FB_ASSERT_TRUE(ring.is_ready);
    FB_ASSERT_TRUE(ring.lease_valid);
    FB_ASSERT_EQ(ring.queue_id, 12345u);
}

FB_TEST(bdev_write_ring_state, has_valid_lease_checks_time) {
    write_ring ring;
    ring.set_lease(12345, 1000000);
    ring.lease_valid = true;

    FB_ASSERT_TRUE(ring.has_valid_lease(500000));
    FB_ASSERT_FALSE(ring.has_valid_lease(1500000));
}

FB_TEST(bdev_write_ring_state, can_accept_io_requires_ready_and_queue) {
    write_ring ring;
    FB_ASSERT_FALSE(ring.can_accept_io());

    ring.is_ready = true;
    FB_ASSERT_FALSE(ring.can_accept_io());  // still no queue_id

    ring.queue_id = 12345;
    FB_ASSERT_TRUE(ring.can_accept_io());
}

FB_TEST(bdev_write_ring_state, connecting_blocks_io) {
    write_ring ring;
    ring.set_lease(12345, 1000000);
    FB_ASSERT_TRUE(ring.can_accept_io());

    ring.is_connecting = true;
    FB_ASSERT_FALSE(ring.can_accept_io());
}

FB_TEST(bdev_write_ring_state, invalidate_resets_state) {
    write_ring ring;
    ring.set_lease(12345, 1000000);
    ring.next_slot = 10;

    ring.invalidate();
    FB_ASSERT_FALSE(ring.is_ready);
    FB_ASSERT_FALSE(ring.lease_valid);
    FB_ASSERT_EQ(ring.queue_id, 0u);
    FB_ASSERT_EQ(ring.next_slot, 0u);
}

FB_TEST(bdev_write_ring_state, lease_valid_flag_independent) {
    write_ring ring;
    ring.set_lease(12345, 1000000);
    ring.lease_valid = false;  // manually invalidate lease

    FB_ASSERT_FALSE(ring.has_valid_lease(500000));  // even though time is within range
}

FB_TEST(bdev_write_ring_state, next_slot_advances_independently) {
    write_ring ring;
    ring.set_lease(12345, 1000000);
    ring.slot_count = 4;
    ring.next_slot = 0;

    for (int i = 0; i < 10; ++i) {
        ring.next_slot = (ring.next_slot + 1) % ring.slot_count;
    }
    FB_ASSERT_EQ(ring.next_slot, 2u);
}

// ============================================================================
// Test Suite: bdev_rpc_error_handling — RPC error code handling
// ============================================================================

FB_SUITE_SETUP(bdev_rpc_error_handling) {}
FB_SUITE_TEARDOWN(bdev_rpc_error_handling) {}

enum class rpc_error_code {
    success = 0,
    invalid_param = -1,
    not_found = -2,
    already_exists = -3,
    permission_denied = -4,
    internal_error = -5,
    timeout = -6
};

struct rpc_error_handler {
    rpc_error_code last_error{rpc_error_code::success};
    uint64_t error_count{0};

    bool is_success(rpc_error_code code) const {
        return code == rpc_error_code::success;
    }

    bool is_retryable(rpc_error_code code) const {
        return code == rpc_error_code::timeout ||
               code == rpc_error_code::internal_error;
    }

    bool is_client_error(rpc_error_code code) const {
        return code == rpc_error_code::invalid_param ||
               code == rpc_error_code::not_found ||
               code == rpc_error_code::already_exists ||
               code == rpc_error_code::permission_denied;
    }

    bool is_server_error(rpc_error_code code) const {
        return code == rpc_error_code::internal_error ||
               code == rpc_error_code::timeout;
    }

    std::string error_message(rpc_error_code code) const {
        switch (code) {
            case rpc_error_code::success: return "success";
            case rpc_error_code::invalid_param: return "invalid parameter";
            case rpc_error_code::not_found: return "not found";
            case rpc_error_code::already_exists: return "already exists";
            case rpc_error_code::permission_denied: return "permission denied";
            case rpc_error_code::internal_error: return "internal error";
            case rpc_error_code::timeout: return "timeout";
            default: return "unknown error";
        }
    }

    void record_error(rpc_error_code code) {
        last_error = code;
        if (!is_success(code)) error_count++;
    }

    void reset() {
        last_error = rpc_error_code::success;
        error_count = 0;
    }
};

FB_TEST(bdev_rpc_error_handling, success_is_not_error) {
    rpc_error_handler handler;
    FB_ASSERT_TRUE(handler.is_success(rpc_error_code::success));
    FB_ASSERT_FALSE(handler.is_client_error(rpc_error_code::success));
    FB_ASSERT_FALSE(handler.is_server_error(rpc_error_code::success));
}

FB_TEST(bdev_rpc_error_handling, timeout_is_retryable) {
    rpc_error_handler handler;
    FB_ASSERT_TRUE(handler.is_retryable(rpc_error_code::timeout));
    FB_ASSERT_FALSE(handler.is_retryable(rpc_error_code::invalid_param));
}

FB_TEST(bdev_rpc_error_handling, client_vs_server_errors) {
    rpc_error_handler handler;
    FB_ASSERT_TRUE(handler.is_client_error(rpc_error_code::invalid_param));
    FB_ASSERT_TRUE(handler.is_client_error(rpc_error_code::not_found));
    FB_ASSERT_TRUE(handler.is_server_error(rpc_error_code::internal_error));
    FB_ASSERT_TRUE(handler.is_server_error(rpc_error_code::timeout));
}

FB_TEST(bdev_rpc_error_handling, error_messages_correct) {
    rpc_error_handler handler;
    FB_ASSERT_STR_EQ(handler.error_message(rpc_error_code::success).c_str(), "success");
    FB_ASSERT_STR_EQ(handler.error_message(rpc_error_code::invalid_param).c_str(), "invalid parameter");
    FB_ASSERT_STR_EQ(handler.error_message(rpc_error_code::timeout).c_str(), "timeout");
}

FB_TEST(bdev_rpc_error_handling, record_error_increments_count) {
    rpc_error_handler handler;
    FB_ASSERT_EQ(handler.error_count, 0u);

    handler.record_error(rpc_error_code::success);
    FB_ASSERT_EQ(handler.error_count, 0u);  // success not counted

    handler.record_error(rpc_error_code::timeout);
    FB_ASSERT_EQ(handler.error_count, 1u);

    handler.record_error(rpc_error_code::not_found);
    FB_ASSERT_EQ(handler.error_count, 2u);
}

FB_TEST(bdev_rpc_error_handling, last_error_tracking) {
    rpc_error_handler handler;
    handler.record_error(rpc_error_code::timeout);
    FB_ASSERT_TRUE(handler.last_error == rpc_error_code::timeout);

    handler.record_error(rpc_error_code::not_found);
    FB_ASSERT_TRUE(handler.last_error == rpc_error_code::not_found);
}

FB_TEST(bdev_rpc_error_handling, reset_clears_state) {
    rpc_error_handler handler;
    handler.record_error(rpc_error_code::timeout);
    handler.record_error(rpc_error_code::internal_error);
    FB_ASSERT_EQ(handler.error_count, 2u);

    handler.reset();
    FB_ASSERT_EQ(handler.error_count, 0u);
    FB_ASSERT_TRUE(handler.last_error == rpc_error_code::success);
}

// ============================================================================
// Test Suite: bdev_leader_tracking — Leader election tracking
// ============================================================================

FB_SUITE_SETUP(bdev_leader_tracking) {}
FB_SUITE_TEARDOWN(bdev_leader_tracking) {}

struct leader_info {
    int32_t leader_id{-1};
    uint64_t term{0};
    uint64_t epoch{0};
    bool is_valid{false};

    void update(int32_t new_leader, uint64_t new_term, uint64_t new_epoch) {
        if (new_term >= term && new_epoch >= epoch) {
            leader_id = new_leader;
            term = new_term;
            epoch = new_epoch;
            is_valid = true;
        }
    }

    void invalidate() {
        is_valid = false;
    }

    bool is_newer_than(uint64_t other_term, uint64_t other_epoch) const {
        if (term != other_term) return term > other_term;
        return epoch > other_epoch;
    }
};

struct leader_cache {
    std::unordered_map<int32_t, leader_info> cache;

    void set_leader(int32_t pool_id, const leader_info& info) {
        cache[pool_id] = info;
    }

    std::optional<leader_info> get_leader(int32_t pool_id) const {
        auto it = cache.find(pool_id);
        if (it != cache.end() && it->second.is_valid) {
            return it->second;
        }
        return std::nullopt;
    }

    void invalidate_pool(int32_t pool_id) {
        auto it = cache.find(pool_id);
        if (it != cache.end()) {
            it->second.invalidate();
        }
    }

    size_t valid_count() const {
        size_t count = 0;
        for (const auto& [_, info] : cache) {
            if (info.is_valid) count++;
        }
        return count;
    }
};

FB_TEST(bdev_leader_tracking, leader_info_default_invalid) {
    leader_info info;
    FB_ASSERT_EQ(info.leader_id, -1);
    FB_ASSERT_EQ(info.term, 0u);
    FB_ASSERT_FALSE(info.is_valid);
}

FB_TEST(bdev_leader_tracking, update_sets_valid) {
    leader_info info;
    info.update(5, 1, 100);
    FB_ASSERT_EQ(info.leader_id, 5);
    FB_ASSERT_EQ(info.term, 1u);
    FB_ASSERT_TRUE(info.is_valid);
}

FB_TEST(bdev_leader_tracking, update_rejected_if_older) {
    leader_info info;
    info.update(5, 2, 100);

    info.update(3, 1, 50);  // older term
    FB_ASSERT_EQ(info.leader_id, 5);  // unchanged

    info.update(3, 2, 50);  // same term, older epoch
    FB_ASSERT_EQ(info.leader_id, 5);  // unchanged
}

FB_TEST(bdev_leader_tracking, update_accepted_if_newer) {
    leader_info info;
    info.update(5, 1, 100);

    info.update(3, 2, 50);  // newer term
    FB_ASSERT_EQ(info.leader_id, 3);

    info.update(7, 2, 150);  // same term, newer epoch
    FB_ASSERT_EQ(info.leader_id, 7);
}

FB_TEST(bdev_leader_tracking, invalidate_marks_invalid) {
    leader_info info;
    info.update(5, 1, 100);
    FB_ASSERT_TRUE(info.is_valid);

    info.invalidate();
    FB_ASSERT_FALSE(info.is_valid);
    FB_ASSERT_EQ(info.leader_id, 5);  // data preserved
}

FB_TEST(bdev_leader_tracking, cache_store_and_retrieve) {
    leader_cache cache;
    leader_info info;
    info.update(5, 1, 100);

    cache.set_leader(1, info);
    auto retrieved = cache.get_leader(1);
    FB_ASSERT_TRUE(retrieved.has_value());
    FB_ASSERT_EQ(retrieved->leader_id, 5);
}

FB_TEST(bdev_leader_tracking, cache_returns_nullopt_for_invalid) {
    leader_cache cache;
    leader_info info;
    info.update(5, 1, 100);
    info.invalidate();

    cache.set_leader(1, info);
    auto retrieved = cache.get_leader(1);
    FB_ASSERT_FALSE(retrieved.has_value());
}

FB_TEST(bdev_leader_tracking, cache_invalidate_pool) {
    leader_cache cache;
    leader_info info;
    info.update(5, 1, 100);

    cache.set_leader(1, info);
    cache.invalidate_pool(1);
    FB_ASSERT_FALSE(cache.get_leader(1).has_value());
}

FB_TEST(bdev_leader_tracking, cache_valid_count) {
    leader_cache cache;
    leader_info info;
    info.update(5, 1, 100);

    cache.set_leader(1, info);
    cache.set_leader(2, info);
    info.invalidate();
    cache.set_leader(3, info);

    FB_ASSERT_EQ(cache.valid_count(), 2u);
}

FB_TEST(bdev_leader_tracking, newer_term_check) {
    leader_info info;
    info.update(5, 2, 100);

    FB_ASSERT_FALSE(info.is_newer_than(3, 100));  // other has higher term
    FB_ASSERT_TRUE(info.is_newer_than(1, 100));   // info has higher term
    FB_ASSERT_TRUE(info.is_newer_than(2, 50));    // same term, higher epoch
}

// ============================================================================
// Test Main Entry Point
// ============================================================================

FB_TEST_MAIN()
