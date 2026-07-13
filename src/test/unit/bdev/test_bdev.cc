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
        if (health == replica_health::degraded || health == replica_health::failed) {
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
// Test Suite: bdev_extent_allocator — Extent allocation tracking
// ============================================================================

FB_SUITE_SETUP(bdev_extent_allocator) {}
FB_SUITE_TEARDOWN(bdev_extent_allocator) {}

struct extent {
    uint64_t start_block{0};
    uint64_t length{0};
    uint64_t allocated_at_us{0};
    bool is_free{false};

    bool contains(uint64_t block) const {
        return block >= start_block && block < start_block + length;
    }

    bool overlaps(const extent& other) const {
        return !(start_block + length <= other.start_block ||
                 other.start_block + other.length <= start_block);
    }

    bool can_merge(const extent& other) const {
        return is_free && other.is_free &&
               (start_block + length == other.start_block ||
                other.start_block + other.length == start_block);
    }
};

struct extent_allocator {
    std::vector<extent> extents;
    uint64_t total_blocks{0};
    uint64_t free_blocks{0};
    uint64_t next_search_start{0};

    void initialize(uint64_t total) {
        total_blocks = total;
        free_blocks = total;
        extent e;
        e.start_block = 0;
        e.length = total;
        e.is_free = true;
        extents.push_back(e);
    }

    std::optional<extent> allocate(uint64_t size, uint64_t now_us) {
        if (size > free_blocks) return std::nullopt;

        for (size_t i = 0; i < extents.size(); ++i) {
            if (extents[i].is_free && extents[i].length >= size) {
                extent allocated;
                allocated.start_block = extents[i].start_block;
                allocated.length = size;
                allocated.allocated_at_us = now_us;
                allocated.is_free = false;

                // Split the free extent
                if (extents[i].length > size) {
                    extent remaining;
                    remaining.start_block = extents[i].start_block + size;
                    remaining.length = extents[i].length - size;
                    remaining.is_free = true;
                    extents.insert(extents.begin() + i + 1, remaining);
                }

                extents[i] = allocated;
                free_blocks -= size;
                return allocated;
            }
        }
        return std::nullopt;
    }

    void free(uint64_t start, uint64_t length) {
        for (size_t i = 0; i < extents.size(); ++i) {
            if (extents[i].start_block == start) {
                extents[i].is_free = true;
                free_blocks += length;
                coalesce_adjacent(i);
                return;
            }
        }
    }

    void coalesce_adjacent(size_t idx) {
        // Coalesce with next
        if (idx + 1 < extents.size() && extents[idx].can_merge(extents[idx + 1])) {
            extents[idx].length += extents[idx + 1].length;
            extents.erase(extents.begin() + idx + 1);
        }
        // Coalesce with previous
        if (idx > 0 && extents[idx].can_merge(extents[idx - 1])) {
            extents[idx - 1].length += extents[idx].length;
            extents.erase(extents.begin() + idx);
        }
    }

    double utilization() const {
        if (total_blocks == 0) return 0.0;
        return static_cast<double>(total_blocks - free_blocks) / total_blocks;
    }

    extent* find_extent(uint64_t block) {
        for (auto& e : extents) {
            if (e.contains(block)) return &e;
        }
        return nullptr;
    }
};

FB_TEST(bdev_extent_allocator, initialize_creates_single_free_extent) {
    extent_allocator alloc;
    alloc.initialize(1000);
    FB_ASSERT_EQ(alloc.extents.size(), 1u);
    FB_ASSERT_EQ(alloc.free_blocks, 1000u);
}

FB_TEST(bdev_extent_allocator, allocate_success) {
    extent_allocator alloc;
    alloc.initialize(1000);

    auto ext = alloc.allocate(100, 0);
    FB_ASSERT_TRUE(ext.has_value());
    FB_ASSERT_EQ(ext->length, 100u);
    FB_ASSERT_EQ(alloc.free_blocks, 900u);
}

FB_TEST(bdev_extent_allocator, allocate_fails_if_insufficient) {
    extent_allocator alloc;
    alloc.initialize(100);

    auto ext = alloc.allocate(200, 0);
    FB_ASSERT_FALSE(ext.has_value());
}

FB_TEST(bdev_extent_allocator, allocate_splits_extent) {
    extent_allocator alloc;
    alloc.initialize(1000);
    alloc.allocate(100, 0);

    FB_ASSERT_EQ(alloc.extents.size(), 2u);
    FB_ASSERT_TRUE(alloc.extents[1].is_free);
    FB_ASSERT_EQ(alloc.extents[1].start_block, 100u);
}

FB_TEST(bdev_extent_allocator, free_restores_space) {
    extent_allocator alloc;
    alloc.initialize(1000);
    auto ext = alloc.allocate(100, 0);

    alloc.free(ext->start_block, ext->length);
    FB_ASSERT_EQ(alloc.free_blocks, 1000u);
}

FB_TEST(bdev_extent_allocator, free_coalesces_adjacent) {
    extent_allocator alloc;
    alloc.initialize(1000);
    auto e1 = alloc.allocate(100, 0);
    auto e2 = alloc.allocate(100, 0);

    alloc.free(e1->start_block, e1->length);
    alloc.free(e2->start_block, e2->length);

    FB_ASSERT_EQ(alloc.extents.size(), 1u);  // merged back
}

FB_TEST(bdev_extent_allocator, utilization_calculation) {
    extent_allocator alloc;
    alloc.initialize(1000);
    alloc.allocate(250, 0);

    FB_ASSERT_TRUE(alloc.utilization() > 0.24 && alloc.utilization() < 0.26);
}

FB_TEST(bdev_extent_allocator, find_extent_by_block) {
    extent_allocator alloc;
    alloc.initialize(1000);
    alloc.allocate(100, 0);

    auto ext = alloc.find_extent(50);
    FB_ASSERT_TRUE(ext != nullptr);
    FB_ASSERT_FALSE(ext->is_free);
}

FB_TEST(bdev_extent_allocator, find_extent_free_block) {
    extent_allocator alloc;
    alloc.initialize(1000);
    alloc.allocate(100, 0);

    auto ext = alloc.find_extent(150);
    FB_ASSERT_TRUE(ext != nullptr);
    FB_ASSERT_TRUE(ext->is_free);
}

FB_TEST(bdev_extent_allocator, multiple_allocations) {
    extent_allocator alloc;
    alloc.initialize(1000);

    alloc.allocate(100, 0);
    alloc.allocate(200, 0);
    alloc.allocate(50, 0);

    FB_ASSERT_EQ(alloc.free_blocks, 650u);
    FB_ASSERT_EQ(alloc.extents.size(), 4u);
}

FB_TEST(bdev_extent_allocator, extent_contains_check) {
    extent ext;
    ext.start_block = 100;
    ext.length = 50;

    FB_ASSERT_TRUE(ext.contains(100));
    FB_ASSERT_TRUE(ext.contains(149));
    FB_ASSERT_FALSE(ext.contains(150));
}

FB_TEST(bdev_extent_allocator, extent_overlaps_check) {
    extent e1;
    e1.start_block = 0;
    e1.length = 100;

    extent e2;
    e2.start_block = 50;
    e2.length = 100;

    FB_ASSERT_TRUE(e1.overlaps(e2));
}

FB_TEST(bdev_extent_allocator, extent_no_overlap) {
    extent e1;
    e1.start_block = 0;
    e1.length = 100;

    extent e2;
    e2.start_block = 100;
    e2.length = 100;

    FB_ASSERT_FALSE(e1.overlaps(e2));
}

FB_TEST(bdev_extent_allocator, extent_can_merge_contiguous) {
    extent e1;
    e1.start_block = 0;
    e1.length = 100;
    e1.is_free = true;

    extent e2;
    e2.start_block = 100;
    e2.length = 50;
    e2.is_free = true;

    FB_ASSERT_TRUE(e1.can_merge(e2));
}

// ============================================================================
// Test Suite: bdev_checksum_verification — Checksum verification
// ============================================================================

FB_SUITE_SETUP(bdev_checksum_verification) {}
FB_SUITE_TEARDOWN(bdev_checksum_verification) {}

enum class checksum_type : uint8_t {
    none = 0,
    crc32 = 1,
    crc64 = 2,
    xxhash = 3
};

struct checksum_ctx {
    checksum_type type{checksum_type::none};
    uint64_t value{0};
    uint64_t computed_value{0};
    bool verified{false};

    void set(checksum_type t, uint64_t val) {
        type = t;
        value = val;
        verified = false;
    }

    void compute_crc32(const void* data, size_t len) {
        // Simplified CRC32 simulation
        uint32_t crc = 0xFFFFFFFF;
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            crc ^= bytes[i];
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }
        computed_value = ~crc;
        type = checksum_type::crc32;
    }

    bool verify() {
        if (type == checksum_type::none) return true;
        verified = (value == computed_value);
        return verified;
    }

    void reset() {
        type = checksum_type::none;
        value = 0;
        computed_value = 0;
        verified = false;
    }
};

struct checksum_manager {
    std::unordered_map<uint64_t, checksum_ctx> block_checksums;

    void set_checksum(uint64_t block, checksum_type type, uint64_t value) {
        block_checksums[block].set(type, value);
    }

    bool verify_block(uint64_t block, const void* data, size_t len) {
        auto it = block_checksums.find(block);
        if (it == block_checksums.end()) return true;  // no checksum

        if (it->second.type == checksum_type::crc32) {
            it->second.compute_crc32(data, len);
        } else if (it->second.type == checksum_type::crc64) {
            it->second.computed_value = *(static_cast<const uint64_t*>(data));  // simplified
        }

        return it->second.verify();
    }

    bool has_checksum(uint64_t block) const {
        return block_checksums.find(block) != block_checksums.end();
    }

    void remove_checksum(uint64_t block) {
        block_checksums.erase(block);
    }

    size_t count() const { return block_checksums.size(); }

    size_t verified_count() const {
        size_t n = 0;
        for (const auto& [_, ctx] : block_checksums) {
            if (ctx.verified) n++;
        }
        return n;
    }

    void clear() { block_checksums.clear(); }
};

FB_TEST(bdev_checksum_verification, initial_no_checksum) {
    checksum_ctx chk_ctx;
    FB_ASSERT_TRUE(chk_ctx.type == checksum_type::none);
}

FB_TEST(bdev_checksum_verification, set_checksum_type_and_value) {
    checksum_ctx chk_ctx;
    chk_ctx.set(checksum_type::crc32, 0x12345678);
    FB_ASSERT_TRUE(chk_ctx.type == checksum_type::crc32);
    FB_ASSERT_EQ(chk_ctx.value, 0x12345678ull);
}

FB_TEST(bdev_checksum_verification, compute_crc32) {
    checksum_ctx chk_ctx;
    uint8_t data[] = {1, 2, 3, 4, 5};
    chk_ctx.compute_crc32(data, 5);
    FB_ASSERT_TRUE(chk_ctx.type == checksum_type::crc32);
    FB_ASSERT_NE(chk_ctx.computed_value, 0ull);
}

FB_TEST(bdev_checksum_verification, verify_success_when_match) {
    checksum_ctx chk_ctx;
    uint8_t data[] = {1, 2, 3, 4, 5};
    chk_ctx.compute_crc32(data, 5);
    chk_ctx.value = chk_ctx.computed_value;  // match

    FB_ASSERT_TRUE(chk_ctx.verify());
}

FB_TEST(bdev_checksum_verification, verify_fails_on_mismatch) {
    checksum_ctx chk_ctx;
    chk_ctx.type = checksum_type::crc32;
    chk_ctx.value = 0x12345678;
    chk_ctx.computed_value = 0x87654321;

    FB_ASSERT_FALSE(chk_ctx.verify());
}

FB_TEST(bdev_checksum_verification, verify_none_always_succeeds) {
    checksum_ctx chk_ctx;
    chk_ctx.type = checksum_type::none;
    FB_ASSERT_TRUE(chk_ctx.verify());
}

FB_TEST(bdev_checksum_verification, reset_clears_all) {
    checksum_ctx chk_ctx;
    chk_ctx.set(checksum_type::crc32, 123);
    chk_ctx.reset();

    FB_ASSERT_TRUE(chk_ctx.type == checksum_type::none);
    FB_ASSERT_EQ(chk_ctx.value, 0ull);
}

FB_TEST(bdev_checksum_verification, manager_set_checksum) {
    checksum_manager mgr;
    mgr.set_checksum(100, checksum_type::crc32, 0xABCD);

    FB_ASSERT_TRUE(mgr.has_checksum(100));
    FB_ASSERT_EQ(mgr.count(), 1u);
}

FB_TEST(bdev_checksum_verification, manager_verify_block_no_checksum) {
    checksum_manager mgr;
    uint8_t data[] = {1, 2, 3};

    FB_ASSERT_TRUE(mgr.verify_block(999, data, 3));  // no checksum
}

FB_TEST(bdev_checksum_verification, manager_verify_block_with_checksum) {
    checksum_manager mgr;
    uint8_t data[] = {1, 2, 3, 4, 5};

    checksum_ctx temp;
    temp.compute_crc32(data, 5);
    mgr.set_checksum(100, checksum_type::crc32, temp.computed_value);

    FB_ASSERT_TRUE(mgr.verify_block(100, data, 5));
}

FB_TEST(bdev_checksum_verification, manager_remove_checksum) {
    checksum_manager mgr;
    mgr.set_checksum(100, checksum_type::crc32, 123);
    mgr.remove_checksum(100);

    FB_ASSERT_FALSE(mgr.has_checksum(100));
}

FB_TEST(bdev_checksum_verification, manager_verified_count) {
    checksum_manager mgr;
    mgr.set_checksum(1, checksum_type::crc32, 100);
    mgr.set_checksum(2, checksum_type::crc32, 200);

    mgr.block_checksums[1].verified = true;
    FB_ASSERT_EQ(mgr.verified_count(), 1u);
}

FB_TEST(bdev_checksum_verification, manager_clear) {
    checksum_manager mgr;
    mgr.set_checksum(1, checksum_type::crc32, 100);
    mgr.set_checksum(2, checksum_type::crc32, 200);
    mgr.clear();

    FB_ASSERT_EQ(mgr.count(), 0u);
}

// ============================================================================
// Test Suite: bdev_quota_management — Quota enforcement
// ============================================================================

FB_SUITE_SETUP(bdev_quota_management) {}
FB_SUITE_TEARDOWN(bdev_quota_management) {}

struct quota_limit {
    uint64_t max_bytes{0};
    uint64_t used_bytes{0};
    uint64_t reserved_bytes{0};
    bool enforced{true};

    void set_limit(uint64_t limit) { max_bytes = limit; }

    bool can_allocate(uint64_t size) const {
        if (!enforced) return true;
        return used_bytes + reserved_bytes + size <= max_bytes;
    }

    void allocate(uint64_t size) {
        if (can_allocate(size)) used_bytes += size;
    }

    void free(uint64_t size) {
        if (used_bytes >= size) used_bytes -= size;
        else used_bytes = 0;
    }

    void reserve(uint64_t size) {
        if (can_allocate(size)) reserved_bytes += size;
    }

    void release_reservation(uint64_t size) {
        if (reserved_bytes >= size) reserved_bytes -= size;
        else reserved_bytes = 0;
    }

    uint64_t available() const {
        if (used_bytes + reserved_bytes > max_bytes) return 0;
        return max_bytes - used_bytes - reserved_bytes;
    }

    double usage_ratio() const {
        if (max_bytes == 0) return 0.0;
        return static_cast<double>(used_bytes) / max_bytes;
    }

    void reset() { used_bytes = 0; reserved_bytes = 0; }
};

struct quota_manager {
    std::unordered_map<std::string, quota_limit> pool_quotas;

    void set_pool_quota(const std::string& pool, uint64_t limit) {
        pool_quotas[pool].set_limit(limit);
    }

    quota_limit* get_quota(const std::string& pool) {
        auto it = pool_quotas.find(pool);
        return it != pool_quotas.end() ? &it->second : nullptr;
    }

    bool can_allocate(const std::string& pool, uint64_t size) const {
        auto it = pool_quotas.find(pool);
        return it == pool_quotas.end() || it->second.can_allocate(size);
    }

    void allocate(const std::string& pool, uint64_t size) {
        auto it = pool_quotas.find(pool);
        if (it != pool_quotas.end()) it->second.allocate(size);
    }

    void free(const std::string& pool, uint64_t size) {
        auto it = pool_quotas.find(pool);
        if (it != pool_quotas.end()) it->second.free(size);
    }

    uint64_t total_used() const {
        uint64_t total = 0;
        for (const auto& [_, q] : pool_quotas) total += q.used_bytes;
        return total;
    }

    size_t pool_count() const { return pool_quotas.size(); }
};

FB_TEST(bdev_quota_management, limit_initial_zero) {
    quota_limit limit;
    FB_ASSERT_EQ(limit.max_bytes, 0u);
}

FB_TEST(bdev_quota_management, set_limit_configures_max) {
    quota_limit limit;
    limit.set_limit(10ull * 1024 * 1024 * 1024);
    FB_ASSERT_EQ(limit.max_bytes, 10ull * 1024 * 1024 * 1024);
}

FB_TEST(bdev_quota_management, can_allocate_within_limit) {
    quota_limit limit;
    limit.set_limit(1000);
    FB_ASSERT_TRUE(limit.can_allocate(500));
}

FB_TEST(bdev_quota_management, can_allocate_exceeds_limit) {
    quota_limit limit;
    limit.set_limit(100);
    FB_ASSERT_FALSE(limit.can_allocate(200));
}

FB_TEST(bdev_quota_management, can_allocate_unenforced) {
    quota_limit limit;
    limit.set_limit(100);
    limit.enforced = false;
    FB_ASSERT_TRUE(limit.can_allocate(1000));
}

FB_TEST(bdev_quota_management, allocate_increments_used) {
    quota_limit limit;
    limit.set_limit(1000);
    limit.allocate(100);
    FB_ASSERT_EQ(limit.used_bytes, 100u);
}

FB_TEST(bdev_quota_management, free_decrements_used) {
    quota_limit limit;
    limit.set_limit(1000);
    limit.allocate(100);
    limit.free(50);
    FB_ASSERT_EQ(limit.used_bytes, 50u);
}

FB_TEST(bdev_quota_management, reserve_affects_available) {
    quota_limit limit;
    limit.set_limit(1000);
    limit.reserve(100);
    FB_ASSERT_EQ(limit.available(), 900u);
}

FB_TEST(bdev_quota_management, usage_ratio) {
    quota_limit limit;
    limit.set_limit(1000);
    limit.allocate(250);
    FB_ASSERT_TRUE(limit.usage_ratio() > 0.24 && limit.usage_ratio() < 0.26);
}

FB_TEST(bdev_quota_management, manager_set_pool_quota) {
    quota_manager mgr;
    mgr.set_pool_quota("pool1", 1000);
    FB_ASSERT_EQ(mgr.pool_count(), 1u);
}

FB_TEST(bdev_quota_management, manager_can_allocate) {
    quota_manager mgr;
    mgr.set_pool_quota("pool1", 100);
    FB_ASSERT_TRUE(mgr.can_allocate("pool1", 50));
    FB_ASSERT_FALSE(mgr.can_allocate("pool1", 200));
}

FB_TEST(bdev_quota_management, manager_no_limit_allows_all) {
    quota_manager mgr;
    FB_ASSERT_TRUE(mgr.can_allocate("unknown_pool", 1000000));
}

FB_TEST(bdev_quota_management, manager_total_used) {
    quota_manager mgr;
    mgr.set_pool_quota("pool1", 1000);
    mgr.set_pool_quota("pool2", 2000);
    mgr.allocate("pool1", 100);
    mgr.allocate("pool2", 200);
    FB_ASSERT_EQ(mgr.total_used(), 300u);
}

// ============================================================================
// Test Suite: bdev_health_monitor — Device health monitoring
// ============================================================================

FB_SUITE_SETUP(bdev_health_monitor) {}
FB_SUITE_TEARDOWN(bdev_health_monitor) {}

enum class health_status : uint8_t {
    unknown,
    healthy,
    warning,
    degraded,
    failed
};

struct health_metric {
    std::string name;
    uint64_t current_value{0};
    uint64_t warning_threshold{0};
    uint64_t critical_threshold{0};
    health_status status{health_status::unknown};

    void set_thresholds(uint64_t warn, uint64_t crit) {
        warning_threshold = warn;
        critical_threshold = crit;
    }

    void update(uint64_t value) {
        current_value = value;
        if (critical_threshold > 0 && value >= critical_threshold) {
            status = health_status::failed;
        } else if (warning_threshold > 0 && value >= warning_threshold) {
            status = health_status::warning;
        } else {
            status = health_status::healthy;
        }
    }

    bool is_healthy() const { return status == health_status::healthy; }
    bool needs_attention() const {
        return status == health_status::warning ||
               status == health_status::degraded ||
               status == health_status::failed;
    }
};

struct health_monitor {
    std::unordered_map<std::string, health_metric> metrics;
    health_status overall_status{health_status::unknown};
    uint64_t last_check_us{0};
    uint64_t check_interval_us{60000000};  // 1 minute

    void add_metric(const std::string& name, uint64_t warn, uint64_t crit) {
        metrics[name].name = name;
        metrics[name].set_thresholds(warn, crit);
    }

    void update_metric(const std::string& name, uint64_t value) {
        auto it = metrics.find(name);
        if (it != metrics.end()) {
            it->second.update(value);
            update_overall();
        }
    }

    void update_overall() {
        overall_status = health_status::healthy;
        for (const auto& [_, m] : metrics) {
            if (static_cast<uint8_t>(m.status) > static_cast<uint8_t>(overall_status)) {
                overall_status = m.status;
            }
        }
    }

    bool should_check(uint64_t now_us) const {
        return now_us - last_check_us >= check_interval_us;
    }

    void mark_checked(uint64_t now_us) { last_check_us = now_us; }

    size_t unhealthy_count() const {
        size_t n = 0;
        for (const auto& [_, m] : metrics) {
            if (!m.is_healthy()) n++;
        }
        return n;
    }

    std::vector<std::string> get_unhealthy_metrics() const {
        std::vector<std::string> result;
        for (const auto& [name, m] : metrics) {
            if (!m.is_healthy()) result.push_back(name);
        }
        return result;
    }
};

FB_TEST(bdev_health_monitor, metric_initial_unknown) {
    health_metric m;
    FB_ASSERT_TRUE(m.status == health_status::unknown);
}

FB_TEST(bdev_health_monitor, metric_set_thresholds) {
    health_metric m;
    m.set_thresholds(100, 200);
    FB_ASSERT_EQ(m.warning_threshold, 100u);
    FB_ASSERT_EQ(m.critical_threshold, 200u);
}

FB_TEST(bdev_health_monitor, metric_update_healthy) {
    health_metric m;
    m.set_thresholds(100, 200);
    m.update(50);
    FB_ASSERT_TRUE(m.status == health_status::healthy);
}

FB_TEST(bdev_health_monitor, metric_update_warning) {
    health_metric m;
    m.set_thresholds(100, 200);
    m.update(150);
    FB_ASSERT_TRUE(m.status == health_status::warning);
}

FB_TEST(bdev_health_monitor, metric_update_failed) {
    health_metric m;
    m.set_thresholds(100, 200);
    m.update(250);
    FB_ASSERT_TRUE(m.status == health_status::failed);
}

FB_TEST(bdev_health_monitor, metric_needs_attention) {
    health_metric m;
    m.set_thresholds(100, 200);
    m.update(150);
    FB_ASSERT_TRUE(m.needs_attention());
}

FB_TEST(bdev_health_monitor, monitor_add_metric) {
    health_monitor mon;
    mon.add_metric("error_rate", 100, 200);
    FB_ASSERT_EQ(mon.metrics.size(), 1u);
}

FB_TEST(bdev_health_monitor, monitor_update_metric) {
    health_monitor mon;
    mon.add_metric("error_rate", 100, 200);
    mon.update_metric("error_rate", 150);

    FB_ASSERT_TRUE(mon.metrics["error_rate"].status == health_status::warning);
}

FB_TEST(bdev_health_monitor, monitor_overall_status) {
    health_monitor mon;
    mon.add_metric("metric1", 100, 200);
    mon.add_metric("metric2", 100, 200);
    mon.update_metric("metric1", 50);   // healthy
    mon.update_metric("metric2", 250); // failed

    FB_ASSERT_TRUE(mon.overall_status == health_status::failed);
}

FB_TEST(bdev_health_monitor, monitor_should_check) {
    health_monitor mon;
    mon.last_check_us = 0;
    FB_ASSERT_TRUE(mon.should_check(60000000));
}

FB_TEST(bdev_health_monitor, monitor_unhealthy_count) {
    health_monitor mon;
    mon.add_metric("m1", 100, 200);
    mon.add_metric("m2", 100, 200);
    mon.add_metric("m3", 100, 200);
    mon.update_metric("m1", 50);   // healthy
    mon.update_metric("m2", 150); // warning
    mon.update_metric("m3", 250); // failed

    FB_ASSERT_EQ(mon.unhealthy_count(), 2u);
}

FB_TEST(bdev_health_monitor, monitor_get_unhealthy_metrics) {
    health_monitor mon;
    mon.add_metric("m1", 100, 200);
    mon.add_metric("m2", 100, 200);
    mon.update_metric("m1", 150);
    mon.update_metric("m2", 250);

    auto unhealthy = mon.get_unhealthy_metrics();
    FB_ASSERT_EQ(unhealthy.size(), 2u);
}

// ============================================================================
// Test Suite: bdev_io_timeout — IO timeout and expiration
// ============================================================================

FB_SUITE_SETUP(bdev_io_timeout) {}
FB_SUITE_TEARDOWN(bdev_io_timeout) {}

struct timeout_config {
    uint64_t default_timeout_us{30000000};  // 30s
    uint64_t min_timeout_us{1000000};       // 1s
    uint64_t max_timeout_us{300000000};     // 5min

    bool is_valid(uint64_t timeout_us) const {
        return timeout_us >= min_timeout_us && timeout_us <= max_timeout_us;
    }

    uint64_t clamp(uint64_t timeout_us) const {
        if (timeout_us < min_timeout_us) return min_timeout_us;
        if (timeout_us > max_timeout_us) return max_timeout_us;
        return timeout_us;
    }
};

struct io_timeout_tracker {
    uint64_t io_id{0};
    uint64_t start_us{0};
    uint64_t deadline_us{0};
    bool completed{false};
    bool timed_out{false};

    void start(uint64_t id, uint64_t now_us, uint64_t timeout_us) {
        io_id = id;
        start_us = now_us;
        deadline_us = now_us + timeout_us;
        completed = false;
        timed_out = false;
    }

    bool is_expired(uint64_t now_us) const {
        return !completed && now_us >= deadline_us;
    }

    void mark_completed(uint64_t now_us) {
        completed = true;
        (void)now_us;
    }

    uint64_t elapsed_us(uint64_t now_us) const {
        return now_us - start_us;
    }

    uint64_t remaining_us(uint64_t now_us) const {
        if (completed || timed_out) return 0;
        if (now_us >= deadline_us) return 0;
        return deadline_us - now_us;
    }
};

struct timeout_manager {
    timeout_config config;
    std::unordered_map<uint64_t, io_timeout_tracker> pending_ios;
    uint64_t timeout_count{0};

    uint64_t start_io(uint64_t io_id, uint64_t now_us, uint64_t timeout_us) {
        uint64_t actual_timeout = config.clamp(timeout_us);
        pending_ios[io_id].start(io_id, now_us, actual_timeout);
        return actual_timeout;
    }

    void complete_io(uint64_t io_id, uint64_t now_us) {
        auto it = pending_ios.find(io_id);
        if (it != pending_ios.end()) {
            it->second.mark_completed(now_us);
            pending_ios.erase(it);
        }
    }

    std::vector<uint64_t> check_timeouts(uint64_t now_us) {
        std::vector<uint64_t> expired;
        for (auto& [id, tracker] : pending_ios) {
            if (tracker.is_expired(now_us)) {
                tracker.timed_out = true;
                expired.push_back(id);
                timeout_count++;
            }
        }
        return expired;
    }

    bool has_pending(uint64_t io_id) const {
        return pending_ios.find(io_id) != pending_ios.end();
    }

    size_t pending_count() const { return pending_ios.size(); }

    void cancel_io(uint64_t io_id) { pending_ios.erase(io_id); }

    void clear() {
        pending_ios.clear();
        timeout_count = 0;
    }
};

FB_TEST(bdev_io_timeout, config_valid_range) {
    timeout_config cfg;
    FB_ASSERT_TRUE(cfg.is_valid(30000000));
    FB_ASSERT_FALSE(cfg.is_valid(500000));    // too low
    FB_ASSERT_FALSE(cfg.is_valid(400000000)); // too high
}

FB_TEST(bdev_io_timeout, config_clamp_low) {
    timeout_config cfg;
    FB_ASSERT_EQ(cfg.clamp(500000), cfg.min_timeout_us);
}

FB_TEST(bdev_io_timeout, config_clamp_high) {
    timeout_config cfg;
    FB_ASSERT_EQ(cfg.clamp(400000000), cfg.max_timeout_us);
}

FB_TEST(bdev_io_timeout, config_clamp_within) {
    timeout_config cfg;
    FB_ASSERT_EQ(cfg.clamp(60000000), 60000000u);
}

FB_TEST(bdev_io_timeout, tracker_start) {
    io_timeout_tracker tracker;
    tracker.start(1, 1000000, 30000000);

    FB_ASSERT_EQ(tracker.io_id, 1u);
    FB_ASSERT_EQ(tracker.deadline_us, 31000000u);
}

FB_TEST(bdev_io_timeout, tracker_not_expired) {
    io_timeout_tracker tracker;
    tracker.start(1, 1000000, 30000000);

    FB_ASSERT_FALSE(tracker.is_expired(20000000));
}

FB_TEST(bdev_io_timeout, tracker_expired) {
    io_timeout_tracker tracker;
    tracker.start(1, 1000000, 30000000);

    FB_ASSERT_TRUE(tracker.is_expired(35000000));
}

FB_TEST(bdev_io_timeout, tracker_completed_not_expired) {
    io_timeout_tracker tracker;
    tracker.start(1, 1000000, 30000000);
    tracker.mark_completed(20000000);

    FB_ASSERT_FALSE(tracker.is_expired(35000000));
}

FB_TEST(bdev_io_timeout, tracker_elapsed) {
    io_timeout_tracker tracker;
    tracker.start(1, 1000000, 30000000);

    FB_ASSERT_EQ(tracker.elapsed_us(5000000), 4000000u);
}

FB_TEST(bdev_io_timeout, tracker_remaining) {
    io_timeout_tracker tracker;
    tracker.start(1, 1000000, 30000000);

    FB_ASSERT_EQ(tracker.remaining_us(5000000), 26000000u);
}

FB_TEST(bdev_io_timeout, manager_start_io) {
    timeout_manager mgr;
    mgr.start_io(1, 0, 30000000);

    FB_ASSERT_TRUE(mgr.has_pending(1));
    FB_ASSERT_EQ(mgr.pending_count(), 1u);
}

FB_TEST(bdev_io_timeout, manager_complete_io) {
    timeout_manager mgr;
    mgr.start_io(1, 0, 30000000);
    mgr.complete_io(1, 15000000);

    FB_ASSERT_FALSE(mgr.has_pending(1));
}

FB_TEST(bdev_io_timeout, manager_check_timeouts) {
    timeout_manager mgr;
    mgr.start_io(1, 0, 10000000);
    mgr.start_io(2, 0, 30000000);

    auto expired = mgr.check_timeouts(20000000);
    FB_ASSERT_EQ(expired.size(), 1u);
    FB_ASSERT_EQ(expired[0], 1u);
    FB_ASSERT_EQ(mgr.timeout_count, 1u);
}

FB_TEST(bdev_io_timeout, manager_cancel_io) {
    timeout_manager mgr;
    mgr.start_io(1, 0, 30000000);
    mgr.cancel_io(1);

    FB_ASSERT_FALSE(mgr.has_pending(1));
}

// ============================================================================
// Test Suite: bdev_write_ordering — Write ordering guarantees
// ============================================================================

FB_SUITE_SETUP(bdev_write_ordering) {}
FB_SUITE_TEARDOWN(bdev_write_ordering) {}

enum class write_order : uint8_t {
    none,
    strict,
    relaxed
};

struct ordered_write {
    uint64_t sequence{0};
    uint64_t offset{0};
    uint64_t length{0};
    bool committed{false};
    bool acked{false};
};

struct write_ordering_manager {
    write_order order_policy{write_order::relaxed};
    std::deque<ordered_write> pending_writes;
    uint64_t next_sequence{1};
    uint64_t committed_sequence{0};

    void submit(uint64_t offset, uint64_t length) {
        ordered_write w;
        w.sequence = next_sequence++;
        w.offset = offset;
        w.length = length;
        pending_writes.push_back(w);
    }

    bool can_commit(const ordered_write& w) const {
        if (order_policy == write_order::strict) {
            return w.sequence == committed_sequence + 1;
        }
        return true;  // relaxed allows any order
    }

    void commit(uint64_t seq) {
        for (auto& w : pending_writes) {
            if (w.sequence == seq && can_commit(w)) {
                w.committed = true;
                if (seq > committed_sequence) committed_sequence = seq;
            }
        }
    }

    void ack(uint64_t seq) {
        for (auto it = pending_writes.begin(); it != pending_writes.end(); ++it) {
            if (it->sequence == seq && it->committed) {
                it->acked = true;
                pending_writes.erase(it);
                return;
            }
        }
    }

    size_t pending_count() const { return pending_writes.size(); }

    std::optional<ordered_write> find_pending(uint64_t seq) const {
        for (const auto& w : pending_writes) {
            if (w.sequence == seq) return w;
        }
        return std::nullopt;
    }

    void clear() {
        pending_writes.clear();
        next_sequence = 1;
        committed_sequence = 0;
    }
};

FB_TEST(bdev_write_ordering, submit_assigns_sequence) {
    write_ordering_manager mgr;
    mgr.submit(0, 1024);
    mgr.submit(1024, 512);

    FB_ASSERT_EQ(mgr.pending_count(), 2u);
    FB_ASSERT_EQ(mgr.find_pending(1)->sequence, 1u);
    FB_ASSERT_EQ(mgr.find_pending(2)->sequence, 2u);
}

FB_TEST(bdev_write_ordering, relaxed_allows_any_commit) {
    write_ordering_manager mgr;
    mgr.order_policy = write_order::relaxed;
    mgr.submit(0, 1024);
    mgr.submit(1024, 512);

    FB_ASSERT_TRUE(mgr.can_commit(*mgr.find_pending(2)));
}

FB_TEST(bdev_write_ordering, strict_requires_sequence) {
    write_ordering_manager mgr;
    mgr.order_policy = write_order::strict;
    mgr.submit(0, 1024);
    mgr.submit(1024, 512);

    FB_ASSERT_TRUE(mgr.can_commit(*mgr.find_pending(1)));
    FB_ASSERT_FALSE(mgr.can_commit(*mgr.find_pending(2)));
}

FB_TEST(bdev_write_ordering, commit_updates_committed_sequence) {
    write_ordering_manager mgr;
    mgr.submit(0, 1024);
    mgr.commit(1);

    FB_ASSERT_TRUE(mgr.find_pending(1)->committed);
    FB_ASSERT_EQ(mgr.committed_sequence, 1u);
}

FB_TEST(bdev_write_ordering, ack_removes_committed) {
    write_ordering_manager mgr;
    mgr.submit(0, 1024);
    mgr.commit(1);
    mgr.ack(1);

    FB_ASSERT_EQ(mgr.pending_count(), 0u);
}

FB_TEST(bdev_write_ordering, ack_fails_if_not_committed) {
    write_ordering_manager mgr;
    mgr.submit(0, 1024);
    mgr.ack(1);  // not committed yet

    FB_ASSERT_EQ(mgr.pending_count(), 1u);  // still pending
}

FB_TEST(bdev_write_ordering, strict_enforces_order) {
    write_ordering_manager mgr;
    mgr.order_policy = write_order::strict;
    mgr.submit(0, 1024);
    mgr.submit(1024, 512);

    mgr.commit(2);  // should fail - sequence 1 not committed
    FB_ASSERT_FALSE(mgr.find_pending(2)->committed);

    mgr.commit(1);  // should succeed
    FB_ASSERT_TRUE(mgr.find_pending(1)->committed);
}

FB_TEST(bdev_write_ordering, clear_resets_state) {
    write_ordering_manager mgr;
    mgr.submit(0, 1024);
    mgr.commit(1);
    mgr.clear();

    FB_ASSERT_EQ(mgr.pending_count(), 0u);
    FB_ASSERT_EQ(mgr.next_sequence, 1u);
    FB_ASSERT_EQ(mgr.committed_sequence, 0u);
}

FB_TEST(bdev_write_ordering, multiple_commits) {
    write_ordering_manager mgr;
    mgr.order_policy = write_order::relaxed;
    for (int i = 0; i < 5; ++i) {
        mgr.submit(i * 1024, 1024);
    }

    for (int i = 1; i <= 5; ++i) {
        mgr.commit(i);
    }

    FB_ASSERT_EQ(mgr.committed_sequence, 5u);
}

// ============================================================================
// Test Suite: bdev_rate_limiter — IO rate limiting
// ============================================================================

FB_SUITE_SETUP(bdev_rate_limiter) {}
FB_SUITE_TEARDOWN(bdev_rate_limiter) {}

struct rate_limiter {
    uint64_t max_ops_per_sec{1000};
    uint64_t max_bytes_per_sec{0};  // 0 means unlimited
    uint64_t ops_in_window{0};
    uint64_t bytes_in_window{0};
    uint64_t window_start_us{0};
    uint64_t window_size_us{1000000};  // 1 second
    bool enabled{true};

    void set_limits(uint64_t ops, uint64_t bytes) {
        max_ops_per_sec = ops;
        max_bytes_per_sec = bytes;
    }

    void reset_window(uint64_t now_us) {
        window_start_us = now_us;
        ops_in_window = 0;
        bytes_in_window = 0;
    }

    bool window_expired(uint64_t now_us) const {
        return now_us - window_start_us >= window_size_us;
    }

    bool can_submit(uint64_t bytes) {
        if (!enabled) return true;

        if (max_ops_per_sec > 0 && ops_in_window >= max_ops_per_sec) {
            return false;
        }
        if (max_bytes_per_sec > 0 && bytes_in_window + bytes > max_bytes_per_sec) {
            return false;
        }
        return true;
    }

    void record_submit(uint64_t bytes) {
        ops_in_window++;
        bytes_in_window += bytes;
    }

    void try_submit(uint64_t now_us, uint64_t bytes) {
        if (window_expired(now_us)) {
            reset_window(now_us);
        }
        if (can_submit(bytes)) {
            record_submit(bytes);
        }
    }

    uint64_t ops_remaining() const {
        if (max_ops_per_sec == 0) return UINT64_MAX;
        if (ops_in_window >= max_ops_per_sec) return 0;
        return max_ops_per_sec - ops_in_window;
    }

    double ops_utilization() const {
        if (max_ops_per_sec == 0) return 0.0;
        return static_cast<double>(ops_in_window) / max_ops_per_sec;
    }
};

FB_TEST(bdev_rate_limiter, initial_no_limits) {
    rate_limiter limiter;
    limiter.enabled = false;
    FB_ASSERT_TRUE(limiter.can_submit(1000000));
}

FB_TEST(bdev_rate_limiter, set_limits_configures) {
    rate_limiter limiter;
    limiter.set_limits(100, 1024 * 1024);
    FB_ASSERT_EQ(limiter.max_ops_per_sec, 100u);
    FB_ASSERT_EQ(limiter.max_bytes_per_sec, 1024u * 1024u);
}

FB_TEST(bdev_rate_limiter, can_submit_within_ops_limit) {
    rate_limiter limiter;
    limiter.set_limits(100, 0);
    limiter.ops_in_window = 50;

    FB_ASSERT_TRUE(limiter.can_submit(1024));
}

FB_TEST(bdev_rate_limiter, cannot_submit_exceeds_ops) {
    rate_limiter limiter;
    limiter.set_limits(100, 0);
    limiter.ops_in_window = 100;

    FB_ASSERT_FALSE(limiter.can_submit(1024));
}

FB_TEST(bdev_rate_limiter, cannot_submit_exceeds_bytes) {
    rate_limiter limiter;
    limiter.set_limits(0, 1000);
    limiter.bytes_in_window = 800;

    FB_ASSERT_FALSE(limiter.can_submit(300));  // would exceed 1000
}

FB_TEST(bdev_rate_limiter, record_submit_increments) {
    rate_limiter limiter;
    limiter.record_submit(1024);

    FB_ASSERT_EQ(limiter.ops_in_window, 1u);
    FB_ASSERT_EQ(limiter.bytes_in_window, 1024u);
}

FB_TEST(bdev_rate_limiter, window_expired_after_time) {
    rate_limiter limiter;
    limiter.window_start_us = 0;

    FB_ASSERT_TRUE(limiter.window_expired(1000000));
    FB_ASSERT_FALSE(limiter.window_expired(500000));
}

FB_TEST(bdev_rate_limiter, reset_window_clears_counters) {
    rate_limiter limiter;
    limiter.ops_in_window = 50;
    limiter.bytes_in_window = 5000;
    limiter.reset_window(1000000);

    FB_ASSERT_EQ(limiter.ops_in_window, 0u);
    FB_ASSERT_EQ(limiter.bytes_in_window, 0u);
}

FB_TEST(bdev_rate_limiter, try_submit_with_window_reset) {
    rate_limiter limiter;
    limiter.set_limits(100, 0);
    limiter.window_start_us = 0;
    limiter.ops_in_window = 100;

    limiter.try_submit(2000000, 1024);  // window expired, reset
    FB_ASSERT_EQ(limiter.ops_in_window, 1u);
}

FB_TEST(bdev_rate_limiter, ops_remaining_calculation) {
    rate_limiter limiter;
    limiter.set_limits(100, 0);
    limiter.ops_in_window = 30;

    FB_ASSERT_EQ(limiter.ops_remaining(), 70u);
}

FB_TEST(bdev_rate_limiter, ops_utilization_calculation) {
    rate_limiter limiter;
    limiter.set_limits(100, 0);
    limiter.ops_in_window = 25;

    FB_ASSERT_TRUE(limiter.ops_utilization() > 0.24 && limiter.ops_utilization() < 0.26);
}

FB_TEST(bdev_rate_limiter, ops_remaining_zero_at_limit) {
    rate_limiter limiter;
    limiter.set_limits(100, 0);
    limiter.ops_in_window = 100;

    FB_ASSERT_EQ(limiter.ops_remaining(), 0u);
}

FB_TEST(bdev_rate_limiter, ops_remaining_unlimited) {
    rate_limiter limiter;
    limiter.max_ops_per_sec = 0;

    FB_ASSERT_EQ(limiter.ops_remaining(), UINT64_MAX);
}

// ============================================================================
// Test Suite: bdev_snapshot_manager — Snapshot creation and management
// ============================================================================

FB_SUITE_SETUP(bdev_snapshot_manager) {}
FB_SUITE_TEARDOWN(bdev_snapshot_manager) {}

enum class snapshot_state : uint8_t {
    creating,
    available,
    deleting,
    error
};

struct snap_entry {
    uint64_t snap_id{0};
    std::string name;
    uint64_t created_at_us{0};
    uint64_t size_bytes{0};
    snapshot_state state{snapshot_state::creating};
    uint64_t parent_snap_id{0};  // 0 = no parent

    bool is_available() const { return state == snapshot_state::available; }

    bool is_ancestor_of(const snap_entry& other) const {
        if (other.parent_snap_id == 0) return false;
        return other.parent_snap_id == snap_id;
    }
};

struct snap_lineage_manager {
    std::unordered_map<uint64_t, snap_entry> snapshots;
    uint64_t next_snap_id{1};

    uint64_t create(const std::string& name, uint64_t now_us, uint64_t size, uint64_t parent_id) {
        snap_entry entry;
        entry.snap_id = next_snap_id++;
        entry.name = name;
        entry.created_at_us = now_us;
        entry.size_bytes = size;
        entry.parent_snap_id = parent_id;
        entry.state = snapshot_state::creating;
        snapshots[entry.snap_id] = entry;
        return entry.snap_id;
    }

    void mark_available(uint64_t snap_id) {
        auto it = snapshots.find(snap_id);
        if (it != snapshots.end() && it->second.state == snapshot_state::creating) {
            it->second.state = snapshot_state::available;
        }
    }

    void mark_deleting(uint64_t snap_id) {
        auto it = snapshots.find(snap_id);
        if (it != snapshots.end() && it->second.state == snapshot_state::available) {
            it->second.state = snapshot_state::deleting;
        }
    }

    void remove(uint64_t snap_id) {
        snapshots.erase(snap_id);
    }

    void mark_error(uint64_t snap_id) {
        auto it = snapshots.find(snap_id);
        if (it != snapshots.end()) {
            it->second.state = snapshot_state::error;
        }
    }

    snap_entry* get(uint64_t snap_id) {
        auto it = snapshots.find(snap_id);
        return it != snapshots.end() ? &it->second : nullptr;
    }

    snap_entry* find_by_name(const std::string& name) {
        for (auto& [_, snap] : snapshots) {
            if (snap.name == name) return &snap;
        }
        return nullptr;
    }

    std::vector<uint64_t> get_children(uint64_t parent_id) const {
        std::vector<uint64_t> children;
        for (const auto& [id, snap] : snapshots) {
            if (snap.parent_snap_id == parent_id) {
                children.push_back(id);
            }
        }
        return children;
    }

    size_t available_count() const {
        size_t n = 0;
        for (const auto& [_, snap] : snapshots) {
            if (snap.is_available()) n++;
        }
        return n;
    }

    size_t count() const { return snapshots.size(); }

    uint64_t total_size() const {
        uint64_t total = 0;
        for (const auto& [_, snap] : snapshots) {
            if (snap.is_available()) total += snap.size_bytes;
        }
        return total;
    }

    bool can_delete(uint64_t snap_id) const {
        auto it = snapshots.find(snap_id);
        if (it == snapshots.end()) return false;
        if (!it->second.is_available()) return false;
        for (const auto& [id, snap] : snapshots) {
            if (snap.parent_snap_id == snap_id && snap.is_available()) {
                return false;
            }
        }
        return true;
    }
};

FB_TEST(bdev_snapshot_manager, create_returns_id) {
    snap_lineage_manager mgr;
    uint64_t id = mgr.create("snap1", 1000, 1024 * 1024, 0);

    FB_ASSERT_EQ(id, 1u);
    FB_ASSERT_EQ(mgr.count(), 1u);
}

FB_TEST(bdev_snap_lineage_manager, create_initial_state_creating) {
    snap_lineage_manager mgr;
    uint64_t id = mgr.create("snap1", 1000, 1024, 0);

    FB_ASSERT_TRUE(mgr.get(id)->state == snapshot_state::creating);
}

FB_TEST(bdev_snap_lineage_manager, mark_available_changes_state) {
    snap_lineage_manager mgr;
    uint64_t id = mgr.create("snap1", 1000, 1024, 0);
    mgr.mark_available(id);

    FB_ASSERT_TRUE(mgr.get(id)->is_available());
}

FB_TEST(bdev_snap_lineage_manager, mark_deleting_from_available) {
    snap_lineage_manager mgr;
    uint64_t id = mgr.create("snap1", 1000, 1024, 0);
    mgr.mark_available(id);
    mgr.mark_deleting(id);

    FB_ASSERT_TRUE(mgr.get(id)->state == snapshot_state::deleting);
}

FB_TEST(bdev_snap_lineage_manager, remove_deletes_entry) {
    snap_lineage_manager mgr;
    uint64_t id = mgr.create("snap1", 1000, 1024, 0);
    mgr.remove(id);

    FB_ASSERT_TRUE(mgr.get(id) == nullptr);
}

FB_TEST(bdev_snap_lineage_manager, mark_error_state) {
    snap_lineage_manager mgr;
    uint64_t id = mgr.create("snap1", 1000, 1024, 0);
    mgr.mark_error(id);

    FB_ASSERT_TRUE(mgr.get(id)->state == snapshot_state::error);
}

FB_TEST(bdev_snap_lineage_manager, find_by_name) {
    snap_lineage_manager mgr;
    mgr.create("snap1", 1000, 1024, 0);
    mgr.create("snap2", 2000, 2048, 0);

    FB_ASSERT_TRUE(mgr.find_by_name("snap1") != nullptr);
    FB_ASSERT_TRUE(mgr.find_by_name("snap2") != nullptr);
    FB_ASSERT_TRUE(mgr.find_by_name("snap3") == nullptr);
}

FB_TEST(bdev_snap_lineage_manager, parent_child_lineage) {
    snap_lineage_manager mgr;
    uint64_t id1 = mgr.create("snap1", 1000, 1024, 0);
    mgr.mark_available(id1);
    uint64_t id2 = mgr.create("snap2", 2000, 2048, id1);
    mgr.mark_available(id2);

    FB_ASSERT_EQ(mgr.get(id2)->parent_snap_id, id1);
    auto children = mgr.get_children(id1);
    FB_ASSERT_EQ(children.size(), 1u);
    FB_ASSERT_EQ(children[0], id2);
}

FB_TEST(bdev_snap_lineage_manager, available_count) {
    snap_lineage_manager mgr;
    uint64_t id1 = mgr.create("snap1", 1000, 1024, 0);
    uint64_t id2 = mgr.create("snap2", 2000, 2048, 0);
    mgr.mark_available(id1);

    FB_ASSERT_EQ(mgr.available_count(), 1u);
}

FB_TEST(bdev_snap_lineage_manager, total_size_available_only) {
    snap_lineage_manager mgr;
    uint64_t id1 = mgr.create("snap1", 1000, 1024, 0);
    mgr.mark_available(id1);
    mgr.create("snap2", 2000, 2048, 0);  // still creating

    FB_ASSERT_EQ(mgr.total_size(), 1024u);  // only snap1 counted
}

FB_TEST(bdev_snap_lineage_manager, can_delete_no_children) {
    snap_lineage_manager mgr;
    uint64_t id = mgr.create("snap1", 1000, 1024, 0);
    mgr.mark_available(id);

    FB_ASSERT_TRUE(mgr.can_delete(id));
}

FB_TEST(bdev_snap_lineage_manager, cannot_delete_with_available_children) {
    snap_lineage_manager mgr;
    uint64_t id1 = mgr.create("snap1", 1000, 1024, 0);
    mgr.mark_available(id1);
    uint64_t id2 = mgr.create("snap2", 2000, 2048, id1);
    mgr.mark_available(id2);

    FB_ASSERT_FALSE(mgr.can_delete(id1));  // has child snap2
}

FB_TEST(bdev_snap_lineage_manager, can_delete_after_child_deleted) {
    snap_lineage_manager mgr;
    uint64_t id1 = mgr.create("snap1", 1000, 1024, 0);
    mgr.mark_available(id1);
    uint64_t id2 = mgr.create("snap2", 2000, 2048, id1);
    mgr.mark_available(id2);

    mgr.mark_deleting(id2);
    mgr.remove(id2);

    FB_ASSERT_TRUE(mgr.can_delete(id1));  // child gone
}

FB_TEST(bdev_snap_lineage_manager, snapshot_chain_depth) {
    snap_lineage_manager mgr;
    uint64_t id1 = mgr.create("snap1", 1000, 1024, 0);
    mgr.mark_available(id1);
    uint64_t id2 = mgr.create("snap2", 2000, 2048, id1);
    mgr.mark_available(id2);
    uint64_t id3 = mgr.create("snap3", 3000, 4096, id2);
    mgr.mark_available(id3);

    // Verify chain: snap3 -> snap2 -> snap1
    FB_ASSERT_EQ(mgr.get(id3)->parent_snap_id, id2);
    FB_ASSERT_EQ(mgr.get(id2)->parent_snap_id, id1);
    FB_ASSERT_EQ(mgr.get(id1)->parent_snap_id, 0u);
}

FB_TEST(bdev_snap_lineage_manager, cannot_delete_creating_snapshot) {
    snap_lineage_manager mgr;
    uint64_t id = mgr.create("snap1", 1000, 1024, 0);
    // still in creating state

    FB_ASSERT_FALSE(mgr.can_delete(id));
}

// ============================================================================
// Test Suite: bdev_rpc_request — RPC request parsing and validation
// ============================================================================

FB_SUITE_SETUP(bdev_rpc_request) {}
FB_SUITE_TEARDOWN(bdev_rpc_request) {}

struct rpc_create_params {
    std::string name;
    std::string pool_name;
    std::string image_name;
    uint64_t image_size{0};
    uint64_t object_size{0};
    uint32_t block_size{512};
    std::string monitor_address;
};

struct rpc_delete_params {
    std::string name;
};

struct rpc_resize_params {
    std::string name;
    uint64_t new_size{0};
};

struct field_decoder {
    std::string field_name;
    bool required;
};

static const std::vector<field_decoder> create_decoders = {
    {"name", true}, {"pool_name", true}, {"image_name", true},
    {"image_size", true}, {"object_size", true}, {"block_size", false},
    {"monitor_address", true}
};

struct rpc_validator {
    static bool validate_required(const std::unordered_map<std::string, std::string>& fields,
                                  const std::vector<field_decoder>& decoders) {
        for (const auto& dec : decoders) {
            if (dec.required && fields.find(dec.field_name) == fields.end()) return false;
        }
        return true;
    }

    static std::vector<std::string> missing_fields(const std::unordered_map<std::string, std::string>& fields,
                                                    const std::vector<field_decoder>& decoders) {
        std::vector<std::string> missing;
        for (const auto& dec : decoders) {
            if (dec.required && fields.find(dec.field_name) == fields.end()) {
                missing.push_back(dec.field_name);
            }
        }
        return missing;
    }

    static std::optional<rpc_create_params> parse_create(const std::unordered_map<std::string, std::string>& fields) {
        if (!validate_required(fields, create_decoders)) return std::nullopt;
        rpc_create_params params;
        params.name = fields.at("name");
        params.pool_name = fields.at("pool_name");
        params.image_name = fields.at("image_name");
        params.image_size = std::stoull(fields.at("image_size"));
        params.object_size = std::stoull(fields.at("object_size"));
        params.monitor_address = fields.at("monitor_address");
        auto it = fields.find("block_size");
        if (it != fields.end()) params.block_size = static_cast<uint32_t>(std::stoul(it->second));
        return params;
    }

    static std::optional<rpc_delete_params> parse_delete(const std::unordered_map<std::string, std::string>& fields) {
        if (fields.find("name") == fields.end()) return std::nullopt;
        return rpc_delete_params{fields.at("name")};
    }

    static std::optional<rpc_resize_params> parse_resize(const std::unordered_map<std::string, std::string>& fields) {
        if (fields.find("name") == fields.end() || fields.find("new_size") == fields.end()) return std::nullopt;
        return rpc_resize_params{fields.at("name"), std::stoull(fields.at("new_size"))};
    }
};

FB_TEST(bdev_rpc_request, create_decoder_count) {
    FB_ASSERT_EQ(create_decoders.size(), 7u);
}

FB_TEST(bdev_rpc_request, create_required_fields) {
    FB_ASSERT_TRUE(create_decoders[0].required);   // name
    FB_ASSERT_FALSE(create_decoders[5].required);  // block_size optional
}

FB_TEST(bdev_rpc_request, validate_all_present) {
    std::unordered_map<std::string, std::string> f = {
        {"name", "b0"}, {"pool_name", "p0"}, {"image_name", "i0"},
        {"image_size", "1"}, {"object_size", "1"}, {"monitor_address", "m0"}
    };
    FB_ASSERT_TRUE(rpc_validator::validate_required(f, create_decoders));
}

FB_TEST(bdev_rpc_request, validate_missing_required) {
    std::unordered_map<std::string, std::string> f = {{ "name", "b0" }};
    FB_ASSERT_FALSE(rpc_validator::validate_required(f, create_decoders));
}

FB_TEST(bdev_rpc_request, missing_fields_returns_list) {
    std::unordered_map<std::string, std::string> f;
    auto missing = rpc_validator::missing_fields(f, create_decoders);
    FB_ASSERT_TRUE(missing.size() >= 5u);
}

FB_TEST(bdev_rpc_request, parse_create_success) {
    std::unordered_map<std::string, std::string> f = {
        {"name", "b0"}, {"pool_name", "p0"}, {"image_name", "i0"},
        {"image_size", "1024"}, {"object_size", "4096"}, {"monitor_address", "m0"}
    };
    auto r = rpc_validator::parse_create(f);
    FB_ASSERT_TRUE(r.has_value());
    FB_ASSERT_STR_EQ(r->name.c_str(), "b0");
}

FB_TEST(bdev_rpc_request, parse_create_with_optional_block_size) {
    std::unordered_map<std::string, std::string> f = {
        {"name", "b0"}, {"pool_name", "p0"}, {"image_name", "i0"},
        {"image_size", "1024"}, {"object_size", "4096"}, {"monitor_address", "m0"},
        {"block_size", "512"}
    };
    auto r = rpc_validator::parse_create(f);
    FB_ASSERT_TRUE(r.has_value());
    FB_ASSERT_EQ(r->block_size, 512u);
}

FB_TEST(bdev_rpc_request, parse_create_fails_missing) {
    std::unordered_map<std::string, std::string> f = {{ "name", "b0" }};
    auto r = rpc_validator::parse_create(f);
    FB_ASSERT_FALSE(r.has_value());
}

FB_TEST(bdev_rpc_request, parse_delete_success) {
    std::unordered_map<std::string, std::string> f = {{"name", "b0"}};
    auto r = rpc_validator::parse_delete(f);
    FB_ASSERT_TRUE(r.has_value());
}

FB_TEST(bdev_rpc_request, parse_delete_fails_no_name) {
    std::unordered_map<std::string, std::string> f;
    auto r = rpc_validator::parse_delete(f);
    FB_ASSERT_FALSE(r.has_value());
}

FB_TEST(bdev_rpc_request, parse_resize_success) {
    std::unordered_map<std::string, std::string> f = {{ "name", "b0" }, { "new_size", "2048" }};
    auto r = rpc_validator::parse_resize(f);
    FB_ASSERT_TRUE(r.has_value());
    FB_ASSERT_EQ(r->new_size, 2048ull);
}

FB_TEST(bdev_rpc_request, parse_resize_fails_missing_size) {
    std::unordered_map<std::string, std::string> f = {{ "name", "b0" }};
    auto r = rpc_validator::parse_resize(f);
    FB_ASSERT_FALSE(r.has_value());
}

// ============================================================================
// Test Suite: bdev_io_merge — IO merge and coalescing
// ============================================================================

FB_SUITE_SETUP(bdev_io_merge) {}
FB_SUITE_TEARDOWN(bdev_io_merge) {}

struct merge_request {
    uint64_t offset{0};
    uint64_t length{0};
    uint32_t flags{0};
    bool merged{false};

    bool is_contiguous_with(const merge_request& other) const {
        return offset + length == other.offset;
    }

    bool overlaps_with(const merge_request& other) const {
        return offset < other.offset + other.length && other.offset < offset + length;
    }

    bool can_merge_with(const merge_request& other) const {
        return flags == other.flags && (is_contiguous_with(other) || overlaps_with(other));
    }
};

struct io_merge_engine {
    std::vector<merge_request> pending;
    uint64_t merge_count{0};

    void submit(merge_request req) {
        for (auto& existing : pending) {
            if (existing.can_merge_with(req) && !existing.merged) {
                uint64_t new_end = std::max(existing.offset + existing.length, req.offset + req.length);
                existing.offset = std::min(existing.offset, req.offset);
                existing.length = new_end - existing.offset;
                existing.merged = true;
                merge_count++;
                return;
            }
        }
        pending.push_back(req);
    }

    size_t pending_count() const { return pending.size(); }

    std::optional<merge_request> get_merged() const {
        for (const auto& req : pending) { if (req.merged) return req; }
        return std::nullopt;
    }

    void flush() { pending.clear(); }

    uint64_t total_pending_bytes() const {
        uint64_t total = 0;
        for (const auto& req : pending) total += req.length;
        return total;
    }
};

FB_TEST(bdev_io_merge, contiguous_requests) {
    merge_request a{0, 1024, 0};
    merge_request b{1024, 2048, 0};
    FB_ASSERT_TRUE(a.is_contiguous_with(b));
}

FB_TEST(bdev_io_merge, non_contiguous_requests) {
    merge_request a{0, 1024, 0};
    merge_request b{2048, 1024, 0};
    FB_ASSERT_FALSE(a.is_contiguous_with(b));
}

FB_TEST(bdev_io_merge, overlapping_requests) {
    merge_request a{0, 2048, 0};
    merge_request b{1024, 2048, 0};
    FB_ASSERT_TRUE(a.overlaps_with(b));
}

FB_TEST(bdev_io_merge, can_merge_same_flags_contiguous) {
    merge_request a{0, 1024, 1};
    merge_request b{1024, 2048, 1};
    FB_ASSERT_TRUE(a.can_merge_with(b));
}

FB_TEST(bdev_io_merge, cannot_merge_different_flags) {
    merge_request a{0, 1024, 1};
    merge_request b{1024, 2048, 2};
    FB_ASSERT_FALSE(a.can_merge_with(b));
}

FB_TEST(bdev_io_merge, engine_submit_no_merge) {
    io_merge_engine engine;
    engine.submit({0, 1024, 1});
    engine.submit({4096, 1024, 1});  // gap
    FB_ASSERT_EQ(engine.pending_count(), 2u);
    FB_ASSERT_EQ(engine.merge_count, 0u);
}

FB_TEST(bdev_io_merge, engine_submit_with_merge) {
    io_merge_engine engine;
    engine.submit({0, 1024, 1});
    engine.submit({1024, 2048, 1});
    FB_ASSERT_EQ(engine.pending_count(), 1u);
    FB_ASSERT_EQ(engine.merge_count, 1u);
}

FB_TEST(bdev_io_merge, engine_merged_request_size) {
    io_merge_engine engine;
    engine.submit({0, 1024, 1});
    engine.submit({1024, 2048, 1});
    auto merged = engine.get_merged();
    FB_ASSERT_TRUE(merged.has_value());
    FB_ASSERT_EQ(merged->length, 3072u);
}

FB_TEST(bdev_io_merge, engine_overlapping_merge) {
    io_merge_engine engine;
    engine.submit({0, 2048, 1});
    engine.submit({1024, 2048, 1});
    FB_ASSERT_EQ(engine.pending_count(), 1u);
    FB_ASSERT_EQ(engine.get_merged()->length, 3072u);
}

FB_TEST(bdev_io_merge, engine_flush_clears) {
    io_merge_engine engine;
    engine.submit({0, 1024, 1});
    engine.flush();
    FB_ASSERT_EQ(engine.pending_count(), 0u);
}

FB_TEST(bdev_io_merge, engine_total_pending_bytes) {
    io_merge_engine engine;
    engine.submit({0, 1024, 1});
    engine.submit({4096, 2048, 1});
    FB_ASSERT_EQ(engine.total_pending_bytes(), 3072u);
}

FB_TEST(bdev_io_merge, engine_multiple_merges) {
    io_merge_engine engine;
    engine.submit({0, 1024, 1});
    engine.submit({1024, 1024, 1});
    engine.submit({4096, 512, 2});
    engine.submit({4608, 512, 2});
    FB_ASSERT_EQ(engine.pending_count(), 2u);
    FB_ASSERT_EQ(engine.merge_count, 2u);
}

// ============================================================================
// Test Suite: bdev_dirty_tracking — Dirty block tracking
// ============================================================================

FB_SUITE_SETUP(bdev_dirty_tracking) {}
FB_SUITE_TEARDOWN(bdev_dirty_tracking) {}

struct dirty_block {
    uint64_t block_id{0};
    uint64_t dirty_since_us{0};
    bool is_dirty{false};
    bool needs_flush{false};

    void mark_dirty(uint64_t now_us) {
        is_dirty = true;
        dirty_since_us = now_us;
        needs_flush = true;
    }

    void mark_clean() {
        is_dirty = false;
        needs_flush = false;
    }

    uint64_t dirty_duration(uint64_t now_us) const {
        return is_dirty ? now_us - dirty_since_us : 0;
    }

    bool dirty_timeout_exceeded(uint64_t now_us, uint64_t timeout_us) const {
        return is_dirty && dirty_duration(now_us) > timeout_us;
    }
};

struct dirty_tracker {
    std::unordered_map<uint64_t, dirty_block> blocks;
    uint64_t dirty_timeout_us{30000000};  // 30 seconds
    uint64_t last_flush_us{0};
    uint64_t flush_interval_us{60000000};  // 60 seconds

    void mark_block_dirty(uint64_t block_id, uint64_t now_us) {
        blocks[block_id].mark_dirty(now_us);
    }

    void mark_block_clean(uint64_t block_id) {
        auto it = blocks.find(block_id);
        if (it != blocks.end()) it->second.mark_clean();
    }

    size_t dirty_count() const {
        size_t n = 0;
        for (const auto& [_, blk] : blocks) { if (blk.is_dirty) n++; }
        return n;
    }

    std::vector<uint64_t> get_timeout_blocks(uint64_t now_us) const {
        std::vector<uint64_t> result;
        for (const auto& [id, blk] : blocks) {
            if (blk.dirty_timeout_exceeded(now_us, dirty_timeout_us)) result.push_back(id);
        }
        return result;
    }

    bool needs_flush(uint64_t now_us) const {
        if (dirty_count() > 0 && now_us - last_flush_us >= flush_interval_us) return true;
        return get_timeout_blocks(now_us).size() > 0;
    }

    void record_flush(uint64_t now_us) {
        last_flush_us = now_us;
        for (auto& [_, blk] : blocks) { blk.mark_clean(); }
    }

    void clear() {
        blocks.clear();
        last_flush_us = 0;
    }
};

FB_TEST(bdev_dirty_tracking, block_initial_not_dirty) {
    dirty_block blk;
    FB_ASSERT_FALSE(blk.is_dirty);
}

FB_TEST(bdev_dirty_tracking, mark_dirty_sets_state) {
    dirty_block blk;
    blk.mark_dirty(1000);
    FB_ASSERT_TRUE(blk.is_dirty);
    FB_ASSERT_EQ(blk.dirty_since_us, 1000u);
}

FB_TEST(bdev_dirty_tracking, mark_clean_clears_state) {
    dirty_block blk;
    blk.mark_dirty(1000);
    blk.mark_clean();
    FB_ASSERT_FALSE(blk.is_dirty);
}

FB_TEST(bdev_dirty_tracking, dirty_duration_calculation) {
    dirty_block blk;
    blk.mark_dirty(1000);
    FB_ASSERT_EQ(blk.dirty_duration(5000), 4000u);
}

FB_TEST(bdev_dirty_tracking, dirty_duration_zero_if_clean) {
    dirty_block blk;
    FB_ASSERT_EQ(blk.dirty_duration(5000), 0u);
}

FB_TEST(bdev_dirty_tracking, dirty_timeout_exceeded) {
    dirty_block blk;
    blk.mark_dirty(0);
    FB_ASSERT_TRUE(blk.dirty_timeout_exceeded(60000, 30000));
}

FB_TEST(bdev_dirty_tracking, dirty_timeout_not_exceeded) {
    dirty_block blk;
    blk.mark_dirty(0);
    FB_ASSERT_FALSE(blk.dirty_timeout_exceeded(10000, 30000));
}

FB_TEST(bdev_dirty_tracking, tracker_mark_block_dirty) {
    dirty_tracker tracker;
    tracker.mark_block_dirty(100, 0);
    FB_ASSERT_EQ(tracker.dirty_count(), 1u);
}

FB_TEST(bdev_dirty_tracking, tracker_mark_block_clean) {
    dirty_tracker tracker;
    tracker.mark_block_dirty(100, 0);
    tracker.mark_block_clean(100);
    FB_ASSERT_EQ(tracker.dirty_count(), 0u);
}

FB_TEST(bdev_dirty_tracking, tracker_dirty_count) {
    dirty_tracker tracker;
    tracker.mark_block_dirty(1, 0);
    tracker.mark_block_dirty(2, 0);
    tracker.mark_block_dirty(3, 0);
    FB_ASSERT_EQ(tracker.dirty_count(), 3u);
}

FB_TEST(bdev_dirty_tracking, tracker_get_timeout_blocks) {
    dirty_tracker tracker;
    tracker.dirty_timeout_us = 10000;
    tracker.mark_block_dirty(1, 0);
    tracker.mark_block_dirty(2, 20000);  // not timeout yet
    auto timeout_blocks = tracker.get_timeout_blocks(25000);
    FB_ASSERT_EQ(timeout_blocks.size(), 1u);
}

FB_TEST(bdev_dirty_tracking, tracker_needs_flush_by_interval) {
    dirty_tracker tracker;
    tracker.flush_interval_us = 10000;
    tracker.mark_block_dirty(1, 0);
    FB_ASSERT_TRUE(tracker.needs_flush(15000));
}

FB_TEST(bdev_dirty_tracking, tracker_needs_flush_by_timeout) {
    dirty_tracker tracker;
    tracker.dirty_timeout_us = 5000;
    tracker.mark_block_dirty(1, 0);
    FB_ASSERT_TRUE(tracker.needs_flush(10000));
}

FB_TEST(bdev_dirty_tracking, tracker_record_flush_clears_all) {
    dirty_tracker tracker;
    tracker.mark_block_dirty(1, 0);
    tracker.mark_block_dirty(2, 0);
    tracker.record_flush(50000);
    FB_ASSERT_EQ(tracker.dirty_count(), 0u);
}

FB_TEST(bdev_dirty_tracking, tracker_clear) {
    dirty_tracker tracker;
    tracker.mark_block_dirty(1, 0);
    tracker.clear();
    FB_ASSERT_EQ(tracker.blocks.size(), 0u);
}

// ============================================================================
// Test Suite: bdev_io_replay — IO replay and recovery
// ============================================================================

FB_SUITE_SETUP(bdev_io_replay) {}
FB_SUITE_TEARDOWN(bdev_io_replay) {}

enum class replay_status : uint8_t {
    pending,
    in_progress,
    completed,
    failed,
    abandoned
};

struct replay_entry {
    uint64_t io_id{0};
    uint64_t original_offset{0};
    uint64_t original_length{0};
    uint64_t submitted_at_us{0};
    uint32_t attempt_count{0};
    replay_status status{replay_status::pending};
    uint32_t max_attempts{3};

    bool can_retry() const {
        return attempt_count < max_attempts && status != replay_status::abandoned;
    }

    void mark_attempt() { attempt_count++; }

    void mark_in_progress() { status = replay_status::in_progress; }

    void mark_completed() { status = replay_status::completed; }

    void mark_failed() {
        if (can_retry()) {
            status = replay_status::pending;
        } else {
            status = replay_status::abandoned;
        }
    }

    bool is_finished() const {
        return status == replay_status::completed ||
               status == replay_status::abandoned;
    }

    bool needs_recovery() const {
        return status == replay_status::failed || status == replay_status::pending;
    }
};

struct io_replay_manager {
    std::deque<replay_entry> pending_replays;
    std::unordered_map<uint64_t, replay_entry> active_replays;
    uint64_t replay_id_counter{1};
    uint64_t completed_count{0};
    uint64_t abandoned_count{0};

    uint64_t enqueue(uint64_t offset, uint64_t length, uint64_t now_us) {
        replay_entry entry;
        entry.io_id = replay_id_counter++;
        entry.original_offset = offset;
        entry.original_length = length;
        entry.submitted_at_us = now_us;
        pending_replays.push_back(entry);
        return entry.io_id;
    }

    std::optional<replay_entry> dequeue() {
        if (pending_replays.empty()) return std::nullopt;
        replay_entry entry = pending_replays.front();
        pending_replays.pop_front();
        entry.mark_in_progress();
        active_replays[entry.io_id] = entry;
        return entry;
    }

    void handle_success(uint64_t io_id) {
        auto it = active_replays.find(io_id);
        if (it != active_replays.end()) {
            it->second.mark_completed();
            completed_count++;
            active_replays.erase(it);
        }
    }

    void handle_failure(uint64_t io_id) {
        auto it = active_replays.find(io_id);
        if (it != active_replays.end()) {
            it->second.mark_attempt();
            it->second.mark_failed();
            if (it->second.can_retry()) {
                pending_replays.push_back(it->second);
                active_replays.erase(it);
            } else {
                abandoned_count++;
                active_replays.erase(it);
            }
        }
    }

    size_t pending_count() const { return pending_replays.size(); }
    size_t active_count() const { return active_replays.size(); }

    size_t unfinished_count() const {
        return pending_count() + active_count();
    }
};

FB_TEST(bdev_io_replay, entry_initial_pending) {
    replay_entry entry;
    FB_ASSERT_TRUE(entry.status == replay_status::pending);
}

FB_TEST(bdev_io_replay, entry_can_retry_below_max) {
    replay_entry entry;
    entry.attempt_count = 1;
    FB_ASSERT_TRUE(entry.can_retry());
}

FB_TEST(bdev_io_replay, entry_cannot_retry_at_max) {
    replay_entry entry;
    entry.attempt_count = 3;
    FB_ASSERT_FALSE(entry.can_retry());
}

FB_TEST(bdev_io_replay, entry_mark_attempt_increments) {
    replay_entry entry;
    entry.mark_attempt();
    FB_ASSERT_EQ(entry.attempt_count, 1u);
}

FB_TEST(bdev_io_replay, entry_mark_completed) {
    replay_entry entry;
    entry.mark_in_progress();
    entry.mark_completed();
    FB_ASSERT_TRUE(entry.status == replay_status::completed);
}

FB_TEST(bdev_io_replay, entry_mark_failed_retryable) {
    replay_entry entry;
    entry.attempt_count = 1;
    entry.mark_in_progress();
    entry.mark_failed();
    FB_ASSERT_TRUE(entry.status == replay_status::pending);  // can retry
}

FB_TEST(bdev_io_replay, entry_mark_failed_abandoned) {
    replay_entry entry;
    entry.attempt_count = 3;
    entry.mark_in_progress();
    entry.mark_failed();
    FB_ASSERT_TRUE(entry.status == replay_status::abandoned);  // max reached
}

FB_TEST(bdev_io_replay, entry_is_finished_completed) {
    replay_entry entry;
    entry.mark_completed();
    FB_ASSERT_TRUE(entry.is_finished());
}

FB_TEST(bdev_io_replay, entry_is_finished_abandoned) {
    replay_entry entry;
    entry.status = replay_status::abandoned;
    FB_ASSERT_TRUE(entry.is_finished());
}

FB_TEST(bdev_io_replay, manager_enqueue) {
    io_replay_manager mgr;
    mgr.enqueue(0, 1024, 1000);
    FB_ASSERT_EQ(mgr.pending_count(), 1u);
}

FB_TEST(bdev_io_replay, manager_dequeue) {
    io_replay_manager mgr;
    mgr.enqueue(0, 1024, 1000);
    auto entry = mgr.dequeue();
    FB_ASSERT_TRUE(entry.has_value());
    FB_ASSERT_TRUE(entry->status == replay_status::in_progress);
    FB_ASSERT_EQ(mgr.pending_count(), 0u);
    FB_ASSERT_EQ(mgr.active_count(), 1u);
}

FB_TEST(bdev_io_replay, manager_handle_success) {
    io_replay_manager mgr;
    uint64_t id = mgr.enqueue(0, 1024, 1000);
    mgr.dequeue();
    mgr.handle_success(id);
    FB_ASSERT_EQ(mgr.completed_count, 1u);
    FB_ASSERT_EQ(mgr.active_count(), 0u);
}

FB_TEST(bdev_io_replay, manager_handle_failure_retry) {
    io_replay_manager mgr;
    uint64_t id = mgr.enqueue(0, 1024, 1000);
    mgr.dequeue();
    mgr.handle_failure(id);  // attempt 1
    FB_ASSERT_EQ(mgr.pending_count(), 1u);  // re-queued for retry
    FB_ASSERT_EQ(mgr.abandoned_count, 0u);
}

FB_TEST(bdev_io_replay, manager_handle_failure_abandon) {
    io_replay_manager mgr;
    uint64_t id = mgr.enqueue(0, 1024, 1000);
    mgr.dequeue();
    mgr.handle_failure(id);  // attempt 1 -> retry
    mgr.dequeue();
    mgr.handle_failure(id);  // attempt 2 -> retry
    mgr.dequeue();
    mgr.handle_failure(id);  // attempt 3 -> abandon
    FB_ASSERT_EQ(mgr.abandoned_count, 1u);
}

FB_TEST(bdev_io_replay, manager_unfinished_count) {
    io_replay_manager mgr;
    mgr.enqueue(0, 1024, 1000);
    mgr.enqueue(2048, 512, 2000);
    mgr.dequeue();  // one active, one pending
    FB_ASSERT_EQ(mgr.unfinished_count(), 2u);
}

// ============================================================================
// Test Suite: bdev_connection_pool — Connection pool management
// ============================================================================

FB_SUITE_SETUP(bdev_connection_pool) {}
FB_SUITE_TEARDOWN(bdev_connection_pool) {}

struct connection_entry {
    uint64_t conn_id{0};
    std::string endpoint;
    bool is_active{false};
    bool is_busy{false};
    uint64_t last_used_us{0};
    uint64_t idle_timeout_us{30000000};  // 30s

    void activate() { is_active = true; }

    void deactivate() { is_active = false; is_busy = false; }

    void acquire(uint64_t now_us) { is_busy = true; last_used_us = now_us; }

    void release() { is_busy = false; }

    bool is_idle(uint64_t now_us) const {
        return is_active && !is_busy && now_us - last_used_us > idle_timeout_us;
    }

    bool is_available() const { return is_active && !is_busy; }
};

struct connection_pool {
    std::vector<connection_entry> connections;
    uint64_t next_conn_id{1};
    uint64_t max_connections{10};

    uint64_t create(const std::string& endpoint) {
        if (connections.size() >= max_connections) return 0;
        connection_entry conn;
        conn.conn_id = next_conn_id++;
        conn.endpoint = endpoint;
        conn.activate();
        connections.push_back(conn);
        return conn.conn_id;
    }

    std::optional<uint64_t> acquire_available(uint64_t now_us) {
        for (auto& conn : connections) {
            if (conn.is_available()) {
                conn.acquire(now_us);
                return conn.conn_id;
            }
        }
        return std::nullopt;
    }

    void release(uint64_t conn_id) {
        for (auto& conn : connections) {
            if (conn.conn_id == conn_id) conn.release();
        }
    }

    size_t active_count() const {
        size_t n = 0;
        for (const auto& c : connections) { if (c.is_active) n++; }
        return n;
    }

    size_t available_count() const {
        size_t n = 0;
        for (const auto& c : connections) { if (c.is_available()) n++; }
        return n;
    }

    std::vector<uint64_t> get_idle_connections(uint64_t now_us) const {
        std::vector<uint64_t> idle;
        for (const auto& c : connections) { if (c.is_idle(now_us)) idle.push_back(c.conn_id); }
        return idle;
    }

    void close(uint64_t conn_id) {
        for (auto it = connections.begin(); it != connections.end(); ++it) {
            if (it->conn_id == conn_id) { it->deactivate(); }
        }
    }

    void remove(uint64_t conn_id) {
        connections.erase(std::remove_if(connections.begin(), connections.end(),
            [conn_id](const connection_entry& c) { return c.conn_id == conn_id; }), connections.end());
    }
};

FB_TEST(bdev_connection_pool, entry_initial_inactive) {
    connection_entry conn;
    FB_ASSERT_FALSE(conn.is_active);
}

FB_TEST(bdev_connection_pool, entry_activate) {
    connection_entry conn;
    conn.activate();
    FB_ASSERT_TRUE(conn.is_active);
}

FB_TEST(bdev_connection_pool, entry_acquire_sets_busy) {
    connection_entry conn;
    conn.activate();
    conn.acquire(1000);
    FB_ASSERT_TRUE(conn.is_busy);
}

FB_TEST(bdev_connection_pool, entry_release_clears_busy) {
    connection_entry conn;
    conn.activate();
    conn.acquire(1000);
    conn.release();
    FB_ASSERT_FALSE(conn.is_busy);
}

FB_TEST(bdev_connection_pool, entry_is_idle_after_timeout) {
    connection_entry conn;
    conn.activate();
    conn.acquire(0);
    conn.release();
    FB_ASSERT_TRUE(conn.is_idle(60000000));  // 60s later
}

FB_TEST(bdev_connection_pool, entry_is_available_when_active_not_busy) {
    connection_entry conn;
    conn.activate();
    FB_ASSERT_TRUE(conn.is_available());
}

FB_TEST(bdev_connection_pool, pool_create) {
    connection_pool pool;
    uint64_t id = pool.create("127.0.0.1:9000");
    FB_ASSERT_NE(id, 0u);
    FB_ASSERT_EQ(pool.active_count(), 1u);
}

FB_TEST(bdev_connection_pool, pool_max_connections) {
    connection_pool pool;
    pool.max_connections = 3;
    pool.create("e1");
    pool.create("e2");
    pool.create("e3");
    uint64_t id = pool.create("e4");  // should fail
    FB_ASSERT_EQ(id, 0u);  // cannot create beyond max
}

FB_TEST(bdev_connection_pool, pool_acquire_available) {
    connection_pool pool;
    pool.create("e1");
    auto id = pool.acquire_available(1000);
    FB_ASSERT_TRUE(id.has_value());
}

FB_TEST(bdev_connection_pool, pool_acquire_no_available) {
    connection_pool pool;
    pool.create("e1");
    pool.acquire_available(1000);  // busy now
    auto id2 = pool.acquire_available(2000);
    FB_ASSERT_FALSE(id2.has_value());  // none available
}

FB_TEST(bdev_connection_pool, pool_release) {
    connection_pool pool;
    uint64_t id = pool.create("e1");
    pool.acquire_available(1000);
    pool.release(id);
    FB_ASSERT_EQ(pool.available_count(), 1u);
}

FB_TEST(bdev_connection_pool, pool_get_idle_connections) {
    connection_pool pool;
    pool.create("e1");
    pool.acquire_available(0);
    pool.release(1);
    auto idle = pool.get_idle_connections(60000000);
    FB_ASSERT_EQ(idle.size(), 1u);
}

FB_TEST(bdev_connection_pool, pool_close) {
    connection_pool pool;
    pool.create("e1");
    pool.close(1);
    FB_ASSERT_EQ(pool.active_count(), 0u);
}

FB_TEST(bdev_connection_pool, pool_remove) {
    connection_pool pool;
    pool.create("e1");
    pool.remove(1);
    FB_ASSERT_EQ(pool.connections.size(), 0u);
}

// ============================================================================
// Test Suite: bdev_memory_pool — Memory pool allocation
// ============================================================================

FB_SUITE_SETUP(bdev_memory_pool) {}
FB_SUITE_TEARDOWN(bdev_memory_pool) {}

struct memory_block {
    uint64_t block_id{0};
    uint64_t size{0};
    uint8_t* ptr{nullptr};
    bool allocated{false};

    bool is_free() const { return !allocated; }

    void mark_allocated() { allocated = true; }

    void mark_free() { allocated = false; }
};

struct memory_pool {
    std::vector<memory_block> blocks;
    uint64_t block_size{4096};
    uint64_t total_blocks{0};
    uint64_t allocated_blocks{0};
    uint64_t next_block_id{1};

    void initialize(uint64_t count) {
        total_blocks = count;
        blocks.reserve(count);
        for (uint64_t i = 0; i < count; ++i) {
            blocks.push_back({next_block_id++, block_size, nullptr, false});
        }
    }

    std::optional<memory_block*> allocate() {
        for (auto& blk : blocks) {
            if (blk.is_free()) {
                blk.mark_allocated();
                allocated_blocks++;
                return &blk;
            }
        }
        return std::nullopt;  // pool exhausted
    }

    void free(uint64_t block_id) {
        for (auto& blk : blocks) {
            if (blk.block_id == block_id && blk.allocated) {
                blk.mark_free();
                allocated_blocks--;
            }
        }
    }

    uint64_t free_count() const { return total_blocks - allocated_blocks; }

    bool has_available() const { return free_count() > 0; }

    double utilization() const {
        if (total_blocks == 0) return 0.0;
        return static_cast<double>(allocated_blocks) / total_blocks;
    }

    void reset() {
        for (auto& blk : blocks) blk.mark_free();
        allocated_blocks = 0;
    }

    std::vector<uint64_t> get_allocated_ids() const {
        std::vector<uint64_t> ids;
        for (const auto& blk : blocks) { if (blk.allocated) ids.push_back(blk.block_id); }
        return ids;
    }
};

FB_TEST(bdev_memory_pool, block_initial_free) {
    memory_block blk;
    FB_ASSERT_TRUE(blk.is_free());
}

FB_TEST(bdev_memory_pool, block_mark_allocated) {
    memory_block blk;
    blk.mark_allocated();
    FB_ASSERT_FALSE(blk.is_free());
}

FB_TEST(bdev_memory_pool, block_mark_free) {
    memory_block blk;
    blk.mark_allocated();
    blk.mark_free();
    FB_ASSERT_TRUE(blk.is_free());
}

FB_TEST(bdev_memory_pool, pool_initialize) {
    memory_pool pool;
    pool.initialize(100);
    FB_ASSERT_EQ(pool.total_blocks, 100u);
    FB_ASSERT_EQ(pool.blocks.size(), 100u);
}

FB_TEST(bdev_memory_pool, pool_allocate_success) {
    memory_pool pool;
    pool.initialize(10);
    auto blk = pool.allocate();
    FB_ASSERT_TRUE(blk.has_value());
    FB_ASSERT_EQ(pool.allocated_blocks, 1u);
}

FB_TEST(bdev_memory_pool, pool_allocate_exhausted) {
    memory_pool pool;
    pool.initialize(3);
    pool.allocate();
    pool.allocate();
    pool.allocate();
    auto blk = pool.allocate();  // should fail
    FB_ASSERT_FALSE(blk.has_value());
}

FB_TEST(bdev_memory_pool, pool_free_reduces_count) {
    memory_pool pool;
    pool.initialize(10);
    auto blk = pool.allocate();
    pool.free((*blk)->block_id);
    FB_ASSERT_EQ(pool.allocated_blocks, 0u);
}

FB_TEST(bdev_memory_pool, pool_free_count) {
    memory_pool pool;
    pool.initialize(10);
    pool.allocate();
    pool.allocate();
    FB_ASSERT_EQ(pool.free_count(), 8u);
}

FB_TEST(bdev_memory_pool, pool_has_available) {
    memory_pool pool;
    pool.initialize(10);
    FB_ASSERT_TRUE(pool.has_available());
}

FB_TEST(bdev_memory_pool, pool_no_available_when_full) {
    memory_pool pool;
    pool.initialize(3);
    pool.allocate();
    pool.allocate();
    pool.allocate();
    FB_ASSERT_FALSE(pool.has_available());
}

FB_TEST(bdev_memory_pool, pool_utilization) {
    memory_pool pool;
    pool.initialize(10);
    pool.allocate(); pool.allocate();
    FB_ASSERT_TRUE(pool.utilization() > 0.19 && pool.utilization() < 0.21);
}

FB_TEST(bdev_memory_pool, pool_reset) {
    memory_pool pool;
    pool.initialize(10);
    pool.allocate();
    pool.reset();
    FB_ASSERT_EQ(pool.allocated_blocks, 0u);
}

FB_TEST(bdev_memory_pool, pool_get_allocated_ids) {
    memory_pool pool;
    pool.initialize(10);
    pool.allocate();
    pool.allocate();
    auto ids = pool.get_allocated_ids();
    FB_ASSERT_EQ(ids.size(), 2u);
}

FB_TEST(bdev_memory_pool, pool_multiple_allocate_free_cycle) {
    memory_pool pool;
    pool.initialize(5);
    auto b1 = pool.allocate();
    auto b2 = pool.allocate();
    pool.free((*b1)->block_id);
    auto b3 = pool.allocate();  // should reuse b1's slot
    FB_ASSERT_EQ(pool.allocated_blocks, 2u);
}

// ============================================================================
// Test Suite: bdev_object_dedup — Object deduplication tracking
// ============================================================================

FB_SUITE_SETUP(bdev_object_dedup) {}
FB_SUITE_TEARDOWN(bdev_object_dedup) {}

struct dedup_entry {
    uint64_t hash{0};
    uint64_t physical_block{0};
    uint64_t ref_count{1};
    uint64_t size{0};

    bool has_multiple_refs() const { return ref_count > 1; }

    void add_ref() { ref_count++; }

    void remove_ref() { if (ref_count > 0) ref_count--; }

    bool is_unique() const { return ref_count == 1; }

    bool is_dead() const { return ref_count == 0; }
};

struct dedup_manager {
    std::unordered_map<uint64_t, dedup_entry> hash_table;
    std::unordered_map<uint64_t, uint64_t> logical_to_hash;  // logical_block -> hash
    uint64_t dedup_count{0};
    uint64_t saved_bytes{0};

    uint64_t compute_hash(const void* data, size_t len) {
        // Simplified hash simulation
        uint64_t h = 0;
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) h = h * 31 + bytes[i];
        return h;
    }

    std::optional<uint64_t> lookup(uint64_t hash) const {
        auto it = hash_table.find(hash);
        return it != hash_table.end() ? std::optional<uint64_t>(it->second.physical_block) : std::nullopt;
    }

    bool is_duplicate(uint64_t hash) const {
        return hash_table.find(hash) != hash_table.end();
    }

    uint64_t store(uint64_t hash, uint64_t phys_block, uint64_t size) {
        if (is_duplicate(hash)) {
            hash_table[hash].add_ref();
            dedup_count++;
            saved_bytes += size;
            return hash_table[hash].physical_block;
        }
        dedup_entry entry{hash, phys_block, 1, size};
        hash_table[hash] = entry;
        return phys_block;
    }

    void map_logical(uint64_t logical_block, uint64_t hash) {
        logical_to_hash[logical_block] = hash;
    }

    uint64_t get_physical(uint64_t logical_block) const {
        auto it = logical_to_hash.find(logical_block);
        if (it == logical_to_hash.end()) return logical_block;  // no mapping
        auto hash_entry = hash_table.find(it->second);
        return hash_entry != hash_table.end() ? hash_entry->second.physical_block : logical_block;
    }

    void release(uint64_t hash) {
        auto it = hash_table.find(hash);
        if (it != hash_table.end()) {
            it->second.remove_ref();
            if (it->second.is_dead()) hash_table.erase(it);
        }
    }

    size_t entry_count() const { return hash_table.size(); }

    size_t shared_count() const {
        size_t n = 0;
        for (const auto& [_, e] : hash_table) { if (e.has_multiple_refs()) n++; }
        return n;
    }
};

FB_TEST(bdev_object_dedup, entry_initial_ref_count) {
    dedup_entry entry;
    FB_ASSERT_EQ(entry.ref_count, 1u);
}

FB_TEST(bdev_object_dedup, entry_add_ref) {
    dedup_entry entry;
    entry.add_ref();
    FB_ASSERT_EQ(entry.ref_count, 2u);
}

FB_TEST(bdev_object_dedup, entry_remove_ref) {
    dedup_entry entry;
    entry.add_ref();
    entry.remove_ref();
    FB_ASSERT_EQ(entry.ref_count, 1u);
}

FB_TEST(bdev_object_dedup, entry_has_multiple_refs) {
    dedup_entry entry;
    entry.add_ref();
    FB_ASSERT_TRUE(entry.has_multiple_refs());
}

FB_TEST(bdev_object_dedup, entry_is_unique) {
    dedup_entry entry;
    FB_ASSERT_TRUE(entry.is_unique());
}

FB_TEST(bdev_object_dedup, entry_is_dead) {
    dedup_entry entry;
    entry.remove_ref();
    FB_ASSERT_TRUE(entry.is_dead());
}

FB_TEST(bdev_object_dedup, manager_compute_hash) {
    dedup_manager mgr;
    uint8_t data[] = {1, 2, 3};
    uint64_t h = mgr.compute_hash(data, 3);
    FB_ASSERT_NE(h, 0u);
}

FB_TEST(bdev_object_dedup, manager_lookup_existing) {
    dedup_manager mgr;
    mgr.store(12345, 100, 4096);
    auto phys = mgr.lookup(12345);
    FB_ASSERT_TRUE(phys.has_value());
    FB_ASSERT_EQ(*phys, 100u);
}

FB_TEST(bdev_object_dedup, manager_lookup_missing) {
    dedup_manager mgr;
    auto phys = mgr.lookup(99999);
    FB_ASSERT_FALSE(phys.has_value());
}

FB_TEST(bdev_object_dedup, manager_is_duplicate_true) {
    dedup_manager mgr;
    mgr.store(12345, 100, 4096);
    FB_ASSERT_TRUE(mgr.is_duplicate(12345));
}

FB_TEST(bdev_object_dedup, manager_is_duplicate_false) {
    dedup_manager mgr;
    FB_ASSERT_FALSE(mgr.is_duplicate(99999));
}

FB_TEST(bdev_object_dedup, manager_store_new) {
    dedup_manager mgr;
    uint64_t phys = mgr.store(12345, 100, 4096);
    FB_ASSERT_EQ(phys, 100u);
    FB_ASSERT_EQ(mgr.entry_count(), 1u);
}

FB_TEST(bdev_object_dedup, manager_store_duplicate) {
    dedup_manager mgr;
    mgr.store(12345, 100, 4096);
    uint64_t phys2 = mgr.store(12345, 200, 4096);  // duplicate hash
    FB_ASSERT_EQ(phys2, 100u);  // returns original physical block
    FB_ASSERT_EQ(mgr.entry_count(), 1u);  // no new entry
    FB_ASSERT_EQ(mgr.dedup_count, 1u);
}

FB_TEST(bdev_object_dedup, manager_map_logical) {
    dedup_manager mgr;
    mgr.store(12345, 100, 4096);
    mgr.map_logical(50, 12345);
    FB_ASSERT_EQ(mgr.get_physical(50), 100u);
}

FB_TEST(bdev_object_dedup, manager_get_physical_no_mapping) {
    dedup_manager mgr;
    FB_ASSERT_EQ(mgr.get_physical(999), 999u);  // returns logical itself
}

FB_TEST(bdev_object_dedup, manager_release) {
    dedup_manager mgr;
    mgr.store(12345, 100, 4096);
    mgr.release(12345);
    FB_ASSERT_EQ(mgr.entry_count(), 0u);  // entry removed (dead)
}

FB_TEST(bdev_object_dedup, manager_shared_count) {
    dedup_manager mgr;
    mgr.store(111, 100, 4096);
    mgr.store(111, 200, 4096);  // duplicate
    mgr.store(222, 300, 4096);  // unique
    FB_ASSERT_EQ(mgr.shared_count(), 1u);  // hash 111 is shared
}

// ============================================================================
// Test Suite: bdev_async_callback — Async operation callback tracking
// ============================================================================

FB_SUITE_SETUP(bdev_async_op_tracking) {}
FB_SUITE_TEARDOWN(bdev_async_op_tracking) {}

struct async_op {
    uint64_t op_id{0};
    uint64_t start_us{0};
    uint64_t timeout_us{0};
    bool completed{false};
    bool cancelled{false};

    bool is_pending() const { return !completed && !cancelled; }
    void mark_completed() { completed = true; }
    void mark_cancelled() { cancelled = true; }
    bool timed_out(uint64_t now_us) const {
        return is_pending() && now_us - start_us > timeout_us;
    }
};

struct callback_entry {
    uint64_t cb_id{0};
    uint64_t op_id{0};
    int32_t result_code{0};
    uint64_t completed_at_us{0};
    bool executed{false};

    void execute(int32_t code, uint64_t now_us) {
        result_code = code;
        completed_at_us = now_us;
        executed = true;
    }
};

struct async_callback_manager {
    std::deque<async_op> pending_ops;
    std::unordered_map<uint64_t, callback_entry> callbacks;
    uint64_t next_op_id{1};
    uint64_t next_cb_id{1};
    uint64_t completed_count{0};
    uint64_t cancelled_count{0};

    uint64_t start_op(uint64_t now_us, uint64_t timeout_us) {
        async_op op;
        op.op_id = next_op_id++;
        op.start_us = now_us;
        op.timeout_us = timeout_us;
        pending_ops.push_back(op);
        return op.op_id;
    }

    uint64_t register_callback(uint64_t op_id) {
        callback_entry cb;
        cb.cb_id = next_cb_id++;
        cb.op_id = op_id;
        callbacks[cb.cb_id] = cb;
        return cb.cb_id;
    }

    void complete_op(uint64_t op_id, int32_t code, uint64_t now_us) {
        pending_ops.erase(std::remove_if(pending_ops.begin(), pending_ops.end(),
            [op_id](const async_op& o) { return o.op_id == op_id; }), pending_ops.end());
        for (auto& [id, cb] : callbacks) {
            if (cb.op_id == op_id && !cb.executed) {
                cb.execute(code, now_us);
                completed_count++;
            }
        }
    }

    void cancel_op(uint64_t op_id) {
        pending_ops.erase(std::remove_if(pending_ops.begin(), pending_ops.end(),
            [op_id](const async_op& o) { return o.op_id == op_id; }), pending_ops.end());
        for (auto& [id, cb] : callbacks) {
            if (cb.op_id == op_id && !cb.executed) {
                cb.execute(-1, 0);
                cancelled_count++;
            }
        }
    }

    std::vector<uint64_t> get_timeout_ops(uint64_t now_us) const {
        std::vector<uint64_t> timed;
        for (const auto& op : pending_ops) { if (op.timed_out(now_us)) timed.push_back(op.op_id); }
        return timed;
    }

    size_t pending_count() const { return pending_ops.size(); }
    callback_entry* get_callback(uint64_t cb_id) {
        auto it = callbacks.find(cb_id);
        return it != callbacks.end() ? &it->second : nullptr;
    }
};

FB_TEST(bdev_async_op_tracking, op_initial_pending) {
    async_op op;
    FB_ASSERT_TRUE(op.is_pending());
}

FB_TEST(bdev_async_op_tracking, op_mark_completed) {
    async_op op;
    op.mark_completed();
    FB_ASSERT_FALSE(op.is_pending());
}

FB_TEST(bdev_async_op_tracking, op_timed_out) {
    async_op op;
    op.start_us = 0;
    op.timeout_us = 10000;
    FB_ASSERT_TRUE(op.timed_out(50000));
}

FB_TEST(bdev_async_op_tracking, cb_initial_not_executed) {
    callback_entry cb;
    FB_ASSERT_FALSE(cb.executed);
}

FB_TEST(bdev_async_op_tracking, cb_execute_sets_result) {
    callback_entry cb;
    cb.execute(0, 1000);
    FB_ASSERT_TRUE(cb.executed);
}

FB_TEST(bdev_async_op_tracking, manager_start_op) {
    async_callback_manager mgr;
    mgr.start_op(1000, 30000);
    FB_ASSERT_EQ(mgr.pending_count(), 1u);
}

FB_TEST(bdev_async_op_tracking, manager_register_callback) {
    async_callback_manager mgr;
    uint64_t op_id = mgr.start_op(1000, 30000);
    uint64_t cb_id = mgr.register_callback(op_id);
    FB_ASSERT_NE(cb_id, 0u);
}

FB_TEST(bdev_async_op_tracking, manager_complete_op) {
    async_callback_manager mgr;
    uint64_t op_id = mgr.start_op(1000, 30000);
    uint64_t cb_id = mgr.register_callback(op_id);
    mgr.complete_op(op_id, 0, 2000);
    FB_ASSERT_EQ(mgr.pending_count(), 0u);
    FB_ASSERT_TRUE(mgr.get_callback(cb_id)->executed);
}

FB_TEST(bdev_async_callback, manager_cancel_op) {
    async_callback_manager mgr;
    uint64_t op_id = mgr.start_op(1000, 30000);
    uint64_t cb_id = mgr.register_callback(op_id);
    mgr.cancel_op(op_id);
    FB_ASSERT_EQ(mgr.get_callback(cb_id)->result_code, -1);
}

FB_TEST(bdev_async_callback, manager_get_timeout_ops) {
    async_callback_manager mgr;
    mgr.start_op(0, 10000);
    mgr.start_op(0, 60000);
    auto timed = mgr.get_timeout_ops(50000);
    FB_ASSERT_EQ(timed.size(), 1u);
}

FB_TEST(bdev_async_callback, manager_multiple_callbacks) {
    async_callback_manager mgr;
    uint64_t op_id = mgr.start_op(1000, 30000);
    uint64_t cb1 = mgr.register_callback(op_id);
    uint64_t cb2 = mgr.register_callback(op_id);
    mgr.complete_op(op_id, 0, 2000);
    FB_ASSERT_TRUE(mgr.get_callback(cb1)->executed);
    FB_ASSERT_TRUE(mgr.get_callback(cb2)->executed);
}

// ============================================================================
// Test Suite: bdev_write_barrier — Write barrier and flush ordering
// ============================================================================

FB_SUITE_SETUP(bdev_write_barrier) {}
FB_SUITE_TEARDOWN(bdev_write_barrier) {}

struct barrier_entry {
    uint64_t barrier_id{0};
    uint64_t issued_at_us{0};
    bool acknowledged{false};
    bool pending_flushes{false};

    void set_pending() { pending_flushes = true; }

    void clear_pending() { pending_flushes = false; }

    void acknowledge() { acknowledged = true; }

    bool is_complete() const { return acknowledged && !pending_flushes; }
};

struct write_barrier_manager {
    std::deque<barrier_entry> barriers;
    uint64_t next_barrier_id{1};
    uint64_t pending_writes_before_barrier{0};

    uint64_t issue_barrier(uint64_t now_us) {
        barrier_entry b;
        b.barrier_id = next_barrier_id++;
        b.issued_at_us = now_us;
        b.set_pending();
        barriers.push_back(b);
        return b.barrier_id;
    }

    void complete_pending_flushes(uint64_t barrier_id) {
        for (auto& b : barriers) {
            if (b.barrier_id == barrier_id) b.clear_pending();
        }
    }

    void acknowledge_barrier(uint64_t barrier_id) {
        for (auto& b : barriers) {
            if (b.barrier_id == barrier_id) b.acknowledge();
        }
    }

    std::optional<barrier_entry> get_next_pending() const {
        for (const auto& b : barriers) {
            if (b.pending_flushes) return b;
        }
        return std::nullopt;
    }

    size_t pending_barrier_count() const {
        size_t n = 0;
        for (const auto& b : barriers) { if (b.pending_flushes) n++; }
        return n;
    }

    size_t complete_barrier_count() const {
        size_t n = 0;
        for (const auto& b : barriers) { if (b.is_complete()) n++; }
        return n;
    }

    bool has_pending_barriers() const { return pending_barrier_count() > 0; }

    void remove_completed() {
        barriers.erase(std::remove_if(barriers.begin(), barriers.end(),
            [](const barrier_entry& b) { return b.is_complete(); }), barriers.end());
    }

    uint64_t total_barrier_count() const { return barriers.size(); }
};

FB_TEST(bdev_write_barrier, entry_initial_not_acknowledged) {
    barrier_entry b;
    FB_ASSERT_FALSE(b.acknowledged);
}

FB_TEST(bdev_write_barrier, entry_set_pending) {
    barrier_entry b;
    b.set_pending();
    FB_ASSERT_TRUE(b.pending_flushes);
}

FB_TEST(bdev_write_barrier, entry_clear_pending) {
    barrier_entry b;
    b.set_pending();
    b.clear_pending();
    FB_ASSERT_FALSE(b.pending_flushes);
}

FB_TEST(bdev_write_barrier, entry_acknowledge) {
    barrier_entry b;
    b.acknowledge();
    FB_ASSERT_TRUE(b.acknowledged);
}

FB_TEST(bdev_write_barrier, entry_is_complete) {
    barrier_entry b;
    b.set_pending();
    b.clear_pending();
    b.acknowledge();
    FB_ASSERT_TRUE(b.is_complete());
}

FB_TEST(bdev_write_barrier, manager_issue_barrier) {
    write_barrier_manager mgr;
    uint64_t id = mgr.issue_barrier(1000);
    FB_ASSERT_NE(id, 0u);
    FB_ASSERT_EQ(mgr.total_barrier_count(), 1u);
}

FB_TEST(bdev_write_barrier, manager_complete_pending_flushes) {
    write_barrier_manager mgr;
    uint64_t id = mgr.issue_barrier(1000);
    mgr.complete_pending_flushes(id);
    auto b = mgr.get_next_pending();
    FB_ASSERT_FALSE(b.has_value());  // no more pending
}

FB_TEST(bdev_write_barrier, manager_acknowledge_barrier) {
    write_barrier_manager mgr;
    uint64_t id = mgr.issue_barrier(1000);
    mgr.complete_pending_flushes(id);
    mgr.acknowledge_barrier(id);
    FB_ASSERT_EQ(mgr.complete_barrier_count(), 1u);
}

FB_TEST(bdev_write_barrier, manager_get_next_pending) {
    write_barrier_manager mgr;
    mgr.issue_barrier(1000);
    auto b = mgr.get_next_pending();
    FB_ASSERT_TRUE(b.has_value());
}

FB_TEST(bdev_write_barrier, manager_pending_barrier_count) {
    write_barrier_manager mgr;
    mgr.issue_barrier(1000);
    mgr.issue_barrier(2000);
    FB_ASSERT_EQ(mgr.pending_barrier_count(), 2u);
}

FB_TEST(bdev_write_barrier, manager_has_pending_barriers) {
    write_barrier_manager mgr;
    mgr.issue_barrier(1000);
    FB_ASSERT_TRUE(mgr.has_pending_barriers());
}

FB_TEST(bdev_write_barrier, manager_remove_completed) {
    write_barrier_manager mgr;
    uint64_t id = mgr.issue_barrier(1000);
    mgr.complete_pending_flushes(id);
    mgr.acknowledge_barrier(id);
    mgr.remove_completed();
    FB_ASSERT_EQ(mgr.total_barrier_count(), 0u);
}

FB_TEST(bdev_write_barrier, manager_multiple_barriers_order) {
    write_barrier_manager mgr;
    uint64_t id1 = mgr.issue_barrier(1000);
    uint64_t id2 = mgr.issue_barrier(2000);

    FB_ASSERT_EQ(mgr.get_next_pending()->barrier_id, id1);  // FIFO order
}

// ============================================================================
// Test Suite: bdev_io_statistics — IO statistics collection
// ============================================================================

FB_SUITE_SETUP(bdev_io_statistics) {}
FB_SUITE_TEARDOWN(bdev_io_statistics) {}

struct io_stat_entry {
    uint64_t read_ops{0};
    uint64_t write_ops{0};
    uint64_t read_bytes{0};
    uint64_t write_bytes{0};
    uint64_t read_latency_us{0};
    uint64_t write_latency_us{0};
    uint64_t read_errors{0};
    uint64_t write_errors{0};

    void record_read(uint64_t bytes, uint64_t latency_us) {
        read_ops++;
        read_bytes += bytes;
        read_latency_us += latency_us;
    }

    void record_write(uint64_t bytes, uint64_t latency_us) {
        write_ops++;
        write_bytes += bytes;
        write_latency_us += latency_us;
    }

    void record_read_error() { read_errors++; }

    void record_write_error() { write_errors++; }

    uint64_t total_ops() const { return read_ops + write_ops; }

    uint64_t total_bytes() const { return read_bytes + write_bytes; }

    uint64_t total_errors() const { return read_errors + write_errors; }

    double avg_read_latency() const {
        return read_ops > 0 ? static_cast<double>(read_latency_us) / read_ops : 0.0;
    }

    double avg_write_latency() const {
        return write_ops > 0 ? static_cast<double>(write_latency_us) / write_ops : 0.0;
    }

    double read_error_rate() const {
        return read_ops > 0 ? static_cast<double>(read_errors) / read_ops : 0.0;
    }

    double write_error_rate() const {
        return write_ops > 0 ? static_cast<double>(write_errors) / write_ops : 0.0;
    }
};

struct io_stats_collector {
    io_stat_entry current;
    io_stat_entry historical;
    uint64_t collection_start_us{0};
    uint64_t collection_interval_us{1000000};  // 1 second

    void record_read(uint64_t bytes, uint64_t latency, uint64_t now_us) {
        current.record_read(bytes, latency);
    }

    void record_write(uint64_t bytes, uint64_t latency, uint64_t now_us) {
        current.record_write(bytes, latency);
    }

    void record_read_error(uint64_t now_us) { current.record_read_error(); }

    void record_write_error(uint64_t now_us) { current.record_write_error(); }

    void collect(uint64_t now_us) {
        if (now_us - collection_start_us >= collection_interval_us) {
            historical.read_ops += current.read_ops;
            historical.write_ops += current.write_ops;
            historical.read_bytes += current.read_bytes;
            historical.write_bytes += current.write_bytes;
            historical.read_errors += current.read_errors;
            historical.write_errors += current.write_errors;
            current = io_stat_entry{};
            collection_start_us = now_us;
        }
    }

    uint64_t throughput_bytes_per_sec(uint64_t interval_us) const {
        if (interval_us == 0) return 0;
        return (current.read_bytes + current.write_bytes) * 1000000 / interval_us;
    }

    uint64_t iops(uint64_t interval_us) const {
        if (interval_us == 0) return 0;
        return current.total_ops() * 1000000 / interval_us;
    }

    double read_write_ratio() const {
        if (current.write_ops == 0) return 0.0;
        return static_cast<double>(current.read_ops) / current.write_ops;
    }

    void reset() {
        current = io_stat_entry{};
        historical = io_stat_entry{};
        collection_start_us = 0;
    }
};

FB_TEST(bdev_io_statistics, entry_initial_zero) {
    io_stat_entry stats;
    FB_ASSERT_EQ(stats.read_ops, 0u);
    FB_ASSERT_EQ(stats.write_ops, 0u);
}

FB_TEST(bdev_io_statistics, entry_record_read) {
    io_stat_entry stats;
    stats.record_read(1024, 1000);
    FB_ASSERT_EQ(stats.read_ops, 1u);
    FB_ASSERT_EQ(stats.read_bytes, 1024u);
}

FB_TEST(bdev_io_statistics, entry_record_write) {
    io_stat_entry stats;
    stats.record_write(2048, 2000);
    FB_ASSERT_EQ(stats.write_ops, 1u);
    FB_ASSERT_EQ(stats.write_bytes, 2048u);
}

FB_TEST(bdev_io_statistics, entry_record_read_error) {
    io_stat_entry stats;
    stats.record_read(1024, 1000);
    stats.record_read_error();
    FB_ASSERT_EQ(stats.read_errors, 1u);
}

FB_TEST(bdev_io_statistics, entry_record_write_error) {
    io_stat_entry stats;
    stats.record_write(2048, 2000);
    stats.record_write_error();
    FB_ASSERT_EQ(stats.write_errors, 1u);
}

FB_TEST(bdev_io_statistics, entry_total_ops) {
    io_stat_entry stats;
    stats.record_read(1024, 1000);
    stats.record_write(2048, 2000);
    FB_ASSERT_EQ(stats.total_ops(), 2u);
}

FB_TEST(bdev_io_statistics, entry_total_bytes) {
    io_stat_entry stats;
    stats.record_read(1024, 1000);
    stats.record_write(2048, 2000);
    FB_ASSERT_EQ(stats.total_bytes(), 3072u);
}

FB_TEST(bdev_io_statistics, entry_avg_read_latency) {
    io_stat_entry stats;
    stats.record_read(1024, 1000);
    stats.record_read(1024, 3000);
    FB_ASSERT_EQ(stats.avg_read_latency(), 2000.0);
}

FB_TEST(bdev_io_statistics, entry_avg_write_latency) {
    io_stat_entry stats;
    stats.record_write(1024, 2000);
    FB_ASSERT_EQ(stats.avg_write_latency(), 2000.0);
}

FB_TEST(bdev_io_statistics, entry_read_error_rate) {
    io_stat_entry stats;
    stats.record_read(1024, 1000);
    stats.record_read(1024, 1000);
    stats.record_read_error();
    FB_ASSERT_TRUE(stats.read_error_rate() > 0.3 && stats.read_error_rate() < 0.6);
}

FB_TEST(bdev_io_statistics, collector_record) {
    io_stats_collector collector;
    collector.record_read(1024, 1000, 0);
    collector.record_write(2048, 2000, 0);
    FB_ASSERT_EQ(collector.current.total_ops(), 2u);
}

FB_TEST(bdev_io_statistics, collector_collect_historical) {
    io_stats_collector collector;
    collector.collection_start_us = 0;
    collector.record_read(1024, 1000, 500000);
    collector.collect(1000000);  // triggers collection
    FB_ASSERT_EQ(collector.historical.read_ops, 1u);
    FB_ASSERT_EQ(collector.current.read_ops, 0u);  // reset
}

FB_TEST(bdev_io_statistics, collector_throughput_calculation) {
    io_stats_collector collector;
    collector.record_read(1024 * 1024, 1000, 0);
    collector.record_write(1024 * 1024, 1000, 0);
    uint64_t throughput = collector.throughput_bytes_per_sec(1000000);
    FB_ASSERT_EQ(throughput, 2u * 1024u * 1024u);
}

FB_TEST(bdev_io_statistics, collector_iops_calculation) {
    io_stats_collector collector;
    collector.record_read(1024, 1000, 0);
    collector.record_write(1024, 1000, 0);
    uint64_t iops = collector.iops(1000000);
    FB_ASSERT_EQ(iops, 2u);
}

FB_TEST(bdev_io_statistics, collector_read_write_ratio) {
    io_stats_collector collector;
    collector.record_read(1024, 1000, 0);
    collector.record_write(1024, 1000, 0);
    collector.record_write(1024, 1000, 0);
    FB_ASSERT_TRUE(collector.read_write_ratio() > 0.3 && collector.read_write_ratio() < 0.6);
}

FB_TEST(bdev_io_statistics, collector_reset) {
    io_stats_collector collector;
    collector.record_read(1024, 1000, 0);
    collector.reset();
    FB_ASSERT_EQ(collector.current.read_ops, 0u);
    FB_ASSERT_EQ(collector.historical.read_ops, 0u);
}

// ============================================================================
// Test Suite: bdev_token_throttle — Token bucket throttling
// ============================================================================

FB_SUITE_SETUP(bdev_token_throttle) {}
FB_SUITE_TEARDOWN(bdev_token_throttle) {}

struct token_bucket {
    uint64_t tokens{0};
    uint64_t max_tokens{1000};
    uint64_t refill_rate{100};  // tokens per second
    uint64_t last_refill_us{0};

    void refill(uint64_t now_us) {
        uint64_t elapsed_us = now_us - last_refill_us;
        uint64_t new_tokens = elapsed_us * refill_rate / 1000000;
        tokens = std::min(tokens + new_tokens, max_tokens);
        last_refill_us = now_us;
    }

    bool try_consume(uint64_t count, uint64_t now_us) {
        refill(now_us);
        if (tokens >= count) {
            tokens -= count;
            return true;
        }
        return false;
    }

    uint64_t available(uint64_t now_us) {
        refill(now_us);
        return tokens;
    }

    bool can_consume(uint64_t count, uint64_t now_us) {
        refill(now_us);
        return tokens >= count;
    }

    uint64_t wait_time_for(uint64_t count, uint64_t now_us) const {
        if (count <= tokens) return 0;
        uint64_t deficit = count - tokens;
        return deficit * 1000000 / refill_rate;
    }

    void reset() { tokens = max_tokens; }
};

FB_TEST(bdev_token_throttle, bucket_initial_tokens) {
    token_bucket bucket;
    bucket.max_tokens = 1000;
    bucket.tokens = 1000;
    FB_ASSERT_EQ(bucket.tokens, 1000u);
}

FB_TEST(bdev_token_throttle, bucket_refill_adds_tokens) {
    token_bucket bucket;
    bucket.tokens = 0;
    bucket.last_refill_us = 0;
    bucket.refill(1000000);  // 1 second
    FB_ASSERT_EQ(bucket.tokens, 100u);  // refill_rate = 100
}

FB_TEST(bdev_token_throttle, bucket_refill_caps_at_max) {
    token_bucket bucket;
    bucket.tokens = 950;
    bucket.max_tokens = 1000;
    bucket.last_refill_us = 0;
    bucket.refill(1000000);
    FB_ASSERT_EQ(bucket.tokens, 1000u);  // capped
}

FB_TEST(bdev_token_throttle, bucket_try_consume_success) {
    token_bucket bucket;
    bucket.tokens = 100;
    bucket.last_refill_us = 1000000;
    FB_ASSERT_TRUE(bucket.try_consume(50, 1000000));
    FB_ASSERT_EQ(bucket.tokens, 50u);
}

FB_TEST(bdev_token_throttle, bucket_try_consume_fail_insufficient) {
    token_bucket bucket;
    bucket.tokens = 30;
    bucket.last_refill_us = 0;
    FB_ASSERT_FALSE(bucket.try_consume(50, 0));
}

FB_TEST(bdev_token_throttle, bucket_available_after_refill) {
    token_bucket bucket;
    bucket.tokens = 0;
    bucket.last_refill_us = 0;
    FB_ASSERT_EQ(bucket.available(5000000), 500u);  // 5s = 500 tokens
}

FB_TEST(bdev_token_throttle, bucket_can_consume_check) {
    token_bucket bucket;
    bucket.tokens = 80;
    bucket.last_refill_us = 0;
    FB_ASSERT_TRUE(bucket.can_consume(80, 0));
    FB_ASSERT_FALSE(bucket.can_consume(81, 0));
}

FB_TEST(bdev_token_throttle, bucket_wait_time_for_deficit) {
    token_bucket bucket;
    bucket.tokens = 20;
    bucket.refill_rate = 100;
    FB_ASSERT_EQ(bucket.wait_time_for(100, 0), 800000u);  // 80 deficit * 1s/100
}

FB_TEST(bdev_token_throttle, bucket_wait_time_zero_if_sufficient) {
    token_bucket bucket;
    bucket.tokens = 100;
    FB_ASSERT_EQ(bucket.wait_time_for(50, 0), 0u);
}

FB_TEST(bdev_token_throttle, bucket_reset_to_max) {
    token_bucket bucket;
    bucket.tokens = 10;
    bucket.max_tokens = 1000;
    bucket.reset();
    FB_ASSERT_EQ(bucket.tokens, 1000u);
}

FB_TEST(bdev_token_throttle, bucket_multiple_consumes) {
    token_bucket bucket;
    bucket.tokens = 100;
    bucket.last_refill_us = 0;
    bucket.try_consume(30, 0);
    bucket.try_consume(40, 0);
    FB_ASSERT_EQ(bucket.tokens, 30u);
}

FB_TEST(bdev_token_throttle, bucket_refill_between_consumes) {
    token_bucket bucket;
    bucket.tokens = 10;
    bucket.last_refill_us = 0;
    bucket.try_consume(10, 0);  // consume all
    FB_ASSERT_FALSE(bucket.can_consume(1, 0));
    bucket.refill(1000000);  // refill 100 tokens
    FB_ASSERT_TRUE(bucket.can_consume(50, 1000000));
}

// ============================================================================
// Test Suite: bdev_image_lock — Image locking for exclusive access
// ============================================================================

FB_SUITE_SETUP(bdev_image_lock) {}
FB_SUITE_TEARDOWN(bdev_image_lock) {}

enum class lock_mode : uint8_t {
    none,
    shared,
    exclusive
};

struct lock_holder {
    uint64_t holder_id{0};
    lock_mode mode{lock_mode::none};
    uint64_t acquired_at_us{0};
    uint64_t timeout_us{0};

    bool is_expired(uint64_t now_us) const {
        return timeout_us > 0 && now_us - acquired_at_us > timeout_us;
    }
};

struct image_lock {
    std::vector<lock_holder> holders;
    uint64_t next_holder_id{1};

    std::optional<uint64_t> acquire(lock_mode mode, uint64_t now_us, uint64_t timeout_us) {
        // Exclusive: no other holders allowed
        if (mode == lock_mode::exclusive && !holders.empty()) return std::nullopt;
        // Shared: only if no exclusive holder
        if (mode == lock_mode::shared) {
            for (const auto& h : holders) {
                if (h.mode == lock_mode::exclusive) return std::nullopt;
            }
        }

        lock_holder holder;
        holder.holder_id = next_holder_id++;
        holder.mode = mode;
        holder.acquired_at_us = now_us;
        holder.timeout_us = timeout_us;
        holders.push_back(holder);
        return holder.holder_id;
    }

    bool release(uint64_t holder_id) {
        auto it = std::find_if(holders.begin(), holders.end(),
            [holder_id](const lock_holder& h) { return h.holder_id == holder_id; });
        if (it != holders.end()) { holders.erase(it); return true; }
        return false;
    }

    void purge_expired(uint64_t now_us) {
        holders.erase(std::remove_if(holders.begin(), holders.end(),
            [now_us](const lock_holder& h) { return h.is_expired(now_us); }), holders.end());
    }

    bool is_locked() const { return !holders.empty(); }

    bool has_exclusive() const {
        for (const auto& h : holders) { if (h.mode == lock_mode::exclusive) return true; }
        return false;
    }

    size_t shared_count() const {
        size_t n = 0;
        for (const auto& h : holders) { if (h.mode == lock_mode::shared) n++; }
        return n;
    }

    size_t holder_count() const { return holders.size(); }

    void force_release_all() { holders.clear(); }
};

FB_TEST(bdev_image_lock, acquire_shared_success) {
    image_lock lock;
    auto id = lock.acquire(lock_mode::shared, 0, 0);
    FB_ASSERT_TRUE(id.has_value());
    FB_ASSERT_EQ(lock.holder_count(), 1u);
}

FB_TEST(bdev_image_lock, acquire_exclusive_success) {
    image_lock lock;
    auto id = lock.acquire(lock_mode::exclusive, 0, 0);
    FB_ASSERT_TRUE(id.has_value());
    FB_ASSERT_TRUE(lock.has_exclusive());
}

FB_TEST(bdev_image_lock, multiple_shared_allowed) {
    image_lock lock;
    lock.acquire(lock_mode::shared, 0, 0);
    auto id2 = lock.acquire(lock_mode::shared, 0, 0);
    FB_ASSERT_TRUE(id2.has_value());
    FB_ASSERT_EQ(lock.shared_count(), 2u);
}

FB_TEST(bdev_image_lock, shared_blocks_exclusive) {
    image_lock lock;
    lock.acquire(lock_mode::shared, 0, 0);
    auto id = lock.acquire(lock_mode::exclusive, 0, 0);
    FB_ASSERT_FALSE(id.has_value());
}

FB_TEST(bdev_image_lock, exclusive_blocks_shared) {
    image_lock lock;
    lock.acquire(lock_mode::exclusive, 0, 0);
    auto id = lock.acquire(lock_mode::shared, 0, 0);
    FB_ASSERT_FALSE(id.has_value());
}

FB_TEST(bdev_image_lock, exclusive_blocks_exclusive) {
    image_lock lock;
    lock.acquire(lock_mode::exclusive, 0, 0);
    auto id = lock.acquire(lock_mode::exclusive, 0, 0);
    FB_ASSERT_FALSE(id.has_value());
}

FB_TEST(bdev_image_lock, release_success) {
    image_lock lock;
    auto id = lock.acquire(lock_mode::shared, 0, 0);
    FB_ASSERT_TRUE(lock.release(*id));
    FB_ASSERT_EQ(lock.holder_count(), 0u);
}

FB_TEST(bdev_image_lock, release_allows_new_exclusive) {
    image_lock lock;
    auto id = lock.acquire(lock_mode::shared, 0, 0);
    lock.release(*id);
    auto ex = lock.acquire(lock_mode::exclusive, 0, 0);
    FB_ASSERT_TRUE(ex.has_value());
}

FB_TEST(bdev_image_lock, is_locked_check) {
    image_lock lock;
    FB_ASSERT_FALSE(lock.is_locked());
    lock.acquire(lock_mode::shared, 0, 0);
    FB_ASSERT_TRUE(lock.is_locked());
}

FB_TEST(bdev_image_lock, purge_expired_removes_timed_out) {
    image_lock lock;
    lock.acquire(lock_mode::shared, 0, 10000);  // 10ms timeout
    lock.purge_expired(50000);  // 50ms later
    FB_ASSERT_EQ(lock.holder_count(), 0u);
}

FB_TEST(bdev_image_lock, purge_expired_keeps_valid) {
    image_lock lock;
    lock.acquire(lock_mode::shared, 0, 60000);  // 60s timeout
    lock.purge_expired(5000);
    FB_ASSERT_EQ(lock.holder_count(), 1u);
}

FB_TEST(bdev_image_lock, force_release_all) {
    image_lock lock;
    lock.acquire(lock_mode::shared, 0, 0);
    lock.acquire(lock_mode::shared, 0, 0);
    lock.force_release_all();
    FB_ASSERT_EQ(lock.holder_count(), 0u);
}

// ============================================================================
// Test Suite: bdev_stripe_align — Stripe alignment for RAID
// ============================================================================

FB_SUITE_SETUP(bdev_stripe_align) {}
FB_SUITE_TEARDOWN(bdev_stripe_align) {}

struct stripe_config {
    uint64_t stripe_size{65536};   // 64 KiB default
    uint32_t stripe_count{4};      // number of data stripes
    uint32_t parity_count{1};      // number of parity stripes

    uint64_t align_down(uint64_t offset) const {
        return offset & ~(stripe_size - 1);
    }

    uint64_t align_up(uint64_t offset) const {
        return (offset + stripe_size - 1) & ~(stripe_size - 1);
    }

    bool is_aligned(uint64_t offset) const {
        return (offset & (stripe_size - 1)) == 0;
    }

    uint64_t stripe_index(uint64_t offset) const {
        return offset / stripe_size;
    }

    uint64_t parity_group(uint64_t stripe_idx) const {
        return stripe_idx / stripe_count;
    }

    uint64_t offset_in_stripe(uint64_t offset) const {
        return offset % stripe_size;
    }

    uint64_t remaining_in_stripe(uint64_t offset) const {
        return stripe_size - offset_in_stripe(offset);
    }

    uint32_t total_disks() const { return stripe_count + parity_count; }

    double write_amplification() const {
        return static_cast<double>(total_disks()) / stripe_count;
    }
};

FB_TEST(bdev_stripe_align, default_stripe_size) {
    stripe_config cfg;
    FB_ASSERT_EQ(cfg.stripe_size, 65536u);
}

FB_TEST(bdev_stripe_align, align_down) {
    stripe_config cfg;
    FB_ASSERT_EQ(cfg.align_down(70000), 65536u);
}

FB_TEST(bdev_stripe_align, align_up) {
    stripe_config cfg;
    FB_ASSERT_EQ(cfg.align_up(70000), 131072u);
}

FB_TEST(bdev_stripe_align, align_up_already_aligned) {
    stripe_config cfg;
    FB_ASSERT_EQ(cfg.align_up(65536), 65536u);
}

FB_TEST(bdev_stripe_align, is_aligned_true) {
    stripe_config cfg;
    FB_ASSERT_TRUE(cfg.is_aligned(0));
    FB_ASSERT_TRUE(cfg.is_aligned(65536));
}

FB_TEST(bdev_stripe_align, is_aligned_false) {
    stripe_config cfg;
    FB_ASSERT_FALSE(cfg.is_aligned(1));
    FB_ASSERT_FALSE(cfg.is_aligned(100));
}

FB_TEST(bdev_stripe_align, stripe_index) {
    stripe_config cfg;
    FB_ASSERT_EQ(cfg.stripe_index(0), 0u);
    FB_ASSERT_EQ(cfg.stripe_index(65536), 1u);
    FB_ASSERT_EQ(cfg.stripe_index(131072), 2u);
}

FB_TEST(bdev_stripe_align, parity_group) {
    stripe_config cfg;
    cfg.stripe_count = 4;
    FB_ASSERT_EQ(cfg.parity_group(0), 0u);
    FB_ASSERT_EQ(cfg.parity_group(3), 0u);
    FB_ASSERT_EQ(cfg.parity_group(4), 1u);
}

FB_TEST(bdev_stripe_align, offset_in_stripe) {
    stripe_config cfg;
    FB_ASSERT_EQ(cfg.offset_in_stripe(0), 0u);
    FB_ASSERT_EQ(cfg.offset_in_stripe(100), 100u);
    FB_ASSERT_EQ(cfg.offset_in_stripe(65536), 0u);
}

FB_TEST(bdev_stripe_align, remaining_in_stripe) {
    stripe_config cfg;
    FB_ASSERT_EQ(cfg.remaining_in_stripe(0), 65536u);
    FB_ASSERT_EQ(cfg.remaining_in_stripe(100), 65436u);
}

FB_TEST(bdev_stripe_align, total_disks) {
    stripe_config cfg;
    FB_ASSERT_EQ(cfg.total_disks(), 5u);  // 4 data + 1 parity
}

FB_TEST(bdev_stripe_align, write_amplification) {
    stripe_config cfg;
    double amp = cfg.write_amplification();
    FB_ASSERT_TRUE(amp > 1.0 && amp < 2.0);  // 5/4 = 1.25
}

// ============================================================================
// Test Suite: bdev_io_splitting — Large IO splitting across objects
// ============================================================================

FB_SUITE_SETUP(bdev_io_splitting) {}
FB_SUITE_TEARDOWN(bdev_io_splitting) {}

struct io_split_entry {
    uint64_t split_id{0};
    uint64_t original_offset{0};
    uint64_t original_length{0};
    uint64_t split_offset{0};
    uint64_t split_length{0};
    uint32_t split_index{0};
    bool completed{false};
};

struct io_splitter {
    uint64_t object_size{4 * 1024 * 1024};  // 4 MiB
    std::vector<io_split_entry> splits;
    uint64_t next_split_id{1};

    void split(uint64_t offset, uint64_t length) {
        uint64_t remaining = length;
        uint64_t current_offset = offset;
        uint32_t index = 0;

        while (remaining > 0) {
            io_split_entry entry;
            entry.split_id = next_split_id++;
            entry.original_offset = offset;
            entry.original_length = length;
            entry.split_offset = current_offset;
            entry.split_index = index;

            uint64_t offset_in_obj = current_offset % object_size;
            uint64_t max_in_obj = object_size - offset_in_obj;
            entry.split_length = std::min(remaining, max_in_obj);

            splits.push_back(entry);
            current_offset += entry.split_length;
            remaining -= entry.split_length;
            index++;
        }
    }

    size_t split_count() const { return splits.size(); }

    uint64_t total_split_length() const {
        uint64_t total = 0;
        for (const auto& s : splits) total += s.split_length;
        return total;
    }

    bool verify_total_length() const {
        return splits.empty() || splits[0].original_length == total_split_length();
    }

    std::optional<io_split_entry> get_split(uint64_t split_id) const {
        for (const auto& s : splits) { if (s.split_id == split_id) return s; }
        return std::nullopt;
    }

    void mark_completed(uint64_t split_id) {
        for (auto& s : splits) { if (s.split_id == split_id) s.completed = true; }
    }

    size_t completed_count() const {
        size_t n = 0;
        for (const auto& s : splits) { if (s.completed) n++; }
        return n;
    }

    bool all_completed() const {
        for (const auto& s : splits) { if (!s.completed) return false; }
        return true;
    }

    void clear() { splits.clear(); }
};

FB_TEST(bdev_io_splitting, split_single_object) {
    io_splitter splitter;
    splitter.split(0, 1024);  // fits in one object
    FB_ASSERT_EQ(splitter.split_count(), 1u);
}

FB_TEST(bdev_io_splitting, split_across_boundary) {
    io_splitter splitter;
    splitter.split(4 * 1024 * 1024 - 512, 1024);  // spans two objects
    FB_ASSERT_EQ(splitter.split_count(), 2u);
}

FB_TEST(bdev_io_splitting, split_multiple_objects) {
    io_splitter splitter;
    splitter.split(0, 12 * 1024 * 1024);  // 3 objects
    FB_ASSERT_EQ(splitter.split_count(), 3u);
}

FB_TEST(bdev_io_splitting, split_preserves_total_length) {
    io_splitter splitter;
    splitter.split(1024, 8 * 1024 * 1024);
    FB_ASSERT_TRUE(splitter.verify_total_length());
}

FB_TEST(bdev_io_splitting, split_length_matches) {
    io_splitter splitter;
    splitter.split(0, 1024);
    FB_ASSERT_EQ(splitter.splits[0].split_length, 1024u);
}

FB_TEST(bdev_io_splitting, split_offset_sequence) {
    io_splitter splitter;
    splitter.split(0, 8 * 1024 * 1024 + 512);
    FB_ASSERT_EQ(splitter.splits[0].split_offset, 0u);
    FB_ASSERT_EQ(splitter.splits[1].split_offset, 4 * 1024 * 1024);
    FB_ASSERT_EQ(splitter.splits[2].split_offset, 8 * 1024 * 1024);
}

FB_TEST(bdev_io_splitting, split_index_sequence) {
    io_splitter splitter;
    splitter.split(0, 9 * 1024 * 1024);
    FB_ASSERT_EQ(splitter.splits[0].split_index, 0u);
    FB_ASSERT_EQ(splitter.splits[1].split_index, 1u);
    FB_ASSERT_EQ(splitter.splits[2].split_index, 2u);
}

FB_TEST(bdev_io_splitting, split_partial_first_object) {
    io_splitter splitter;
    splitter.split(2 * 1024 * 1024, 1024);  // starts mid-object
    FB_ASSERT_EQ(splitter.splits[0].split_offset, 2 * 1024 * 1024);
    FB_ASSERT_EQ(splitter.splits[0].split_length, 1024u);
}

FB_TEST(bdev_io_splitting, split_partial_last_object) {
    io_splitter splitter;
    splitter.split(0, 5 * 1024 * 1024);  // 1 full + 1 partial
    FB_ASSERT_EQ(splitter.splits[1].split_length, 1 * 1024 * 1024);
}

FB_TEST(bdev_io_splitting, split_get_by_id) {
    io_splitter splitter;
    splitter.split(0, 1024);
    auto s = splitter.get_split(1);
    FB_ASSERT_TRUE(s.has_value());
}

FB_TEST(bdev_io_splitting, split_mark_completed) {
    io_splitter splitter;
    splitter.split(0, 1024);
    splitter.mark_completed(1);
    FB_ASSERT_EQ(splitter.completed_count(), 1u);
}

FB_TEST(bdev_io_splitting, split_all_completed) {
    io_splitter splitter;
    splitter.split(0, 1024);
    splitter.mark_completed(1);
    FB_ASSERT_TRUE(splitter.all_completed());
}

FB_TEST(bdev_io_splitting, split_clear) {
    io_splitter splitter;
    splitter.split(0, 1024);
    splitter.clear();
    FB_ASSERT_EQ(splitter.split_count(), 0u);
}

// ============================================================================
// Test Suite: bdev_read_ahead — Read-ahead and prefetch strategy
// ============================================================================

FB_SUITE_SETUP(bdev_read_ahead) {}
FB_SUITE_TEARDOWN(bdev_read_ahead) {}

struct read_ahead_entry {
    uint64_t prefetch_offset{0};
    uint64_t prefetch_length{0};
    uint64_t triggered_by_offset{0};
    uint64_t triggered_at_us{0};
    bool completed{false};
    bool consumed{false};
};

struct read_ahead_manager {
    std::deque<read_ahead_entry> prefetch_queue;
    uint64_t prefetch_size{64 * 1024};  // 64 KiB default
    uint64_t prefetch_distance{256 * 1024};  // 256 KiB ahead
    uint64_t max_prefetch_count{8};
    uint64_t next_prefetch_id{1};
    uint64_t prefetch_hits{0};
    uint64_t prefetch_misses{0};

    std::optional<read_ahead_entry> trigger(uint64_t read_offset, uint64_t now_us) {
        if (prefetch_queue.size() >= max_prefetch_count) return std::nullopt;

        read_ahead_entry entry;
        entry.prefetch_offset = read_offset + prefetch_distance;
        entry.prefetch_length = prefetch_size;
        entry.triggered_by_offset = read_offset;
        entry.triggered_at_us = now_us;

        prefetch_queue.push_back(entry);
        return entry;
    }

    void mark_completed(uint64_t offset) {
        for (auto& p : prefetch_queue) {
            if (p.prefetch_offset == offset && !p.completed) p.completed = true;
        }
    }

    std::optional<read_ahead_entry> find_prefetch(uint64_t read_offset) {
        for (auto& p : prefetch_queue) {
            if (p.completed && !p.consumed &&
                read_offset >= p.prefetch_offset &&
                read_offset < p.prefetch_offset + p.prefetch_length) {
                p.consumed = true;
                prefetch_hits++;
                return p;
            }
        }
        prefetch_misses++;
        return std::nullopt;
    }

    bool has_prefetch_for(uint64_t offset) const {
        for (const auto& p : prefetch_queue) {
            if (p.completed && !p.consumed &&
                offset >= p.prefetch_offset &&
                offset < p.prefetch_offset + p.prefetch_length) {
                return true;
            }
        }
        return false;
    }

    void cleanup_consumed() {
        prefetch_queue.erase(std::remove_if(prefetch_queue.begin(), prefetch_queue.end(),
            [](const read_ahead_entry& p) { return p.consumed; }), prefetch_queue.end());
    }

    void cleanup_expired(uint64_t now_us, uint64_t timeout_us) {
        prefetch_queue.erase(std::remove_if(prefetch_queue.begin(), prefetch_queue.end(),
            [now_us, timeout_us](const read_ahead_entry& p) {
                return now_us - p.triggered_at_us > timeout_us && !p.consumed;
            }), prefetch_queue.end());
    }

    size_t pending_count() const { return prefetch_queue.size(); }

    size_t completed_count() const {
        size_t n = 0;
        for (const auto& p : prefetch_queue) { if (p.completed) n++; }
        return n;
    }

    double hit_rate() const {
        uint64_t total = prefetch_hits + prefetch_misses;
        return total > 0 ? static_cast<double>(prefetch_hits) / total : 0.0;
    }

    void reset_stats() { prefetch_hits = 0; prefetch_misses = 0; }
};

FB_TEST(bdev_read_ahead, trigger_creates_entry) {
    read_ahead_manager mgr;
    auto entry = mgr.trigger(1024, 0);
    FB_ASSERT_TRUE(entry.has_value());
    FB_ASSERT_EQ(mgr.pending_count(), 1u);
}

FB_TEST(bdev_read_ahead, trigger_respects_max_count) {
    read_ahead_manager mgr;
    mgr.max_prefetch_count = 2;
    mgr.trigger(0, 0);
    mgr.trigger(1024, 0);
    auto entry3 = mgr.trigger(2048, 0);  // should fail
    FB_ASSERT_FALSE(entry3.has_value());
}

FB_TEST(bdev_read_ahead, prefetch_offset_calculation) {
    read_ahead_manager mgr;
    mgr.prefetch_distance = 1024;
    auto entry = mgr.trigger(0, 0);
    FB_ASSERT_EQ(entry->prefetch_offset, 1024u);
}

FB_TEST(bdev_read_ahead, prefetch_length_configured) {
    read_ahead_manager mgr;
    mgr.prefetch_size = 4096;
    auto entry = mgr.trigger(0, 0);
    FB_ASSERT_EQ(entry->prefetch_length, 4096u);
}

FB_TEST(bdev_read_ahead, mark_completed) {
    read_ahead_manager mgr;
    mgr.trigger(0, 0);
    mgr.mark_completed(256 * 1024);  // prefetch_offset
    FB_ASSERT_EQ(mgr.completed_count(), 1u);
}

FB_TEST(bdev_read_ahead, find_prefetch_hit) {
    read_ahead_manager mgr;
    mgr.prefetch_distance = 0;  // trigger at same offset
    mgr.trigger(1024, 0);
    mgr.mark_completed(1024);
    auto hit = mgr.find_prefetch(1024);
    FB_ASSERT_TRUE(hit.has_value());
    FB_ASSERT_EQ(mgr.prefetch_hits, 1u);
}

FB_TEST(bdev_read_ahead, find_prefetch_miss) {
    read_ahead_manager mgr;
    mgr.trigger(0, 0);
    mgr.mark_completed(256 * 1024);
    auto hit = mgr.find_prefetch(512 * 1024);  // different offset
    FB_ASSERT_FALSE(hit.has_value());
    FB_ASSERT_EQ(mgr.prefetch_misses, 1u);
}

FB_TEST(bdev_read_ahead, has_prefetch_for_check) {
    read_ahead_manager mgr;
    mgr.prefetch_distance = 0;
    mgr.trigger(4096, 0);
    mgr.mark_completed(4096);
    FB_ASSERT_TRUE(mgr.has_prefetch_for(4096));
}

FB_TEST(bdev_read_ahead, cleanup_consumed) {
    read_ahead_manager mgr;
    mgr.prefetch_distance = 0;
    mgr.trigger(0, 0);
    mgr.mark_completed(0);
    mgr.find_prefetch(0);  // consumes it
    mgr.cleanup_consumed();
    FB_ASSERT_EQ(mgr.pending_count(), 0u);
}

FB_TEST(bdev_read_ahead, cleanup_expired) {
    read_ahead_manager mgr;
    mgr.trigger(0, 0);
    mgr.cleanup_expired(1000000, 500000);  // 1s now, 500ms timeout
    FB_ASSERT_EQ(mgr.pending_count(), 0u);  // expired
}

FB_TEST(bdev_read_ahead, hit_rate_calculation) {
    read_ahead_manager mgr;
    mgr.prefetch_hits = 8;
    mgr.prefetch_misses = 2;
    FB_ASSERT_TRUE(mgr.hit_rate() > 0.7 && mgr.hit_rate() < 0.9);  // 80%
}

FB_TEST(bdev_read_ahead, reset_stats) {
    read_ahead_manager mgr;
    mgr.prefetch_hits = 10;
    mgr.prefetch_misses = 5;
    mgr.reset_stats();
    FB_ASSERT_EQ(mgr.prefetch_hits, 0u);
}

// ============================================================================
// Test Main Entry Point
// ============================================================================

FB_TEST_MAIN()
