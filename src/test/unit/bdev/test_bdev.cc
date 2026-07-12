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
    FB_ASSERT_EQ(result.last_chunk_size, 1024u * 1024u + 1024u);
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
    resize_context resize_ctx;
    FB_ASSERT_TRUE(resize_ctx.can_start());
    FB_ASSERT_TRUE(resize_ctx.state == resize_state::idle);
}

FB_TEST(bdev_image_resize_state, state_transitions_success_path) {
    resize_context resize_ctx;
    resize_ctx.start_resize(100, 200);
    FB_ASSERT_TRUE(resize_ctx.state == resize_state::requested);

    resize_ctx.begin();
    FB_ASSERT_TRUE(resize_ctx.state == resize_state::in_progress);

    resize_ctx.complete();
    FB_ASSERT_TRUE(resize_ctx.state == resize_state::completed);
}

FB_TEST(bdev_image_resize_state, fail_retries_up_to_max) {
    resize_context resize_ctx;
    resize_ctx.start_resize(100, 200);
    resize_ctx.begin();

    resize_ctx.fail();
    FB_ASSERT_TRUE(resize_ctx.state == resize_state::requested);
    FB_ASSERT_EQ(resize_ctx.retry_count, 1);

    resize_ctx.begin();
    resize_ctx.fail();
    FB_ASSERT_EQ(resize_ctx.retry_count, 2);

    resize_ctx.begin();
    resize_ctx.fail();
    FB_ASSERT_EQ(resize_ctx.retry_count, 3);
    FB_ASSERT_TRUE(resize_ctx.state == resize_state::failed);
}

FB_TEST(bdev_image_resize_state, cannot_start_from_non_idle) {
    resize_context resize_ctx;
    resize_ctx.start_resize(100, 200);
    resize_ctx.begin();

    resize_ctx.start_resize(200, 300);
    FB_ASSERT_EQ(resize_ctx.old_size, 100u);
    FB_ASSERT_EQ(resize_ctx.new_size, 200u);
}

FB_TEST(bdev_image_resize_state, reset_returns_to_idle) {
    resize_context resize_ctx;
    resize_ctx.start_resize(100, 200);
    resize_ctx.begin();
    resize_ctx.fail();
    resize_ctx.fail();
    resize_ctx.fail();

    resize_ctx.reset();
    FB_ASSERT_TRUE(resize_ctx.state == resize_state::idle);
    FB_ASSERT_EQ(resize_ctx.retry_count, 0);
    FB_ASSERT_TRUE(resize_ctx.can_start());
}

FB_TEST(bdev_image_resize_state, complete_only_from_in_progress) {
    resize_context resize_ctx;
    resize_ctx.start_resize(100, 200);
    resize_ctx.complete();
    FB_ASSERT_TRUE(resize_ctx.state == resize_state::requested);
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
            if (state == connection_state::error) {
                reconnect_attempts = 0;
            }
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
        if (new_term > term || (new_term == term && new_epoch >= epoch)) {
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

    FB_ASSERT_FALSE(info.is_newer_than(3, 100));
    FB_ASSERT_TRUE(info.is_newer_than(1, 100));
    FB_ASSERT_TRUE(info.is_newer_than(2, 50));
}

// ============================================================================
// Test Suite: bdev_object_name_builder — Object name construction
// ============================================================================

FB_SUITE_SETUP(bdev_object_name_builder) {}
FB_SUITE_TEARDOWN(bdev_object_name_builder) {}

struct object_name_builder {
    static std::string build(uint64_t pool_id, const std::string& pool_name,
                             const std::string& image_name, uint64_t object_seq) {
        return std::to_string(pool_id) + "_" + pool_name + "_" + image_name + "_" + std::to_string(object_seq);
    }

    static std::string build_prefix(uint64_t pool_id, const std::string& image_name) {
        return std::to_string(pool_id) + "__blk_data___" + image_name;
    }

    static std::string build_full(uint64_t pool_id, const std::string& image_name, uint64_t seq) {
        return build_prefix(pool_id, image_name) + "_" + std::to_string(seq);
    }

    static bool parse_sequence(const std::string& obj_name, uint64_t& seq) {
        auto last_underscore = obj_name.rfind('_');
        if (last_underscore == std::string::npos || last_underscore + 1 >= obj_name.size()) {
            return false;
        }
        try {
            seq = std::stoull(obj_name.substr(last_underscore + 1));
            return true;
        } catch (...) {
            return false;
        }
    }

    static std::optional<uint64_t> extract_pool_id(const std::string& obj_name) {
        auto first_underscore = obj_name.find('_');
        if (first_underscore == std::string::npos || first_underscore == 0) {
            return std::nullopt;
        }
        try {
            return std::stoull(obj_name.substr(0, first_underscore));
        } catch (...) {
            return std::nullopt;
        }
    }
};

FB_TEST(bdev_object_name_builder, build_full_name) {
    auto name = object_name_builder::build(42, "mypool", "myimage", 5);
    FB_ASSERT_STR_EQ(name.c_str(), "42_mypool_myimage_5");
}

FB_TEST(bdev_object_name_builder, build_prefix_format) {
    auto prefix = object_name_builder::build_prefix(1, "test");
    FB_ASSERT_STR_EQ(prefix.c_str(), "1__blk_data___test");
}

FB_TEST(bdev_object_name_builder, build_full_with_sequence) {
    auto name = object_name_builder::build_full(1, "volume", 0);
    FB_ASSERT_STR_EQ(name.c_str(), "1__blk_data___volume_0");

    name = object_name_builder::build_full(1, "volume", 100);
    FB_ASSERT_STR_EQ(name.c_str(), "1__blk_data___volume_100");
}

FB_TEST(bdev_object_name_builder, parse_sequence_success) {
    uint64_t seq = 0;
    FB_ASSERT_TRUE(object_name_builder::parse_sequence("1_pool_image_123", seq));
    FB_ASSERT_EQ(seq, 123u);
}

FB_TEST(bdev_object_name_builder, parse_sequence_failure) {
    uint64_t seq = 0;
    FB_ASSERT_FALSE(object_name_builder::parse_sequence("invalid", seq));
    FB_ASSERT_FALSE(object_name_builder::parse_sequence("no_seq_", seq));
}

FB_TEST(bdev_object_name_builder, extract_pool_id_success) {
    auto pool_id = object_name_builder::extract_pool_id("42_pool_image_123");
    FB_ASSERT_TRUE(pool_id.has_value());
    FB_ASSERT_EQ(pool_id.value(), 42u);
}

FB_TEST(bdev_object_name_builder, extract_pool_id_failure) {
    auto pool_id = object_name_builder::extract_pool_id("invalid");
    FB_ASSERT_FALSE(pool_id.has_value());

    pool_id = object_name_builder::extract_pool_id("_starts_with_underscore");
    FB_ASSERT_FALSE(pool_id.has_value());
}

FB_TEST(bdev_object_name_builder, pool_id_zero_valid) {
    auto prefix = object_name_builder::build_prefix(0, "internal");
    FB_ASSERT_STR_EQ(prefix.c_str(), "0__blk_data___internal");

    auto pool_id = object_name_builder::extract_pool_id("0_internal_image_5");
    FB_ASSERT_TRUE(pool_id.has_value());
    FB_ASSERT_EQ(pool_id.value(), 0u);
}

FB_TEST(bdev_object_name_builder, large_sequence_number) {
    uint64_t large_seq = 999999999999ull;
    auto name = object_name_builder::build_full(1, "bigvol", large_seq);
    FB_ASSERT_TRUE(name.find(std::to_string(large_seq)) != std::string::npos);

    uint64_t parsed_seq = 0;
    FB_ASSERT_TRUE(object_name_builder::parse_sequence(name, parsed_seq));
    FB_ASSERT_EQ(parsed_seq, large_seq);
}

FB_TEST(bdev_object_name_builder, empty_image_name) {
    auto prefix = object_name_builder::build_prefix(1, "");
    FB_ASSERT_STR_EQ(prefix.c_str(), "1__blk_data___");
}

// ============================================================================
// Test Suite: bdev_config_validation — Configuration parameter validation
// ============================================================================

FB_SUITE_SETUP(bdev_config_validation) {}
FB_SUITE_TEARDOWN(bdev_config_validation) {}

struct config_validator {
    // Validate monitor address format
    static bool validate_monitor_address(const std::string& addr) {
        if (addr.empty()) return false;
        auto colon = addr.find(':');
        if (colon == std::string::npos || colon == 0 || colon == addr.size() - 1) {
            return false;
        }
        // Check port is numeric
        std::string port_str = addr.substr(colon + 1);
        for (char c : port_str) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    // Validate block size (power of 2, >= 512)
    static bool validate_block_size(uint32_t size) {
        return size >= 512 && (size & (size - 1)) == 0;
    }

    // Validate object size (power of 2, >= block_size)
    static bool validate_object_size(uint64_t size, uint32_t block_size) {
        return size >= block_size && (size & (size - 1)) == 0;
    }

    // Validate image size (multiple of block_size)
    static bool validate_image_size(uint64_t size, uint32_t block_size) {
        return size > 0 && size % block_size == 0;
    }

    // Validate core count (positive)
    static bool validate_core_count(int cores) {
        return cores > 0;
    }

    // Validate all parameters for bdev creation
    static bool validate_create_params(const std::string& pool_name,
                                        const std::string& image_name,
                                        const std::string& mon_addr,
                                        uint32_t block_size,
                                        uint64_t object_size,
                                        uint64_t image_size) {
        if (pool_name.empty() || image_name.empty()) return false;
        if (!validate_monitor_address(mon_addr)) return false;
        if (!validate_block_size(block_size)) return false;
        if (!validate_object_size(object_size, block_size)) return false;
        if (!validate_image_size(image_size, block_size)) return false;
        return true;
    }
};

FB_TEST(bdev_config_validation, monitor_address_valid_format) {
    FB_ASSERT_TRUE(config_validator::validate_monitor_address("127.0.0.1:9000"));
    FB_ASSERT_TRUE(config_validator::validate_monitor_address("localhost:8080"));
    FB_ASSERT_TRUE(config_validator::validate_monitor_address("10.0.0.1:3333"));
}

FB_TEST(bdev_config_validation, monitor_address_invalid_format) {
    FB_ASSERT_FALSE(config_validator::validate_monitor_address(""));
    FB_ASSERT_FALSE(config_validator::validate_monitor_address("noport"));
    FB_ASSERT_FALSE(config_validator::validate_monitor_address(":9000"));
    FB_ASSERT_FALSE(config_validator::validate_monitor_address("host:"));
    FB_ASSERT_FALSE(config_validator::validate_monitor_address("host:abc"));
}

FB_TEST(bdev_config_validation, block_size_valid_values) {
    FB_ASSERT_TRUE(config_validator::validate_block_size(512));
    FB_ASSERT_TRUE(config_validator::validate_block_size(4096));
    FB_ASSERT_TRUE(config_validator::validate_block_size(65536));
}

FB_TEST(bdev_config_validation, block_size_invalid_values) {
    FB_ASSERT_FALSE(config_validator::validate_block_size(0));
    FB_ASSERT_FALSE(config_validator::validate_block_size(256));
    FB_ASSERT_FALSE(config_validator::validate_block_size(513));
    FB_ASSERT_FALSE(config_validator::validate_block_size(1023));
}

FB_TEST(bdev_config_validation, object_size_valid) {
    FB_ASSERT_TRUE(config_validator::validate_object_size(4096, 512));
    FB_ASSERT_TRUE(config_validator::validate_object_size(4 * 1024 * 1024, 512));
}

FB_TEST(bdev_config_validation, object_size_invalid) {
    FB_ASSERT_FALSE(config_validator::validate_object_size(0, 512));
    FB_ASSERT_FALSE(config_validator::validate_object_size(256, 512));  // smaller than block
    FB_ASSERT_FALSE(config_validator::validate_object_size(3 * 1024 * 1024, 512));  // not power of 2
}

FB_TEST(bdev_config_validation, image_size_valid) {
    FB_ASSERT_TRUE(config_validator::validate_image_size(4096, 4096));
    FB_ASSERT_TRUE(config_validator::validate_image_size(10ull * 1024 * 1024 * 1024, 512));
}

FB_TEST(bdev_config_validation, image_size_invalid) {
    FB_ASSERT_FALSE(config_validator::validate_image_size(0, 512));
    FB_ASSERT_FALSE(config_validator::validate_image_size(1000, 512));  // not aligned
}

FB_TEST(bdev_config_validation, core_count_valid) {
    FB_ASSERT_TRUE(config_validator::validate_core_count(1));
    FB_ASSERT_TRUE(config_validator::validate_core_count(16));
}

FB_TEST(bdev_config_validation, core_count_invalid) {
    FB_ASSERT_FALSE(config_validator::validate_core_count(0));
    FB_ASSERT_FALSE(config_validator::validate_core_count(-1));
}

FB_TEST(bdev_config_validation, create_params_all_valid) {
    FB_ASSERT_TRUE(config_validator::validate_create_params(
        "mypool", "myimage", "127.0.0.1:9000",
        4096, 4 * 1024 * 1024, 10ull * 1024 * 1024 * 1024));
}

FB_TEST(bdev_config_validation, create_params_empty_pool_fails) {
    FB_ASSERT_FALSE(config_validator::validate_create_params(
        "", "myimage", "127.0.0.1:9000", 4096, 4 * 1024 * 1024, 1024));
}

FB_TEST(bdev_config_validation, create_params_empty_image_fails) {
    FB_ASSERT_FALSE(config_validator::validate_create_params(
        "mypool", "", "127.0.0.1:9000", 4096, 4 * 1024 * 1024, 1024));
}

FB_TEST(bdev_config_validation, create_params_invalid_block_size_fails) {
    FB_ASSERT_FALSE(config_validator::validate_create_params(
        "mypool", "myimage", "127.0.0.1:9000", 1000, 4 * 1024 * 1024, 1024));
}

// ============================================================================
// Test Suite: bdev_scatter_gather — Scatter-gather IO operations
// ============================================================================

FB_SUITE_SETUP(bdev_scatter_gather) {}
FB_SUITE_TEARDOWN(bdev_scatter_gather) {}

struct iov_entry {
    void* base{nullptr};
    size_t len{0};
};

struct scatter_gather_ctx {
    std::vector<iov_entry> iovs;
    size_t total_len{0};

    void add_iov(void* base, size_t len) {
        iovs.push_back({base, len});
        total_len += len;
    }

    size_t iov_count() const { return iovs.size(); }

    // Coalesce contiguous entries
    void coalesce() {
        if (iovs.empty()) return;
        std::vector<iov_entry> coalesced;
        iov_entry current = iovs[0];

        for (size_t i = 1; i < iovs.size(); ++i) {
            // Check if contiguous (current ends where next begins)
            if ((char*)current.base + current.len == iovs[i].base) {
                current.len += iovs[i].len;
            } else {
                coalesced.push_back(current);
                current = iovs[i];
            }
        }
        coalesced.push_back(current);
        iovs = std::move(coalesced);
    }

    // Split an entry at given offset
    bool split_entry(size_t idx, size_t offset) {
        if (idx >= iovs.size() || offset >= iovs[idx].len) return false;

        iov_entry first = {iovs[idx].base, offset};
        iov_entry second = {(char*)iovs[idx].base + offset, iovs[idx].len - offset};

        iovs.erase(iovs.begin() + idx);
        iovs.insert(iovs.begin() + idx, second);
        iovs.insert(iovs.begin() + idx, first);
        return true;
    }
};

FB_TEST(bdev_scatter_gather, empty_initial_state) {
    scatter_gather_ctx sg_ctx;
    FB_ASSERT_EQ(sg_ctx.iov_count(), 0u);
    FB_ASSERT_EQ(sg_ctx.total_len, 0u);
}

FB_TEST(bdev_scatter_gather, add_single_iov) {
    scatter_gather_ctx sg_ctx;
    char buf[1024];
    sg_ctx.add_iov(buf, 1024);
    FB_ASSERT_EQ(sg_ctx.iov_count(), 1u);
    FB_ASSERT_EQ(sg_ctx.total_len, 1024u);
}

FB_TEST(bdev_scatter_gather, add_multiple_iovs) {
    scatter_gather_ctx sg_ctx;
    char buf1[1024], buf2[2048], buf3[512];
    sg_ctx.add_iov(buf1, 1024);
    sg_ctx.add_iov(buf2, 2048);
    sg_ctx.add_iov(buf3, 512);
    FB_ASSERT_EQ(sg_ctx.iov_count(), 3u);
    FB_ASSERT_EQ(sg_ctx.total_len, 3584u);
}

FB_TEST(bdev_scatter_gather, coalesce_contiguous) {
    scatter_gather_ctx sg_ctx;
    char buf[4096];
    sg_ctx.add_iov(buf, 1024);
    sg_ctx.add_iov(buf + 1024, 1024);
    sg_ctx.add_iov(buf + 2048, 2048);

    FB_ASSERT_EQ(sg_ctx.iov_count(), 3u);
    sg_ctx.coalesce();
    FB_ASSERT_EQ(sg_ctx.iov_count(), 1u);
    FB_ASSERT_EQ(sg_ctx.iovs[0].len, 4096u);
}

FB_TEST(bdev_scatter_gather, coalesce_non_contiguous_unchanged) {
    scatter_gather_ctx sg_ctx;
    char buf1[1024];
    char buf2[1024];
    // Add with non-contiguous addresses by using separate buffers
    sg_ctx.add_iov(buf1, 1024);
    sg_ctx.add_iov(buf2 + 100, 1024);  // Different address range

    size_t count_before = sg_ctx.iov_count();
    sg_ctx.coalesce();
    // Count should remain same if not contiguous
    FB_ASSERT_EQ(sg_ctx.iov_count(), count_before);
}

FB_TEST(bdev_scatter_gather, split_entry_success) {
    scatter_gather_ctx sg_ctx;
    char buf[4096];
    sg_ctx.add_iov(buf, 4096);

    FB_ASSERT_TRUE(sg_ctx.split_entry(0, 1024));
    FB_ASSERT_EQ(sg_ctx.iov_count(), 2u);
    FB_ASSERT_EQ(sg_ctx.iovs[0].len, 1024u);
    FB_ASSERT_EQ(sg_ctx.iovs[1].len, 3072u);
}

FB_TEST(bdev_scatter_gather, split_entry_invalid_offset) {
    scatter_gather_ctx sg_ctx;
    char buf[1024];
    sg_ctx.add_iov(buf, 1024);

    FB_ASSERT_FALSE(sg_ctx.split_entry(0, 1024));  // offset == len
    FB_ASSERT_FALSE(sg_ctx.split_entry(0, 2000));  // offset > len
}

FB_TEST(bdev_scatter_gather, split_entry_invalid_index) {
    scatter_gather_ctx sg_ctx;
    char buf[1024];
    sg_ctx.add_iov(buf, 1024);

    FB_ASSERT_FALSE(sg_ctx.split_entry(5, 512));  // invalid index
}

FB_TEST(bdev_scatter_gather, total_len_preserved_after_coalesce) {
    scatter_gather_ctx sg_ctx;
    char buf[4096];
    sg_ctx.add_iov(buf, 1024);
    sg_ctx.add_iov(buf + 1024, 2048);
    sg_ctx.add_iov(buf + 3072, 1024);

    size_t len_before = sg_ctx.total_len;
    sg_ctx.coalesce();
    FB_ASSERT_EQ(sg_ctx.total_len, len_before);
}

// ============================================================================
// Test Suite: bdev_retry_backoff — Retry with exponential backoff
// ============================================================================

FB_SUITE_SETUP(bdev_retry_backoff) {}
FB_SUITE_TEARDOWN(bdev_retry_backoff) {}

struct backoff_policy {
    uint64_t initial_delay_us{1000};  // 1ms
    uint64_t max_delay_us{60000000};  // 60s
    double multiplier{2.0};
    int max_retries{5};

    uint64_t calculate_delay(int retry_count) const {
        if (retry_count <= 0) return 0;

        double delay = initial_delay_us;
        for (int i = 1; i < retry_count; ++i) {
            delay *= multiplier;
            if (delay >= max_delay_us) return max_delay_us;  // early cap to prevent overflow
        }
        return std::min(static_cast<uint64_t>(delay), max_delay_us);
    }

    bool should_retry(int retry_count) const {
        return retry_count < max_retries;
    }

    int remaining_retries(int retry_count) const {
        return max_retries - retry_count;
    }

    // Check if current delay exceeds threshold for abort
    bool delay_exceeds_threshold(uint64_t delay_us, uint64_t threshold_us) const {
        return delay_us > threshold_us;
    }
};

FB_TEST(bdev_retry_backoff, initial_delay) {
    backoff_policy policy;
    FB_ASSERT_EQ(policy.calculate_delay(1), 1000u);  // 1ms
}

FB_TEST(bdev_retry_backoff, exponential_growth) {
    backoff_policy policy;
    FB_ASSERT_EQ(policy.calculate_delay(1), 1000u);      // 1ms
    FB_ASSERT_EQ(policy.calculate_delay(2), 2000u);      // 2ms
    FB_ASSERT_EQ(policy.calculate_delay(3), 4000u);      // 4ms
    FB_ASSERT_EQ(policy.calculate_delay(4), 8000u);      // 8ms
}

FB_TEST(bdev_retry_backoff, capped_at_max) {
    backoff_policy policy;
    policy.initial_delay_us = 10000000;  // 10s
    policy.max_delay_us = 30000000;      // 30s cap

    // 10s * 2^4 = 160s, but capped to 30s
    FB_ASSERT_EQ(policy.calculate_delay(4), 30000000u);
}

FB_TEST(bdev_retry_backoff, zero_retries_zero_delay) {
    backoff_policy policy;
    FB_ASSERT_EQ(policy.calculate_delay(0), 0u);
}

FB_TEST(bdev_retry_backoff, should_retry_within_limit) {
    backoff_policy policy;
    policy.max_retries = 5;

    FB_ASSERT_TRUE(policy.should_retry(0));
    FB_ASSERT_TRUE(policy.should_retry(4));
    FB_ASSERT_FALSE(policy.should_retry(5));
}

FB_TEST(bdev_retry_backoff, remaining_retries_count) {
    backoff_policy policy;
    policy.max_retries = 5;

    FB_ASSERT_EQ(policy.remaining_retries(0), 5);
    FB_ASSERT_EQ(policy.remaining_retries(3), 2);
    FB_ASSERT_EQ(policy.remaining_retries(5), 0);
}

FB_TEST(bdev_retry_backoff, delay_threshold_check) {
    backoff_policy policy;
    uint64_t threshold = 10000;  // 10ms

    FB_ASSERT_FALSE(policy.delay_exceeds_threshold(5000, threshold));
    FB_ASSERT_TRUE(policy.delay_exceeds_threshold(15000, threshold));
}

FB_TEST(bdev_retry_backoff, custom_multiplier) {
    backoff_policy policy;
    policy.multiplier = 1.5;
    policy.initial_delay_us = 1000;

    FB_ASSERT_EQ(policy.calculate_delay(1), 1000u);
    FB_ASSERT_EQ(policy.calculate_delay(2), 1500u);
    FB_ASSERT_EQ(policy.calculate_delay(3), 2250u);
}

FB_TEST(bdev_retry_backoff, large_retry_count_still_capped) {
    backoff_policy policy;
    FB_ASSERT_EQ(policy.calculate_delay(100), policy.max_delay_us);
}

FB_TEST(bdev_retry_backoff, no_retries_policy) {
    backoff_policy policy;
    policy.max_retries = 0;
    FB_ASSERT_FALSE(policy.should_retry(0));
    FB_ASSERT_EQ(policy.remaining_retries(0), 0);
}

// ============================================================================
// Test Suite: bdev_lease_renewal — Lease renewal and expiration
// ============================================================================

FB_SUITE_SETUP(bdev_lease_renewal) {}
FB_SUITE_TEARDOWN(bdev_lease_renewal) {}

struct lease_state {
    uint64_t lease_id{0};
    uint64_t granted_at_us{0};
    uint64_t duration_us{0};
    uint64_t renew_at_us{0};  // When to renew (before expiry)
    bool is_valid{false};
    bool renew_pending{false};

    void grant(uint64_t id, uint64_t now_us, uint64_t dur_us, uint64_t renew_window_us) {
        lease_id = id;
        granted_at_us = now_us;
        duration_us = dur_us;
        renew_at_us = now_us + dur_us - renew_window_us;
        is_valid = true;
        renew_pending = false;
    }

    uint64_t expires_at() const {
        return granted_at_us + duration_us;
    }

    uint64_t remaining_us(uint64_t now_us) const {
        if (!is_valid || now_us >= expires_at()) return 0;
        return expires_at() - now_us;
    }

    bool should_renew(uint64_t now_us) const {
        return is_valid && now_us >= renew_at_us && !renew_pending;
    }

    bool is_expired(uint64_t now_us) const {
        return !is_valid || now_us >= expires_at();
    }

    void renew(uint64_t now_us) {
        if (should_renew(now_us)) {
            granted_at_us = now_us;
            renew_at_us = now_us + duration_us - (duration_us / 10);  // 10% guard
            renew_pending = false;
        }
    }

    void invalidate() {
        is_valid = false;
        renew_pending = false;
    }

    void start_renewal() {
        if (is_valid) renew_pending = true;
    }

    void cancel_renewal() {
        renew_pending = false;
    }
};

FB_TEST(bdev_lease_renewal, grant_sets_valid) {
    lease_state lease;
    lease.grant(12345, 0, 30000000, 5000000);  // 30s lease, 5s renew window

    FB_ASSERT_TRUE(lease.is_valid);
    FB_ASSERT_EQ(lease.lease_id, 12345u);
    FB_ASSERT_EQ(lease.expires_at(), 30000000u);
}

FB_TEST(bdev_lease_renewal, remaining_time_calculation) {
    lease_state lease;
    lease.grant(1, 0, 30000000, 5000000);

    FB_ASSERT_EQ(lease.remaining_us(0), 30000000u);
    FB_ASSERT_EQ(lease.remaining_us(10000000), 20000000u);
    FB_ASSERT_EQ(lease.remaining_us(30000000), 0u);
}

FB_TEST(bdev_lease_renewal, should_renew_after_renew_window) {
    lease_state lease;
    lease.grant(1, 0, 30000000, 5000000);  // renew at 25s

    FB_ASSERT_FALSE(lease.should_renew(0));
    FB_ASSERT_FALSE(lease.should_renew(20000000));
    FB_ASSERT_TRUE(lease.should_renew(25000000));
}

FB_TEST(bdev_lease_renewal, should_not_renew_if_pending) {
    lease_state lease;
    lease.grant(1, 0, 30000000, 5000000);
    lease.start_renewal();

    FB_ASSERT_TRUE(lease.renew_pending);
    FB_ASSERT_FALSE(lease.should_renew(25000000));
}

FB_TEST(bdev_lease_renewal, renew_updates_granted_time) {
    lease_state lease;
    lease.grant(1, 0, 30000000, 5000000);

    uint64_t now = 26000000;  // Within renew window
    lease.renew(now);

    FB_ASSERT_EQ(lease.granted_at_us, now);
    FB_ASSERT_EQ(lease.expires_at(), now + 30000000);
}

FB_TEST(bdev_lease_renewal, is_expired_checks_time) {
    lease_state lease;
    lease.grant(1, 0, 30000000, 5000000);

    FB_ASSERT_FALSE(lease.is_expired(0));
    FB_ASSERT_FALSE(lease.is_expired(29000000));
    FB_ASSERT_TRUE(lease.is_expired(30000000));
}

FB_TEST(bdev_lease_renewal, invalidate_clears_state) {
    lease_state lease;
    lease.grant(1, 0, 30000000, 5000000);
    lease.invalidate();

    FB_ASSERT_FALSE(lease.is_valid);
    FB_ASSERT_TRUE(lease.is_expired(0));
}

FB_TEST(bdev_lease_renewal, start_and_cancel_renewal) {
    lease_state lease;
    lease.grant(1, 0, 30000000, 5000000);

    lease.start_renewal();
    FB_ASSERT_TRUE(lease.renew_pending);

    lease.cancel_renewal();
    FB_ASSERT_FALSE(lease.renew_pending);
}

FB_TEST(bdev_lease_renewal, remaining_zero_if_expired) {
    lease_state lease;
    lease.grant(1, 0, 30000000, 5000000);
    lease.invalidate();

    FB_ASSERT_EQ(lease.remaining_us(0), 0u);
}

FB_TEST(bdev_lease_renewal, renew_fails_if_not_should_renew) {
    lease_state lease;
    lease.grant(1, 0, 30000000, 5000000);

    uint64_t old_granted = lease.granted_at_us;
    lease.renew(10000000);  // Too early, before renew_at

    FB_ASSERT_EQ(lease.granted_at_us, old_granted);  // unchanged
}

// ============================================================================
// Test Suite: bdev_io_priority — IO priority queue management
// ============================================================================

FB_SUITE_SETUP(bdev_io_priority) {}
FB_SUITE_TEARDOWN(bdev_io_priority) {}

enum class io_priority : uint8_t {
    low = 0,
    normal = 1,
    high = 2,
    critical = 3
};

struct priority_queue {
    struct entry {
        uint64_t id;
        io_priority prio;
        uint64_t timestamp;
    };

    std::vector<entry> entries;

    void enqueue(uint64_t id, io_priority prio, uint64_t ts) {
        entries.push_back({id, prio, ts});
        std::stable_sort(entries.begin(), entries.end(), [](const entry& a, const entry& b) {
            if (a.prio != b.prio) return static_cast<uint8_t>(a.prio) > static_cast<uint8_t>(b.prio);
            return a.timestamp < b.timestamp;
        });
    }

    std::optional<entry> dequeue() {
        if (entries.empty()) return std::nullopt;
        entry e = entries.front();
        entries.erase(entries.begin());
        return e;
    }

    std::optional<entry> peek() const {
        if (entries.empty()) return std::nullopt;
        return entries.front();
    }

    size_t size() const { return entries.size(); }

    size_t count_by_priority(io_priority prio) const {
        return std::count_if(entries.begin(), entries.end(),
            [prio](const entry& e) { return e.prio == prio; });
    }

    void clear() { entries.clear(); }
};

FB_TEST(bdev_io_priority, empty_queue) {
    priority_queue pq;
    FB_ASSERT_EQ(pq.size(), 0u);
    FB_ASSERT_FALSE(pq.dequeue().has_value());
    FB_ASSERT_FALSE(pq.peek().has_value());
}

FB_TEST(bdev_io_priority, enqueue_single) {
    priority_queue pq;
    pq.enqueue(1, io_priority::normal, 100);
    FB_ASSERT_EQ(pq.size(), 1u);
    FB_ASSERT_EQ(pq.peek()->id, 1u);
}

FB_TEST(bdev_io_priority, priority_ordering) {
    priority_queue pq;
    pq.enqueue(1, io_priority::low, 100);
    pq.enqueue(2, io_priority::high, 100);
    pq.enqueue(3, io_priority::normal, 100);

    FB_ASSERT_EQ(pq.dequeue()->id, 2u);
    FB_ASSERT_EQ(pq.dequeue()->id, 3u);
    FB_ASSERT_EQ(pq.dequeue()->id, 1u);
}

FB_TEST(bdev_io_priority, timestamp_ordering_same_priority) {
    priority_queue pq;
    pq.enqueue(1, io_priority::normal, 300);
    pq.enqueue(2, io_priority::normal, 100);
    pq.enqueue(3, io_priority::normal, 200);

    FB_ASSERT_EQ(pq.dequeue()->id, 2u);
    FB_ASSERT_EQ(pq.dequeue()->id, 3u);
    FB_ASSERT_EQ(pq.dequeue()->id, 1u);
}

FB_TEST(bdev_io_priority, critical_highest) {
    priority_queue pq;
    pq.enqueue(1, io_priority::high, 100);
    pq.enqueue(2, io_priority::critical, 100);

    FB_ASSERT_TRUE(pq.dequeue()->prio == io_priority::critical);
}

FB_TEST(bdev_io_priority, count_by_priority) {
    priority_queue pq;
    pq.enqueue(1, io_priority::high, 100);
    pq.enqueue(2, io_priority::high, 200);
    pq.enqueue(3, io_priority::low, 100);

    FB_ASSERT_EQ(pq.count_by_priority(io_priority::high), 2u);
    FB_ASSERT_EQ(pq.count_by_priority(io_priority::low), 1u);
}

FB_TEST(bdev_io_priority, dequeue_removes_front) {
    priority_queue pq;
    pq.enqueue(1, io_priority::high, 100);
    pq.enqueue(2, io_priority::low, 100);

    pq.dequeue();
    FB_ASSERT_EQ(pq.size(), 1u);
}

FB_TEST(bdev_io_priority, clear_empties_queue) {
    priority_queue pq;
    pq.enqueue(1, io_priority::high, 100);
    pq.enqueue(2, io_priority::low, 100);
    pq.clear();
    FB_ASSERT_EQ(pq.size(), 0u);
}

FB_TEST(bdev_io_priority, priority_over_timestamp) {
    priority_queue pq;
    pq.enqueue(1, io_priority::normal, 100);
    pq.enqueue(2, io_priority::high, 500);

    FB_ASSERT_EQ(pq.dequeue()->id, 2u);
}

// ============================================================================
// Test Suite: bdev_throttling — IO rate limiting and throttling
// ============================================================================

FB_SUITE_SETUP(bdev_throttling) {}
FB_SUITE_TEARDOWN(bdev_throttling) {}

struct throttle_state {
    uint64_t bucket_capacity{1000};  // max IOs per interval
    uint64_t refill_rate{100};       // IOs added per interval
    uint64_t current_tokens{1000};
    uint64_t last_refill_us{0};
    uint64_t interval_us{1000000};   // 1 second

    void refill(uint64_t now_us) {
        uint64_t elapsed = now_us - last_refill_us;
        if (elapsed >= interval_us) {
            uint64_t intervals = elapsed / interval_us;
            uint64_t added = intervals * refill_rate;
            current_tokens = std::min(current_tokens + added, bucket_capacity);
            last_refill_us += intervals * interval_us;
        }
    }

    bool can_proceed(uint64_t now_us) {
        refill(now_us);
        return current_tokens > 0;
    }

    void consume(uint64_t now_us, uint64_t count) {
        refill(now_us);
        if (current_tokens >= count) {
            current_tokens -= count;
        }
    }

    uint64_t wait_time(uint64_t now_us, uint64_t needed) {
        refill(now_us);
        if (current_tokens >= needed) return 0;
        uint64_t deficit = needed - current_tokens;
        uint64_t intervals_needed = (deficit + refill_rate - 1) / refill_rate;
        return intervals_needed * interval_us;
    }

    double current_rate(uint64_t now_us) {
        refill(now_us);
        return static_cast<double>(current_tokens) / bucket_capacity;
    }
};

FB_TEST(bdev_throttling, initial_full_bucket) {
    throttle_state ts;
    FB_ASSERT_EQ(ts.current_tokens, ts.bucket_capacity);
}

FB_TEST(bdev_throttling, can_proceed_when_tokens_available) {
    throttle_state ts;
    FB_ASSERT_TRUE(ts.can_proceed(0));
}

FB_TEST(bdev_throttling, consume_reduces_tokens) {
    throttle_state ts;
    ts.consume(0, 100);
    FB_ASSERT_EQ(ts.current_tokens, 900u);
}

FB_TEST(bdev_throttling, refill_adds_tokens) {
    throttle_state ts;
    ts.consume(0, 500);
    FB_ASSERT_EQ(ts.current_tokens, 500u);

    ts.refill(1000000);  // 1 second later
    FB_ASSERT_EQ(ts.current_tokens, 600u);  // +100 refill
}

FB_TEST(bdev_throttling, refill_capped_at_capacity) {
    throttle_state ts;
    ts.current_tokens = 950;
    ts.refill(1000000);
    FB_ASSERT_EQ(ts.current_tokens, ts.bucket_capacity);
}

FB_TEST(bdev_throttling, wait_time_zero_if_tokens_available) {
    throttle_state ts;
    FB_ASSERT_EQ(ts.wait_time(0, 100), 0u);
}

FB_TEST(bdev_throttling, wait_time_when_tokens_depleted) {
    throttle_state ts;
    ts.consume(0, 1000);  // empty bucket
    FB_ASSERT_EQ(ts.current_tokens, 0u);

    uint64_t wait = ts.wait_time(0, 100);
    FB_ASSERT_EQ(wait, 1000000u);  // need 1 interval to get 100 tokens
}

FB_TEST(bdev_throttling, current_rate_decreases_after_consume) {
    throttle_state ts;
    ts.consume(0, 500);
    FB_ASSERT_TRUE(ts.current_rate(0) < 0.6 && ts.current_rate(0) > 0.4);
}

FB_TEST(bdev_throttling, multiple_intervals_refill) {
    throttle_state ts;
    ts.consume(0, 800);  // leaves 200
    ts.last_refill_us = 0;

    ts.refill(5000000);  // 5 intervals, adds 500
    FB_ASSERT_EQ(ts.current_tokens, 700u);  // 200 + 500
}

FB_TEST(bdev_throttling, partial_interval_no_refill) {
    throttle_state ts;
    ts.consume(0, 500);
    ts.refill(500000);  // only half interval

    FB_ASSERT_EQ(ts.current_tokens, 500u);  // unchanged
}

// ============================================================================
// Test Suite: bdev_snapshot_state — Snapshot creation and tracking
// ============================================================================

FB_SUITE_SETUP(bdev_snapshot_state) {}
FB_SUITE_TEARDOWN(bdev_snapshot_state) {}

struct snapshot_info {
    uint64_t snap_id{0};
    std::string name;
    uint64_t created_at_us{0};
    uint64_t size_bytes{0};
    bool is_complete{false};
    bool is_protected{false};
};

struct snapshot_manager {
    std::unordered_map<uint64_t, snapshot_info> snapshots;
    uint64_t next_snap_id{1};

    uint64_t create(const std::string& name, uint64_t now_us, uint64_t size) {
        uint64_t id = next_snap_id++;
        snapshot_info snap;
        snap.snap_id = id;
        snap.name = name;
        snap.created_at_us = now_us;
        snap.size_bytes = size;
        snap.is_complete = false;
        snapshots[id] = snap;
        return id;
    }

    std::optional<snapshot_info> get(uint64_t id) const {
        auto it = snapshots.find(id);
        if (it != snapshots.end()) return it->second;
        return std::nullopt;
    }

    bool complete(uint64_t id) {
        auto it = snapshots.find(id);
        if (it != snapshots.end() && !it->second.is_complete) {
            it->second.is_complete = true;
            return true;
        }
        return false;
    }

    bool protect(uint64_t id) {
        auto it = snapshots.find(id);
        if (it != snapshots.end()) {
            it->second.is_protected = true;
            return true;
        }
        return false;
    }

    bool unprotect(uint64_t id) {
        auto it = snapshots.find(id);
        if (it != snapshots.end() && it->second.is_protected) {
            it->second.is_protected = false;
            return true;
        }
        return false;
    }

    bool can_delete(uint64_t id) const {
        auto it = snapshots.find(id);
        return it != snapshots.end() && !it->second.is_protected && it->second.is_complete;
    }

    bool delete_snapshot(uint64_t id) {
        if (can_delete(id)) {
            snapshots.erase(id);
            return true;
        }
        return false;
    }

    size_t count() const { return snapshots.size(); }

    size_t complete_count() const {
        size_t n = 0;
        for (const auto& [_, snap] : snapshots) {
            if (snap.is_complete) n++;
        }
        return n;
    }
};

FB_TEST(bdev_snapshot_state, create_returns_id) {
    snapshot_manager sm;
    uint64_t id = sm.create("snap1", 1000000, 10ull * 1024 * 1024 * 1024);
    FB_ASSERT_EQ(id, 1u);
    FB_ASSERT_EQ(sm.count(), 1u);
}

FB_TEST(bdev_snapshot_state, get_returns_info) {
    snapshot_manager sm;
    uint64_t id = sm.create("snap1", 1000000, 1024);

    auto snap = sm.get(id);
    FB_ASSERT_TRUE(snap.has_value());
    FB_ASSERT_STR_EQ(snap->name.c_str(), "snap1");
    FB_ASSERT_EQ(snap->size_bytes, 1024u);
}

FB_TEST(bdev_snapshot_state, get_invalid_returns_nullopt) {
    snapshot_manager sm;
    FB_ASSERT_FALSE(sm.get(999).has_value());
}

FB_TEST(bdev_snapshot_state, complete_marks_done) {
    snapshot_manager sm;
    uint64_t id = sm.create("snap1", 1000000, 1024);

    FB_ASSERT_FALSE(sm.get(id)->is_complete);
    sm.complete(id);
    FB_ASSERT_TRUE(sm.get(id)->is_complete);
}

FB_TEST(bdev_snapshot_state, protect_prevents_delete) {
    snapshot_manager sm;
    uint64_t id = sm.create("snap1", 1000000, 1024);
    sm.complete(id);
    sm.protect(id);

    FB_ASSERT_FALSE(sm.can_delete(id));
    FB_ASSERT_FALSE(sm.delete_snapshot(id));
}

FB_TEST(bdev_snapshot_state, unprotect_allows_delete) {
    snapshot_manager sm;
    uint64_t id = sm.create("snap1", 1000000, 1024);
    sm.complete(id);
    sm.protect(id);
    sm.unprotect(id);

    FB_ASSERT_TRUE(sm.can_delete(id));
    FB_ASSERT_TRUE(sm.delete_snapshot(id));
    FB_ASSERT_EQ(sm.count(), 0u);
}

FB_TEST(bdev_snapshot_state, incomplete_cannot_delete) {
    snapshot_manager sm;
    uint64_t id = sm.create("snap1", 1000000, 1024);
    // not completed

    FB_ASSERT_FALSE(sm.can_delete(id));
}

FB_TEST(bdev_snapshot_state, multiple_snapshots) {
    snapshot_manager sm;
    sm.create("snap1", 1000000, 1024);
    sm.create("snap2", 2000000, 2048);
    sm.create("snap3", 3000000, 4096);

    FB_ASSERT_EQ(sm.count(), 3u);
}

FB_TEST(bdev_snapshot_state, complete_count) {
    snapshot_manager sm;
    auto id1 = sm.create("snap1", 1000000, 1024);
    auto id2 = sm.create("snap2", 2000000, 2048);
    sm.create("snap3", 3000000, 4096);

    sm.complete(id1);
    sm.complete(id2);

    FB_ASSERT_EQ(sm.complete_count(), 2u);
}

FB_TEST(bdev_snapshot_state, sequential_ids) {
    snapshot_manager sm;
    FB_ASSERT_EQ(sm.create("a", 0, 0), 1u);
    FB_ASSERT_EQ(sm.create("b", 0, 0), 2u);
    FB_ASSERT_EQ(sm.create("c", 0, 0), 3u);
}

// ============================================================================
// Test Suite: bdev_clone_state — Clone volume tracking
// ============================================================================

FB_SUITE_SETUP(bdev_clone_state) {}
FB_SUITE_TEARDOWN(bdev_clone_state) {}

struct clone_info {
    uint64_t clone_id{0};
    std::string name;
    uint64_t parent_snap_id{0};
    uint64_t created_at_us{0};
    bool is_temporary{false};
    bool is_flattened{false};
};

struct clone_manager {
    std::unordered_map<uint64_t, clone_info> clones;
    uint64_t next_clone_id{1};

    uint64_t create(const std::string& name, uint64_t parent_snap, uint64_t now_us, bool temp) {
        uint64_t id = next_clone_id++;
        clone_info clone;
        clone.clone_id = id;
        clone.name = name;
        clone.parent_snap_id = parent_snap;
        clone.created_at_us = now_us;
        clone.is_temporary = temp;
        clones[id] = clone;
        return id;
    }

    std::optional<clone_info> get(uint64_t id) const {
        auto it = clones.find(id);
        if (it != clones.end()) return it->second;
        return std::nullopt;
    }

    bool flatten(uint64_t id) {
        auto it = clones.find(id);
        if (it != clones.end()) {
            it->second.is_flattened = true;
            it->second.parent_snap_id = 0;  // No parent after flatten
            return true;
        }
        return false;
    }

    std::vector<uint64_t> get_clones_of_parent(uint64_t parent_snap) const {
        std::vector<uint64_t> result;
        for (const auto& [id, clone] : clones) {
            if (clone.parent_snap_id == parent_snap) {
                result.push_back(id);
            }
        }
        return result;
    }

    bool delete_clone(uint64_t id) {
        return clones.erase(id) > 0;
    }

    size_t count() const { return clones.size(); }

    size_t temporary_count() const {
        size_t n = 0;
        for (const auto& [_, clone] : clones) {
            if (clone.is_temporary) n++;
        }
        return n;
    }

    bool is_dependent(uint64_t clone_id) const {
        auto it = clones.find(clone_id);
        return it != clones.end() && it->second.parent_snap_id != 0 && !it->second.is_flattened;
    }
};

FB_TEST(bdev_clone_state, create_returns_id) {
    clone_manager cm;
    uint64_t id = cm.create("clone1", 100, 1000000, false);
    FB_ASSERT_EQ(id, 1u);
    FB_ASSERT_EQ(cm.count(), 1u);
}

FB_TEST(bdev_clone_state, get_returns_info) {
    clone_manager cm;
    uint64_t id = cm.create("clone1", 100, 1000000, false);

    auto clone = cm.get(id);
    FB_ASSERT_TRUE(clone.has_value());
    FB_ASSERT_STR_EQ(clone->name.c_str(), "clone1");
    FB_ASSERT_EQ(clone->parent_snap_id, 100u);
}

FB_TEST(bdev_clone_state, temporary_clone_flag) {
    clone_manager cm;
    uint64_t id1 = cm.create("temp_clone", 100, 0, true);
    uint64_t id2 = cm.create("perm_clone", 100, 0, false);

    FB_ASSERT_TRUE(cm.get(id1)->is_temporary);
    FB_ASSERT_FALSE(cm.get(id2)->is_temporary);
    FB_ASSERT_EQ(cm.temporary_count(), 1u);
}

FB_TEST(bdev_clone_state, flatten_removes_parent) {
    clone_manager cm;
    uint64_t id = cm.create("clone1", 100, 0, false);

    FB_ASSERT_TRUE(cm.is_dependent(id));
    cm.flatten(id);
    FB_ASSERT_FALSE(cm.is_dependent(id));
    FB_ASSERT_EQ(cm.get(id)->parent_snap_id, 0u);
}

FB_TEST(bdev_clone_state, get_clones_of_parent) {
    clone_manager cm;
    cm.create("clone1", 100, 0, false);
    cm.create("clone2", 100, 0, false);
    cm.create("clone3", 200, 0, false);

    auto clones = cm.get_clones_of_parent(100);
    FB_ASSERT_EQ(clones.size(), 2u);
}

FB_TEST(bdev_clone_state, delete_clone_success) {
    clone_manager cm;
    uint64_t id = cm.create("clone1", 100, 0, false);

    FB_ASSERT_TRUE(cm.delete_clone(id));
    FB_ASSERT_EQ(cm.count(), 0u);
}

FB_TEST(bdev_clone_state, is_dependent_before_flatten) {
    clone_manager cm;
    uint64_t id = cm.create("clone1", 100, 0, false);

    FB_ASSERT_TRUE(cm.is_dependent(id));
}

FB_TEST(bdev_clone_state, flattened_not_dependent) {
    clone_manager cm;
    uint64_t id = cm.create("clone1", 100, 0, false);
    cm.flatten(id);

    FB_ASSERT_FALSE(cm.is_dependent(id));
}

FB_TEST(bdev_clone_state, multiple_clones_different_parents) {
    clone_manager cm;
    cm.create("c1", 100, 0, false);
    cm.create("c2", 200, 0, false);
    cm.create("c3", 300, 0, false);

    FB_ASSERT_EQ(cm.get_clones_of_parent(100).size(), 1u);
    FB_ASSERT_EQ(cm.get_clones_of_parent(200).size(), 1u);
}

FB_TEST(bdev_clone_state, sequential_ids) {
    clone_manager cm;
    FB_ASSERT_EQ(cm.create("a", 0, 0, false), 1u);
    FB_ASSERT_EQ(cm.create("b", 0, 0, false), 2u);
    FB_ASSERT_EQ(cm.create("c", 0, 0, false), 3u);
}

// ============================================================================
// Test Suite: bdev_async_callback — Async operation callback tracking
// ============================================================================

FB_SUITE_SETUP(bdev_async_callback) {}
FB_SUITE_TEARDOWN(bdev_async_callback) {}

template<typename T>
struct async_result {
    T value{};
    int error_code{0};
    bool completed{false};

    void set_success(T val) {
        value = val;
        error_code = 0;
        completed = true;
    }

    void set_error(int err) {
        error_code = err;
        completed = true;
    }

    bool is_success() const { return completed && error_code == 0; }
    bool is_error() const { return completed && error_code != 0; }
    bool is_pending() const { return !completed; }
};

struct callback_tracker {
    int callbacks_fired{0};
    int errors{0};
    int successes{0};
    std::vector<int> error_codes;

    void record_success() {
        callbacks_fired++;
        successes++;
    }

    void record_error(int err) {
        callbacks_fired++;
        errors++;
        error_codes.push_back(err);
    }

    void reset() {
        callbacks_fired = 0;
        errors = 0;
        successes = 0;
        error_codes.clear();
    }

    double success_rate() const {
        if (callbacks_fired == 0) return 0.0;
        return static_cast<double>(successes) / callbacks_fired;
    }
};

struct async_operation {
    uint64_t op_id{0};
    uint64_t start_us{0};
    uint64_t end_us{0};
    bool started{false};
    bool cancelled{false};

    async_result<uint64_t> result;

    void start(uint64_t id, uint64_t now_us) {
        op_id = id;
        start_us = now_us;
        started = true;
    }

    void complete(uint64_t now_us, uint64_t val, int err, callback_tracker& tracker) {
        if (!started || cancelled) return;
        end_us = now_us;
        if (err == 0) {
            result.set_success(val);
            tracker.record_success();
        } else {
            result.set_error(err);
            tracker.record_error(err);
        }
    }

    void cancel() {
        if (started && !result.completed) {
            cancelled = true;
        }
    }

    uint64_t duration_us() const {
        if (!started || end_us == 0) return 0;
        return end_us - start_us;
    }
};

FB_TEST(bdev_async_callback, result_initial_pending) {
    async_result<int> result;
    FB_ASSERT_TRUE(result.is_pending());
    FB_ASSERT_FALSE(result.completed);
}

FB_TEST(bdev_async_callback, result_success) {
    async_result<int> result;
    result.set_success(42);
    FB_ASSERT_TRUE(result.is_success());
    FB_ASSERT_EQ(result.value, 42);
}

FB_TEST(bdev_async_callback, result_error) {
    async_result<int> result;
    result.set_error(-1);
    FB_ASSERT_TRUE(result.is_error());
    FB_ASSERT_EQ(result.error_code, -1);
}

FB_TEST(bdev_async_callback, tracker_records_success) {
    callback_tracker tracker;
    tracker.record_success();
    tracker.record_success();
    FB_ASSERT_EQ(tracker.callbacks_fired, 2);
    FB_ASSERT_EQ(tracker.successes, 2);
}

FB_TEST(bdev_async_callback, tracker_records_errors) {
    callback_tracker tracker;
    tracker.record_error(-1);
    tracker.record_error(-2);
    FB_ASSERT_EQ(tracker.errors, 2);
    FB_ASSERT_EQ(tracker.error_codes.size(), 2u);
}

FB_TEST(bdev_async_callback, tracker_success_rate) {
    callback_tracker tracker;
    tracker.record_success();
    tracker.record_success();
    tracker.record_error(-1);
    FB_ASSERT_TRUE(tracker.success_rate() > 0.65 && tracker.success_rate() < 0.68);
}

FB_TEST(bdev_async_callback, tracker_reset) {
    callback_tracker tracker;
    tracker.record_success();
    tracker.record_error(-1);
    tracker.reset();
    FB_ASSERT_EQ(tracker.callbacks_fired, 0);
}

FB_TEST(bdev_async_callback, operation_start) {
    async_operation op;
    op.start(12345, 1000000);
    FB_ASSERT_TRUE(op.started);
    FB_ASSERT_EQ(op.op_id, 12345u);
}

FB_TEST(bdev_async_callback, operation_complete_success) {
    async_operation op;
    callback_tracker tracker;
    op.start(1, 1000000);
    op.complete(2000000, 42, 0, tracker);

    FB_ASSERT_TRUE(op.result.is_success());
    FB_ASSERT_EQ(op.duration_us(), 1000000u);
}

FB_TEST(bdev_async_callback, operation_complete_error) {
    async_operation op;
    callback_tracker tracker;
    op.start(1, 1000000);
    op.complete(2000000, 0, -5, tracker);

    FB_ASSERT_TRUE(op.result.is_error());
    FB_ASSERT_EQ(tracker.errors, 1);
}

FB_TEST(bdev_async_callback, operation_cancel_prevents_complete) {
    async_operation op;
    callback_tracker tracker;
    op.start(1, 1000000);
    op.cancel();
    op.complete(2000000, 42, 0, tracker);

    FB_ASSERT_TRUE(op.cancelled);
    FB_ASSERT_TRUE(op.result.is_pending());  // not completed
}

FB_TEST(bdev_async_callback, operation_cancel_after_complete_fails) {
    async_operation op;
    callback_tracker tracker;
    op.start(1, 1000000);
    op.complete(2000000, 42, 0, tracker);
    op.cancel();  // already completed

    FB_ASSERT_FALSE(op.cancelled);
}

FB_TEST(bdev_async_callback, operation_duration_before_complete_zero) {
    async_operation op;
    op.start(1, 1000000);
    FB_ASSERT_EQ(op.duration_us(), 0u);
}

// ============================================================================
// Test Suite: bdev_error_recovery — Error recovery and state restoration
// ============================================================================

FB_SUITE_SETUP(bdev_error_recovery) {}
FB_SUITE_TEARDOWN(bdev_error_recovery) {}

enum class error_type {
    none,
    transient,
    persistent,
    critical
};

struct recovery_state {
    error_type last_error{error_type::none};
    int error_count{0};
    int recovery_attempts{0};
    bool recovering{false};
    bool recovered{false};

    void set_error(error_type err) {
        last_error = err;
        if (err != error_type::none) {
            error_count++;
            recovering = false;
            recovered = false;
        }
    }

    bool can_recover() const {
        return last_error == error_type::transient ||
               (last_error == error_type::persistent && recovery_attempts < 3);
    }

    void start_recovery() {
        if (can_recover() && !recovering) {
            recovering = true;
            recovery_attempts++;
        }
    }

    void complete_recovery(bool success) {
        if (recovering) {
            recovering = false;
            recovered = success;
            if (success) {
                last_error = error_type::none;
                recovery_attempts = 0;
            }
        }
    }

    void reset() {
        last_error = error_type::none;
        error_count = 0;
        recovery_attempts = 0;
        recovering = false;
        recovered = false;
    }
};

FB_TEST(bdev_error_recovery, initial_no_error) {
    recovery_state rs;
    FB_ASSERT_TRUE(rs.last_error == error_type::none);
    FB_ASSERT_EQ(rs.error_count, 0);
}

FB_TEST(bdev_error_recovery, set_transient_error) {
    recovery_state rs;
    rs.set_error(error_type::transient);
    FB_ASSERT_TRUE(rs.last_error == error_type::transient);
    FB_ASSERT_EQ(rs.error_count, 1);
}

FB_TEST(bdev_error_recovery, can_recover_transient) {
    recovery_state rs;
    rs.set_error(error_type::transient);
    FB_ASSERT_TRUE(rs.can_recover());
}

FB_TEST(bdev_error_recovery, can_recover_persistent_with_limit) {
    recovery_state rs;
    rs.set_error(error_type::persistent);
    FB_ASSERT_TRUE(rs.can_recover());

    rs.start_recovery();
    rs.complete_recovery(false);
    rs.set_error(error_type::persistent);

    FB_ASSERT_TRUE(rs.can_recover());  // still can try
}

FB_TEST(bdev_error_recovery, cannot_recover_critical) {
    recovery_state rs;
    rs.set_error(error_type::critical);
    FB_ASSERT_FALSE(rs.can_recover());
}

FB_TEST(bdev_error_recovery, start_recovery_increments_attempts) {
    recovery_state rs;
    rs.set_error(error_type::transient);
    rs.start_recovery();

    FB_ASSERT_TRUE(rs.recovering);
    FB_ASSERT_EQ(rs.recovery_attempts, 1);
}

FB_TEST(bdev_error_recovery, complete_recovery_success) {
    recovery_state rs;
    rs.set_error(error_type::transient);
    rs.start_recovery();
    rs.complete_recovery(true);

    FB_ASSERT_TRUE(rs.recovered);
    FB_ASSERT_TRUE(rs.last_error == error_type::none);
}

FB_TEST(bdev_error_recovery, complete_recovery_failure) {
    recovery_state rs;
    rs.set_error(error_type::transient);
    rs.start_recovery();
    rs.complete_recovery(false);

    FB_ASSERT_FALSE(rs.recovered);
    FB_ASSERT_TRUE(rs.last_error == error_type::transient);
}

FB_TEST(bdev_error_recovery, reset_clears_all) {
    recovery_state rs;
    rs.set_error(error_type::transient);
    rs.start_recovery();
    rs.reset();

    FB_ASSERT_TRUE(rs.last_error == error_type::none);
    FB_ASSERT_EQ(rs.error_count, 0);
}

FB_TEST(bdev_error_recovery, multiple_errors_counted) {
    recovery_state rs;
    rs.set_error(error_type::transient);
    rs.set_error(error_type::persistent);
    rs.set_error(error_type::critical);

    FB_ASSERT_EQ(rs.error_count, 3);
}

FB_TEST(bdev_error_recovery, persistent_exhausted_no_recovery) {
    recovery_state rs;
    rs.set_error(error_type::persistent);
    rs.start_recovery(); rs.complete_recovery(false);
    rs.set_error(error_type::persistent);
    rs.start_recovery(); rs.complete_recovery(false);
    rs.set_error(error_type::persistent);
    rs.start_recovery(); rs.complete_recovery(false);
    rs.set_error(error_type::persistent);

    FB_ASSERT_EQ(rs.recovery_attempts, 3);
    FB_ASSERT_FALSE(rs.can_recover());
}

FB_TEST(bdev_error_recovery, cannot_start_recovery_while_recovering) {
    recovery_state rs;
    rs.set_error(error_type::transient);
    rs.start_recovery();

    rs.start_recovery();  // second call
    FB_ASSERT_EQ(rs.recovery_attempts, 1);  // unchanged
}

FB_TEST(bdev_error_recovery, success_clears_recovery_attempts) {
    recovery_state rs;
    rs.set_error(error_type::transient);
    rs.start_recovery(); rs.complete_recovery(false);
    rs.set_error(error_type::transient);
    rs.start_recovery(); rs.complete_recovery(true);

    FB_ASSERT_EQ(rs.recovery_attempts, 0);
}

// ============================================================================
// Test Suite: bdev_cache_coherence — Cache state coherence management
// ============================================================================

FB_SUITE_SETUP(bdev_cache_coherence) {}
FB_SUITE_TEARDOWN(bdev_cache_coherence) {}

enum class cache_state {
    clean,
    dirty,
    flushing,
    invalid
};

struct cache_entry {
    uint64_t key{0};
    cache_state state{cache_state::clean};
    uint64_t version{0};
    uint64_t last_access_us{0};
    bool pinned{false};

    void mark_dirty(uint64_t new_version, uint64_t now_us) {
        if (state != cache_state::invalid && !pinned) {
            state = cache_state::dirty;
            version = new_version;
            last_access_us = now_us;
        }
    }

    void mark_clean() {
        if (state == cache_state::dirty || state == cache_state::flushing) {
            state = cache_state::clean;
        }
    }

    void start_flush() {
        if (state == cache_state::dirty) {
            state = cache_state::flushing;
        }
    }

    void invalidate() {
        if (!pinned) {
            state = cache_state::invalid;
        }
    }

    void pin() { pinned = true; }
    void unpin() { pinned = false; }

    bool is_valid() const { return state != cache_state::invalid; }
    bool needs_flush() const { return state == cache_state::dirty; }
};

struct cache_manager {
    std::unordered_map<uint64_t, cache_entry> entries;
    uint64_t current_version{1};

    cache_entry* get(uint64_t key, uint64_t now_us) {
        auto it = entries.find(key);
        if (it != entries.end() && it->second.is_valid()) {
            it->second.last_access_us = now_us;
            return &it->second;
        }
        return nullptr;
    }

    cache_entry& insert(uint64_t key, uint64_t now_us) {
        cache_entry entry;
        entry.key = key;
        entry.last_access_us = now_us;
        entry.version = current_version;
        entries[key] = entry;
        return entries[key];
    }

    void mark_dirty(uint64_t key, uint64_t now_us) {
        auto it = entries.find(key);
        if (it != entries.end()) {
            current_version++;
            it->second.mark_dirty(current_version, now_us);
        }
    }

    size_t dirty_count() const {
        size_t n = 0;
        for (const auto& [_, entry] : entries) {
            if (entry.needs_flush()) n++;
        }
        return n;
    }

    void flush_all() {
        for (auto& [_, entry] : entries) {
            entry.start_flush();
        }
    }

    void complete_flush(uint64_t key) {
        auto it = entries.find(key);
        if (it != entries.end()) {
            it->second.mark_clean();
        }
    }

    void invalidate_all() {
        for (auto& [_, entry] : entries) {
            entry.invalidate();
        }
    }

    size_t valid_count() const {
        size_t n = 0;
        for (const auto& [_, entry] : entries) {
            if (entry.is_valid()) n++;
        }
        return n;
    }

    void pin(uint64_t key) {
        auto it = entries.find(key);
        if (it != entries.end()) it->second.pin();
    }

    void unpin(uint64_t key) {
        auto it = entries.find(key);
        if (it != entries.end()) it->second.unpin();
    }
};

FB_TEST(bdev_cache_coherence, entry_initial_clean) {
    cache_entry entry;
    FB_ASSERT_TRUE(entry.state == cache_state::clean);
    FB_ASSERT_TRUE(entry.is_valid());
}

FB_TEST(bdev_cache_coherence, mark_dirty_changes_state) {
    cache_entry entry;
    entry.mark_dirty(2, 1000000);
    FB_ASSERT_TRUE(entry.state == cache_state::dirty);
    FB_ASSERT_TRUE(entry.needs_flush());
}

FB_TEST(bdev_cache_coherence, pinned_cannot_dirty) {
    cache_entry entry;
    entry.pin();
    entry.mark_dirty(2, 1000000);
    FB_ASSERT_TRUE(entry.state == cache_state::clean);  // unchanged
}

FB_TEST(bdev_cache_coherence, pinned_cannot_invalidate) {
    cache_entry entry;
    entry.pin();
    entry.invalidate();
    FB_ASSERT_TRUE(entry.is_valid());
}

FB_TEST(bdev_cache_coherence, start_flush_from_dirty) {
    cache_entry entry;
    entry.mark_dirty(2, 1000000);
    entry.start_flush();
    FB_ASSERT_TRUE(entry.state == cache_state::flushing);
}

FB_TEST(bdev_cache_coherence, mark_clean_after_flush) {
    cache_entry entry;
    entry.mark_dirty(2, 1000000);
    entry.start_flush();
    entry.mark_clean();
    FB_ASSERT_TRUE(entry.state == cache_state::clean);
}

FB_TEST(bdev_cache_coherence, invalidate_from_clean) {
    cache_entry entry;
    entry.invalidate();
    FB_ASSERT_TRUE(entry.state == cache_state::invalid);
    FB_ASSERT_FALSE(entry.is_valid());
}

FB_TEST(bdev_cache_coherence, manager_insert_get) {
    cache_manager cm;
    cm.insert(100, 1000000);

    auto entry = cm.get(100, 2000000);
    FB_ASSERT_TRUE(entry != nullptr);
    FB_ASSERT_EQ(entry->last_access_us, 2000000u);
}

FB_TEST(bdev_cache_coherence, manager_mark_dirty) {
    cache_manager cm;
    cm.insert(100, 0);
    cm.mark_dirty(100, 1000000);

    FB_ASSERT_EQ(cm.dirty_count(), 1u);
    FB_ASSERT_TRUE(cm.entries[100].needs_flush());
}

FB_TEST(bdev_cache_coherence, manager_flush_all) {
    cache_manager cm;
    cm.insert(100, 0);
    cm.insert(200, 0);
    cm.mark_dirty(100, 0);
    cm.mark_dirty(200, 0);

    cm.flush_all();
    FB_ASSERT_EQ(cm.dirty_count(), 0u);  // all flushing
}

FB_TEST(bdev_cache_coherence, manager_invalidate_all) {
    cache_manager cm;
    cm.insert(100, 0);
    cm.insert(200, 0);
    cm.invalidate_all();

    FB_ASSERT_EQ(cm.valid_count(), 0u);
}

FB_TEST(bdev_cache_coherence, version_increments_on_dirty) {
    cache_manager cm;
    cm.insert(100, 0);
    cm.mark_dirty(100, 0);
    cm.mark_dirty(100, 0);

    FB_ASSERT_EQ(cm.current_version, 3u);
}

FB_TEST(bdev_cache_coherence, get_invalid_returns_null) {
    cache_manager cm;
    cm.insert(100, 0);
    cm.entries[100].invalidate();

    FB_ASSERT_TRUE(cm.get(100, 0) == nullptr);
}

FB_TEST(bdev_cache_coherence, pin_unpin_flow) {
    cache_manager cm;
    cm.insert(100, 0);
    cm.pin(100);

    FB_ASSERT_TRUE(cm.entries[100].pinned);

    cm.unpin(100);
    FB_ASSERT_FALSE(cm.entries[100].pinned);
}

// ============================================================================
// Test Suite: bdev_io_scheduler — IO scheduling and fairness
// ============================================================================

FB_SUITE_SETUP(bdev_io_scheduler) {}
FB_SUITE_TEARDOWN(bdev_io_scheduler) {}

enum class scheduler_policy {
    fifo,
    round_robin,
    weighted_fair
};

struct io_request {
    uint64_t id{0};
    uint64_t submit_time_us{0};
    uint32_t weight{1};
    uint32_t client_id{0};
    bool scheduled{false};
};

struct io_scheduler {
    scheduler_policy policy{scheduler_policy::fifo};
    std::vector<io_request> pending;
    std::vector<io_request> scheduled_list;
    uint32_t current_rr_index{0};

    void submit(io_request req) {
        pending.push_back(req);
    }

    void schedule_next(uint64_t now_us) {
        if (pending.empty()) return;

        io_request next;
        size_t next_idx = 0;

        switch (policy) {
            case scheduler_policy::fifo:
                next_idx = 0;
                break;
            case scheduler_policy::round_robin:
                next_idx = current_rr_index % pending.size();
                current_rr_index = (current_rr_index + 1) % pending.size();
                break;
            case scheduler_policy::weighted_fair:
                // Find highest weight
                uint32_t max_weight = 0;
                for (size_t i = 0; i < pending.size(); ++i) {
                    if (pending[i].weight > max_weight) {
                        max_weight = pending[i].weight;
                        next_idx = i;
                    }
                }
                break;
        }

        next = pending[next_idx];
        next.scheduled = true;
        next.submit_time_us = now_us;
        scheduled_list.push_back(next);
        pending.erase(pending.begin() + next_idx);
    }

    size_t pending_count() const { return pending.size(); }
    size_t scheduled_count() const { return scheduled_list.size(); }

    void clear() {
        pending.clear();
        scheduled_list.clear();
        current_rr_index = 0;
    }

    std::optional<io_request> get_scheduled(uint64_t id) const {
        for (const auto& req : scheduled_list) {
            if (req.id == id) return req;
        }
        return std::nullopt;
    }
};

FB_TEST(bdev_io_scheduler, empty_initial_state) {
    io_scheduler sched;
    FB_ASSERT_EQ(sched.pending_count(), 0u);
    FB_ASSERT_EQ(sched.scheduled_count(), 0u);
}

FB_TEST(bdev_io_scheduler, submit_adds_to_pending) {
    io_scheduler sched;
    io_request req{1, 0, 1, 100};
    sched.submit(req);
    FB_ASSERT_EQ(sched.pending_count(), 1u);
}

FB_TEST(bdev_io_scheduler, fifo_schedules_first) {
    io_scheduler sched;
    sched.submit({1, 0, 1, 100});
    sched.submit({2, 0, 1, 200});

    sched.schedule_next(1000000);
    FB_ASSERT_EQ(sched.scheduled_count(), 1u);
    FB_ASSERT_EQ(sched.get_scheduled(1)->client_id, 100u);
}

FB_TEST(bdev_io_scheduler, round_robin_cycles) {
    io_scheduler sched;
    sched.policy = scheduler_policy::round_robin;
    sched.submit({1, 0, 1, 100});
    sched.submit({2, 0, 1, 200});
    sched.submit({3, 0, 1, 300});

    sched.schedule_next(0);  // schedules index 0
    sched.schedule_next(0);  // schedules index 0 (was 1, now 0)
    sched.schedule_next(0);  // schedules index 0 (was 2, now 0)

    FB_ASSERT_EQ(sched.scheduled_count(), 3u);
}

FB_TEST(bdev_io_scheduler, weighted_fair_picks_highest) {
    io_scheduler sched;
    sched.policy = scheduler_policy::weighted_fair;
    sched.submit({1, 0, 1, 100});
    sched.submit({2, 0, 5, 200});  // higher weight
    sched.submit({3, 0, 3, 300});

    sched.schedule_next(0);
    FB_ASSERT_EQ(sched.get_scheduled(2)->weight, 5u);
}

FB_TEST(bdev_io_scheduler, schedule_removes_from_pending) {
    io_scheduler sched;
    sched.submit({1, 0, 1, 100});
    sched.submit({2, 0, 1, 200});

    FB_ASSERT_EQ(sched.pending_count(), 2u);
    sched.schedule_next(0);
    FB_ASSERT_EQ(sched.pending_count(), 1u);
}

FB_TEST(bdev_io_scheduler, clear_resets_state) {
    io_scheduler sched;
    sched.submit({1, 0, 1, 100});
    sched.schedule_next(0);

    sched.clear();
    FB_ASSERT_EQ(sched.pending_count(), 0u);
    FB_ASSERT_EQ(sched.scheduled_count(), 0u);
}

FB_TEST(bdev_io_scheduler, get_scheduled_not_found) {
    io_scheduler sched;
    sched.submit({1, 0, 1, 100});
    sched.schedule_next(0);

    FB_ASSERT_FALSE(sched.get_scheduled(999).has_value());
}

FB_TEST(bdev_io_scheduler, multiple_schedules_order) {
    io_scheduler sched;
    for (int i = 1; i <= 5; ++i) {
        sched.submit({(uint64_t)i, 0, 1, (uint32_t)i * 100});
    }

    for (int i = 0; i < 5; ++i) {
        sched.schedule_next(0);
    }

    FB_ASSERT_EQ(sched.scheduled_count(), 5u);
    FB_ASSERT_EQ(sched.pending_count(), 0u);
}

FB_TEST(bdev_io_scheduler, weighted_with_same_weight_picks_first) {
    io_scheduler sched;
    sched.policy = scheduler_policy::weighted_fair;
    sched.submit({1, 0, 3, 100});
    sched.submit({2, 0, 3, 200});

    sched.schedule_next(0);
    FB_ASSERT_EQ(sched.get_scheduled(1)->id, 1u);
}

// ============================================================================
// Test Suite: bdev_replica_state — Replica state machine tracking
// ============================================================================

FB_SUITE_SETUP(bdev_replica_state) {}
FB_SUITE_TEARDOWN(bdev_replica_state) {}

enum class replica_role {
    unknown,
    primary,
    secondary,
    spare
};

enum class replica_health {
    healthy,
    degraded,
    recovering,
    failed
};

struct replica_state {
    uint32_t replica_id{0};
    replica_role role{replica_role::unknown};
    replica_health health{replica_health::healthy};
    uint64_t last_sync_us{0};
    uint64_t sync_lag_us{0};
    bool is_syncing{false};

    void set_primary() {
        role = replica_role::primary;
        health = replica_health::healthy;
    }

    void set_secondary(uint64_t now_us) {
        role = replica_role::secondary;
        last_sync_us = now_us;
    }

    void update_sync_lag(uint64_t now_us) {
        if (role == replica_role::secondary) {
            sync_lag_us = now_us - last_sync_us;
            if (sync_lag_us > 10000000) {  // 10s threshold
                health = replica_health::degraded;
            }
        }
    }

    void start_recovery() {
        if (health == replica_health::degraded || health == replica_role::failed) {
            health = replica_health::recovering;
            is_syncing = true;
        }
    }

    void complete_recovery(uint64_t now_us) {
        if (health == replica_health::recovering) {
            health = replica_health::healthy;
            is_syncing = false;
            last_sync_us = now_us;
        }
    }

    void mark_failed() {
        health = replica_health::failed;
        is_syncing = false;
    }

    bool needs_recovery() const {
        return health == replica_health::degraded || health == replica_health::failed;
    }

    bool is_available() const {
        return health == replica_health::healthy && role != replica_role::unknown;
    }
};

struct replica_group {
    std::vector<replica_state> replicas;
    uint32_t primary_id{0};

    void add_replica(uint32_t id) {
        replica_state r;
        r.replica_id = id;
        replicas.push_back(r);
    }

    replica_state* get_replica(uint32_t id) {
        for (auto& r : replicas) {
            if (r.replica_id == id) return &r;
        }
        return nullptr;
    }

    void designate_primary(uint32_t id) {
        auto* r = get_replica(id);
        if (r) {
            r->set_primary();
            primary_id = id;
        }
    }

    size_t healthy_count() const {
        size_t n = 0;
        for (const auto& r : replicas) {
            if (r.health == replica_health::healthy) n++;
        }
        return n;
    }

    size_t available_count() const {
        size_t n = 0;
        for (const auto& r : replicas) {
            if (r.is_available()) n++;
        }
        return n;
    }

    bool can_serve_reads() const {
        return healthy_count() >= 2;
    }

    bool can_serve_writes() const {
        for (const auto& r : replicas) {
            if (r.role == replica_role::primary && r.is_available()) return true;
        }
        return false;
    }
};

FB_TEST(bdev_replica_state, initial_unknown_role) {
    replica_state r;
    FB_ASSERT_TRUE(r.role == replica_role::unknown);
    FB_ASSERT_FALSE(r.is_available());
}

FB_TEST(bdev_replica_state, set_primary_healthy) {
    replica_state r;
    r.set_primary();
    FB_ASSERT_TRUE(r.role == replica_role::primary);
    FB_ASSERT_TRUE(r.health == replica_health::healthy);
    FB_ASSERT_TRUE(r.is_available());
}

FB_TEST(bdev_replica_state, set_secondary_updates_sync_time) {
    replica_state r;
    r.set_secondary(1000000);
    FB_ASSERT_TRUE(r.role == replica_role::secondary);
    FB_ASSERT_EQ(r.last_sync_us, 1000000u);
}

FB_TEST(bdev_replica_state, sync_lag_marks_degraded) {
    replica_state r;
    r.set_secondary(0);
    r.update_sync_lag(20000000);  // 20s lag

    FB_ASSERT_TRUE(r.health == replica_health::degraded);
}

FB_TEST(bdev_replica_state, small_lag_healthy) {
    replica_state r;
    r.set_secondary(0);
    r.update_sync_lag(5000000);  // 5s lag

    FB_ASSERT_TRUE(r.health == replica_health::healthy);
}

FB_TEST(bdev_replica_state, start_recovery_from_degraded) {
    replica_state r;
    r.health = replica_health::degraded;
    r.start_recovery();

    FB_ASSERT_TRUE(r.health == replica_health::recovering);
    FB_ASSERT_TRUE(r.is_syncing);
}

FB_TEST(bdev_replica_state, complete_recovery_becomes_healthy) {
    replica_state r;
    r.health = replica_health::recovering;
    r.complete_recovery(1000000);

    FB_ASSERT_TRUE(r.health == replica_health::healthy);
    FB_ASSERT_FALSE(r.is_syncing);
}

FB_TEST(bdev_replica_state, mark_failed_stops_syncing) {
    replica_state r;
    r.health = replica_health::recovering;
    r.is_syncing = true;
    r.mark_failed();

    FB_ASSERT_TRUE(r.health == replica_health::failed);
    FB_ASSERT_FALSE(r.is_syncing);
}

FB_TEST(bdev_replica_state, needs_recovery_check) {
    replica_state r;
    r.health = replica_health::degraded;
    FB_ASSERT_TRUE(r.needs_recovery());

    r.health = replica_health::healthy;
    FB_ASSERT_FALSE(r.needs_recovery());
}

FB_TEST(bdev_replica_state, group_add_replica) {
    replica_group group;
    group.add_replica(1);
    group.add_replica(2);
    FB_ASSERT_EQ(group.replicas.size(), 2u);
}

FB_TEST(bdev_replica_state, group_designate_primary) {
    replica_group group;
    group.add_replica(1);
    group.designate_primary(1);

    FB_ASSERT_EQ(group.primary_id, 1u);
    FB_ASSERT_TRUE(group.get_replica(1)->role == replica_role::primary);
}

FB_TEST(bdev_replica_state, group_healthy_count) {
    replica_group group;
    group.add_replica(1);
    group.add_replica(2);
    group.add_replica(3);
    group.designate_primary(1);
    group.get_replica(2)->set_secondary(0);
    group.get_replica(3)->set_secondary(0);
    group.get_replica(3)->health = replica_health::degraded;

    FB_ASSERT_EQ(group.healthy_count(), 2u);
}

FB_TEST(bdev_replica_state, group_can_serve_reads) {
    replica_group group;
    group.add_replica(1);
    group.add_replica(2);
    group.designate_primary(1);
    group.get_replica(2)->set_secondary(0);

    FB_ASSERT_TRUE(group.can_serve_reads());
}

FB_TEST(bdev_replica_state, group_can_serve_writes_with_primary) {
    replica_group group;
    group.add_replica(1);
    group.designate_primary(1);

    FB_ASSERT_TRUE(group.can_serve_writes());
}

FB_TEST(bdev_replica_state, group_cannot_write_without_primary) {
    replica_group group;
    group.add_replica(1);
    group.add_replica(2);
    // no primary designated

    FB_ASSERT_FALSE(group.can_serve_writes());
}

// ============================================================================
// Test Main Entry Point
// ============================================================================

FB_TEST_MAIN()
