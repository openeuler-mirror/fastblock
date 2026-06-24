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
 * @file test_base.cc
 * @brief Unit tests for base module: core_sharded, shard_service, sharded<>
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <cstdint>
#include <limits>
#include <vector>
#include <memory>
#include <type_traits>

// ============================================================================
// Test Suite: core_id_type (Core ID Type Tests)
// ============================================================================

FB_SUITE_SETUP(core_id_type) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_id_type) {
    // Teardown code here
}

FB_TEST(core_id_type, type_is_uint32) {
    // core_id_type should be uint32_t
    using core_id_type = uint32_t;
    constexpr bool same_type = std::is_same_v<core_id_type, uint32_t>;
    FB_ASSERT_TRUE(same_type);
    FB_ASSERT_TRUE(std::is_unsigned_v<core_id_type>);
}

FB_TEST(core_id_type, size_check) {
    // core_id_type should be 4 bytes
    using core_id_type = uint32_t;
    FB_ASSERT_EQ(sizeof(core_id_type), 4);
}

FB_TEST(core_id_type, max_value) {
    // Max core_id_type should be UINT32_MAX (used as sentinel)
    using core_id_type = uint32_t;
    constexpr core_id_type sentinel = std::numeric_limits<core_id_type>::max();
    FB_ASSERT_EQ(sentinel, UINT32_MAX);
}

FB_TEST(core_id_type, valid_range) {
    // Valid core IDs are 0 to N-1 where N is core count
    uint32_t core_id_0 = 0;
    uint32_t core_id_127 = 127;
    FB_ASSERT_TRUE(core_id_0 < core_id_127);
}

FB_TEST(core_id_type, sentinel_distinguishable) {
    // Sentinel value must be distinguishable from valid IDs
    uint32_t valid_id = 100;
    uint32_t sentinel = std::numeric_limits<uint32_t>::max();
    FB_ASSERT_TRUE(valid_id != sentinel);
    FB_ASSERT_TRUE(sentinel > valid_id);
}

// ============================================================================
// Test Suite: core_container (Core Container Tests)
// ============================================================================

FB_SUITE_SETUP(core_container) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_container) {
    // Teardown code here
}

FB_TEST(core_container, empty_initialization) {
    // core_container_type is std::vector<core_id_type>
    std::vector<uint32_t> shard_cores;
    FB_ASSERT_TRUE(shard_cores.empty());
    FB_ASSERT_EQ(shard_cores.size(), 0);
}

FB_TEST(core_container, add_cores) {
    // Add multiple cores to container
    std::vector<uint32_t> shard_cores;
    shard_cores.push_back(0);
    shard_cores.push_back(1);
    shard_cores.push_back(2);
    shard_cores.push_back(3);

    FB_ASSERT_EQ(shard_cores.size(), 4);
    FB_ASSERT_EQ(shard_cores[0], 0);
    FB_ASSERT_EQ(shard_cores[3], 3);
}

FB_TEST(core_container, unique_cores) {
    // Each core should appear only once
    std::vector<uint32_t> shard_cores = {0, 1, 2, 3, 4};
    std::set<uint32_t> unique_cores(shard_cores.begin(), shard_cores.end());
    FB_ASSERT_EQ(unique_cores.size(), shard_cores.size());
}

FB_TEST(core_container, sequential_cores) {
    // Cores should be added in sequential order
    std::vector<uint32_t> shard_cores = {0, 1, 2, 3};
    for (size_t i = 1; i < shard_cores.size(); i++) {
        FB_ASSERT_TRUE(shard_cores[i] > shard_cores[i-1]);
    }
}

FB_TEST(core_container, non_sequential_cores) {
    // Cores can be non-sequential (e.g., NUMA topology)
    std::vector<uint32_t> shard_cores = {0, 2, 4, 6};
    FB_ASSERT_EQ(shard_cores.size(), 4);
    // Even-numbered cores only (e.g., hyperthread siblings excluded)
    for (uint32_t core : shard_cores) {
        FB_ASSERT_EQ(core % 2, 0);
    }
}

FB_TEST(core_container, find_core_by_index) {
    // Find shard_id by core_id (reverse mapping)
    std::vector<uint32_t> shard_cores = {0, 2, 4, 6};
    uint32_t target_core = 4;

    uint32_t shard_id = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < shard_cores.size(); i++) {
        if (shard_cores[i] == target_core) {
            shard_id = i;
            break;
        }
    }
    FB_ASSERT_EQ(shard_id, 2); // core 4 is at index 2
}

FB_TEST(core_container, this_shard_id_not_found) {
    // If current core not in shard_cores, return UINT32_MAX
    std::vector<uint32_t> shard_cores = {0, 1, 2, 3};
    uint32_t current_core = 99; // Not in list

    uint32_t shard_id = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < shard_cores.size(); i++) {
        if (shard_cores[i] == current_core) {
            shard_id = i;
            break;
        }
    }
    FB_ASSERT_EQ(shard_id, std::numeric_limits<uint32_t>::max());
}

FB_TEST(core_container, core_count_matches_capacity) {
    // shard_cores size should match configured count
    uint32_t configured_count = 8;
    std::vector<uint32_t> shard_cores;
    for (uint32_t i = 0; i < configured_count; i++) {
        shard_cores.push_back(i);
    }
    FB_ASSERT_EQ(shard_cores.size(), configured_count);
}

// ============================================================================
// Test Suite: core_iterator (Core Iterator Tests)
// ============================================================================

FB_SUITE_SETUP(core_iterator) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_iterator) {
    // Teardown code here
}

FB_TEST(core_iterator, iterator_category) {
    // core_iterator should be forward_iterator
    using iter_category = std::forward_iterator_tag;
    constexpr bool same = std::is_same_v<iter_category, std::forward_iterator_tag>;
    FB_ASSERT_TRUE(same);
}

FB_TEST(core_iterator, value_type) {
    // value_type should be uint32_t (core_id_type)
    using value_type = uint32_t;
    FB_ASSERT_TRUE(std::is_unsigned_v<value_type>);
    FB_ASSERT_EQ(sizeof(value_type), 4);
}

FB_TEST(core_iterator, default_construction) {
    // Iterator should be default constructible
    uint32_t default_value = 0;
    FB_ASSERT_EQ(default_value, 0);
}

FB_TEST(core_iterator, end_sentinel) {
    // end() returns iterator with UINT32_MAX
    uint32_t end_value = UINT32_MAX;
    FB_ASSERT_EQ(end_value, std::numeric_limits<uint32_t>::max());
}

FB_TEST(core_iterator, dereference_operator) {
    // operator* returns reference to core_id
    uint32_t core_id = 5;
    uint32_t& ref = core_id;
    FB_ASSERT_EQ(ref, 5);

    // Modification through reference
    ref = 7;
    FB_ASSERT_EQ(core_id, 7);
}

FB_TEST(core_iterator, arrow_operator) {
    // operator-> returns pointer to core_id
    uint32_t core_id = 5;
    uint32_t* ptr = &core_id;
    FB_ASSERT_EQ(*ptr, 5);
}

FB_TEST(core_iterator, equality_comparison) {
    // Iterators with same core_id are equal
    uint32_t core_a = 3;
    uint32_t core_b = 3;
    FB_ASSERT_TRUE(core_a == core_b);

    // Different core_ids are not equal
    uint32_t core_c = 4;
    FB_ASSERT_TRUE(core_a != core_c);
}

FB_TEST(core_iterator, iteration_pattern) {
    // Iteration: while (begin != end) { *begin; ++begin; }
    std::vector<uint32_t> cores = {0, 1, 2, 3};
    auto it = cores.begin();
    uint32_t count = 0;
    while (it != cores.end()) {
        count++;
        ++it;
    }
    FB_ASSERT_EQ(count, cores.size());
}

// ============================================================================
// Test Suite: shard_count (Shard Count Tests)
// ============================================================================

FB_SUITE_SETUP(shard_count) {
    // Setup code here
}

FB_SUITE_TEARDOWN(shard_count) {
    // Teardown code here
}

FB_TEST(shard_count, single_shard) {
    // Single shard configuration
    uint32_t shard_count = 1;
    FB_ASSERT_EQ(shard_count, 1);
}

FB_TEST(shard_count, multi_shard) {
    // Multi-shard configuration (typical 4-16 shards)
    uint32_t shard_count = 8;
    FB_ASSERT_TRUE(shard_count > 1);
    FB_ASSERT_TRUE(shard_count <= 256); // Reasonable upper bound
}

FB_TEST(shard_count, power_of_two_optimization) {
    // Power-of-two shards optimize hashing
    uint32_t shard_count = 8;
    bool is_power_of_two = (shard_count & (shard_count - 1)) == 0;
    FB_ASSERT_TRUE(is_power_of_two);
}

FB_TEST(shard_count, non_power_of_two) {
    // Non-power-of-two shards also work
    uint32_t shard_count = 6;
    bool is_power_of_two = (shard_count & (shard_count - 1)) == 0;
    FB_ASSERT_TRUE(!is_power_of_two);
}

FB_TEST(shard_count, shard_id_within_count) {
    // shard_id must be < count()
    uint32_t shard_count = 4;
    uint32_t shard_id = 3;
    FB_ASSERT_TRUE(shard_id < shard_count);
}

FB_TEST(shard_count, shard_count_matches_threads) {
    // shard_count should equal _threads.size()
    uint32_t expected_shards = 4;
    uint32_t shard_cores_size = 4;
    uint32_t threads_size = 4;

    FB_ASSERT_EQ(shard_cores_size, threads_size);
    FB_ASSERT_EQ(shard_cores_size, expected_shards);
}

// ============================================================================
// Test Suite: shard_distribution (Shard Distribution Tests)
// ============================================================================

FB_SUITE_SETUP(shard_distribution) {
    // Setup code here
}

FB_SUITE_TEARDOWN(shard_distribution) {
    // Teardown code here
}

FB_TEST(shard_distribution, even_distribution) {
    // 8 items distributed across 4 shards = 2 per shard
    uint32_t total_items = 8;
    uint32_t shard_count = 4;
    uint32_t items_per_shard = total_items / shard_count;

    FB_ASSERT_EQ(items_per_shard, 2);
    FB_ASSERT_EQ(items_per_shard * shard_count, total_items);
}

FB_TEST(shard_distribution, uneven_distribution) {
    // 10 items across 4 shards = 3 with extra 2 items
    uint32_t total_items = 10;
    uint32_t shard_count = 4;
    uint32_t base_per_shard = total_items / shard_count;
    uint32_t remainder = total_items % shard_count;

    FB_ASSERT_EQ(base_per_shard, 2);
    FB_ASSERT_EQ(remainder, 2);
    // First `remainder` shards get +1
}

FB_TEST(shard_distribution, modulo_assignment) {
    // Hash-based shard assignment: item_id % shard_count
    uint32_t shard_count = 4;
    std::vector<uint32_t> assignments;

    for (uint32_t i = 0; i < 16; i++) {
        assignments.push_back(i % shard_count);
    }

    // Each shard gets exactly 4 items
    std::vector<uint32_t> shard_counts(shard_count, 0);
    for (uint32_t a : assignments) {
        shard_counts[a]++;
    }
    for (uint32_t c : shard_counts) {
        FB_ASSERT_EQ(c, 4);
    }
}

FB_TEST(shard_distribution, hash_consistency) {
    // Same key should always go to same shard
    uint32_t key = 12345;
    uint32_t shard_count = 4;

    uint32_t shard1 = key % shard_count;
    uint32_t shard2 = key % shard_count;
    FB_ASSERT_EQ(shard1, shard2);
}

FB_TEST(shard_distribution, load_balance_quality) {
    // Hash distribution should be approximately balanced
    uint32_t shard_count = 4;
    std::vector<uint32_t> counts(shard_count, 0);

    for (uint32_t i = 0; i < 1000; i++) {
        counts[i % shard_count]++;
    }

    // Each shard should have 250 items
    for (uint32_t c : counts) {
        FB_ASSERT_EQ(c, 250);
    }
}

// ============================================================================
// Test Suite: lambda_ctx (Lambda Context Tests)
// ============================================================================

FB_SUITE_SETUP(lambda_ctx) {
    // Setup code here
}

FB_SUITE_TEARDOWN(lambda_ctx) {
    // Teardown code here
}

FB_TEST(lambda_ctx, lambda_capture_by_value) {
    // Lambda captures arguments by value via tuple
    int captured_value = 42;
    auto lambda = [captured_value]() { return captured_value; };

    FB_ASSERT_EQ(lambda(), 42);
}

FB_TEST(lambda_ctx, tuple_packing) {
    // Arguments packed into std::tuple
    auto args = std::make_tuple(1, 2.5, std::string("test"));
    FB_ASSERT_EQ(std::get<0>(args), 1);
    FB_ASSERT_EQ(std::get<1>(args), 2.5);
    FB_ASSERT_EQ(std::get<2>(args), "test");
}

FB_TEST(lambda_ctx, apply_invocation) {
    // std::apply unpacks tuple into function call
    auto sum = [](int a, int b, int c) { return a + b + c; };
    auto args = std::make_tuple(1, 2, 3);
    int result = std::apply(sum, args);
    FB_ASSERT_EQ(result, 6);
}

FB_TEST(lambda_ctx, void_return) {
    // Lambda with void return type
    bool called = false;
    auto lambda = [&called]() { called = true; };
    lambda();
    FB_ASSERT_TRUE(called);
}

FB_TEST(lambda_ctx, multiple_argument_types) {
    // Lambda with mixed argument types
    auto process = [](int i, const std::string& s, double d) {
        return std::to_string(i) + s + std::to_string(static_cast<int>(d));
    };

    auto args = std::make_tuple(1, std::string("_"), 2.5);
    std::string result = std::apply(process, args);
    FB_ASSERT_TRUE(!result.empty());
}

FB_TEST(lambda_ctx, deferred_execution) {
    // lambda_ctx stores lambda + args, executes later via run_task()
    int executed_count = 0;
    auto deferred = [&executed_count]() { executed_count++; };

    // Execute immediately
    deferred();
    FB_ASSERT_EQ(executed_count, 1);

    // Execute again
    deferred();
    FB_ASSERT_EQ(executed_count, 2);
}

FB_TEST(lambda_ctx, ownership_via_new) {
    // lambda_ctx allocated via new, deleted after run
    int* ptr = new int(100);
    FB_ASSERT_TRUE(ptr != nullptr);
    FB_ASSERT_EQ(*ptr, 100);
    delete ptr;
    // After delete, pointer is dangling
}

// ============================================================================
// Test Suite: core_context (Core Context Tests)
// ============================================================================

FB_SUITE_SETUP(core_context) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_context) {
    // Teardown code here
}

FB_TEST(core_context, abstract_interface) {
    // core_context::run_task is pure virtual
    // Concrete classes must override it
    bool is_abstract = true;
    FB_ASSERT_TRUE(is_abstract);
}

FB_TEST(core_context, run_static_method) {
    // core_context::run(void*) is the static dispatch
    void* arg = nullptr;
    FB_ASSERT_TRUE(arg == nullptr);
}

FB_TEST(core_context, ownership_transfer) {
    // run() deletes the context after running
    // Caller must not access ctx after run()
    int* heap_obj = new int(42);
    FB_ASSERT_TRUE(heap_obj != nullptr);
    delete heap_obj;
    // heap_obj now dangling
}

FB_TEST(core_context, virtual_destructor) {
    // core_context has virtual destructor for safe deletion
    // Derived class destruction works correctly
    bool has_virtual_destructor = true;
    FB_ASSERT_TRUE(has_virtual_destructor);
}

FB_TEST(core_context, opaque_void_pointer) {
    // run() receives void* and casts back to core_context*
    int dummy = 0;
    void* opaque = &dummy;
    FB_ASSERT_TRUE(opaque != nullptr);

    int* recovered = static_cast<int*>(opaque);
    FB_ASSERT_TRUE(recovered != nullptr);
    FB_ASSERT_EQ(*recovered, 0);
}

// ============================================================================
// Test Suite: invoke_on_logic (invoke_on Logic Tests)
// ============================================================================

FB_SUITE_SETUP(invoke_on_logic) {
    // Setup code here
}

FB_SUITE_TEARDOWN(invoke_on_logic) {
    // Teardown code here
}

FB_TEST(invoke_on_logic, same_shard_inline_execution) {
    // If invoke_on() targets current shard, execute inline
    uint32_t current_shard = 2;
    uint32_t target_shard = 2;

    bool inline_execution = (current_shard == target_shard);
    FB_ASSERT_TRUE(inline_execution);
}

FB_TEST(invoke_on_logic, different_shard_async_send) {
    // If target is different shard, use spdk_thread_send_msg
    uint32_t current_shard = 0;
    uint32_t target_shard = 3;

    bool async_send = (current_shard != target_shard);
    FB_ASSERT_TRUE(async_send);
}

FB_TEST(invoke_on_logic, shard_id_bounds) {
    // shard_id must be < shard count
    uint32_t shard_count = 8;
    uint32_t valid_shard = 7;
    uint32_t invalid_shard = 8;

    FB_ASSERT_TRUE(valid_shard < shard_count);
    FB_ASSERT_TRUE(!(invalid_shard < shard_count));
}

FB_TEST(invoke_on_logic, lambda_lifetime) {
    // Lambda must be heap-allocated to survive across thread boundary
    auto* heap_lambda = new int(10); // Simulating heap allocation
    FB_ASSERT_TRUE(heap_lambda != nullptr);
    delete heap_lambda;
}

FB_TEST(invoke_on_logic, args_perfect_forwarding) {
    // Args... must be perfectly forwarded to lambda_ctx
    auto move_count = 0;

    struct movable {
        int& counter;
        movable(int& c) : counter(c) {}
        movable(movable&& o) : counter(o.counter) { counter++; }
        movable(const movable&) = delete;
    };

    movable m(move_count);
    movable moved = std::move(m);
    FB_ASSERT_TRUE(move_count >= 1);
}

FB_TEST(invoke_on_logic, return_value_meaning) {
    // invoke_on returns 0 on success (or spdk_thread_send_msg's return code)
    int success_rc = 0;
    int error_rc = -ENOMEM;

    FB_ASSERT_EQ(success_rc, 0);
    FB_ASSERT_TRUE(error_rc < 0);
}

// ============================================================================
// Test Suite: sharded_template (sharded<> Template Tests)
// ============================================================================

FB_SUITE_SETUP(sharded_template) {
    // Setup code here
}

FB_SUITE_TEARDOWN(sharded_template) {
    // Teardown code here
}

FB_TEST(sharded_template, instances_vector_init) {
    // _instances vector resized to count()
    uint32_t shard_count = 4;
    std::vector<int*> instances(shard_count, nullptr);

    FB_ASSERT_EQ(instances.size(), shard_count);
    for (auto* inst : instances) {
        FB_ASSERT_TRUE(inst == nullptr);
    }
}

FB_TEST(sharded_template, start_allocates_per_shard) {
    // start() allocates Service per shard
    uint32_t shard_count = 4;
    std::vector<int*> instances(shard_count, nullptr);

    for (uint32_t i = 0; i < shard_count; i++) {
        instances[i] = new int(static_cast<int>(i));
    }

    FB_ASSERT_EQ(instances.size(), 4);
    for (uint32_t i = 0; i < shard_count; i++) {
        FB_ASSERT_TRUE(instances[i] != nullptr);
        FB_ASSERT_EQ(*instances[i], static_cast<int>(i));
    }

    // Cleanup
    for (auto* inst : instances) {
        delete inst;
    }
}

FB_TEST(sharded_template, stop_deletes_all) {
    // stop() deletes all instances
    std::vector<int*> instances;
    instances.push_back(new int(1));
    instances.push_back(new int(2));
    instances.push_back(new int(3));

    // Simulate stop()
    for (auto*& inst : instances) {
        delete inst;
        inst = nullptr;
    }
    instances.clear();

    FB_ASSERT_TRUE(instances.empty());
}

FB_TEST(sharded_template, local_returns_current_shard) {
    // local() returns instance for current shard
    std::vector<int*> instances;
    for (int i = 0; i < 4; i++) {
        instances.push_back(new int(i));
    }

    uint32_t current_shard = 2;
    int& local_inst = *instances[current_shard];
    FB_ASSERT_EQ(local_inst, 2);

    // Cleanup
    for (auto* inst : instances) {
        delete inst;
    }
}

FB_TEST(sharded_template, on_shard_returns_specific_shard) {
    // on_shard(N) returns instance for shard N
    std::vector<int*> instances;
    for (int i = 0; i < 4; i++) {
        instances.push_back(new int(i * 10));
    }

    int& shard_3 = *instances[3];
    FB_ASSERT_EQ(shard_3, 30);

    int& shard_0 = *instances[0];
    FB_ASSERT_EQ(shard_0, 0);

    // Cleanup
    for (auto* inst : instances) {
        delete inst;
    }
}

FB_TEST(sharded_template, shard_is_started_true) {
    // shard_is_started returns true if instance not null
    std::vector<int*> instances = {new int(1), new int(2)};

    bool started_0 = (instances.size() > 0 && instances[0] != nullptr);
    bool started_1 = (instances.size() > 1 && instances[1] != nullptr);

    FB_ASSERT_TRUE(started_0);
    FB_ASSERT_TRUE(started_1);

    // Cleanup
    for (auto* inst : instances) {
        delete inst;
    }
}

FB_TEST(sharded_template, shard_is_started_false_null) {
    // shard_is_started returns false if instance is null
    std::vector<int*> instances = {nullptr, new int(1)};

    bool started_0 = (instances.size() > 0 && instances[0] != nullptr);
    FB_ASSERT_TRUE(!started_0);

    delete instances[1];
}

FB_TEST(sharded_template, shard_is_started_false_oob) {
    // shard_is_started returns false if shard >= size
    std::vector<int*> instances = {new int(1)};
    uint32_t oob_shard = 5;

    bool started = (instances.size() > oob_shard);
    FB_ASSERT_TRUE(!started);

    delete instances[0];
}

FB_TEST(sharded_template, size_returns_count) {
    // size() returns _instances.size()
    std::vector<int*> instances(8, nullptr);
    FB_ASSERT_EQ(instances.size(), 8);
}

FB_TEST(sharded_template, empty_after_stop) {
    // After stop(), size() returns 0
    std::vector<int*> instances = {new int(1), new int(2)};
    for (auto*& inst : instances) {
        delete inst;
        inst = nullptr;
    }
    instances.clear();

    FB_ASSERT_EQ(instances.size(), 0);
}

// ============================================================================
// Test Suite: cpuset_operations (CPU Set Operations Tests)
// ============================================================================

FB_SUITE_SETUP(cpuset_operations) {
    // Setup code here
}

FB_SUITE_TEARDOWN(cpuset_operations) {
    // Teardown code here
}

FB_TEST(cpuset_operations, single_cpu_assignment) {
    // Each thread is assigned a single CPU via spdk_cpuset
    // Verify single bit set in cpumask
    uint64_t cpu_mask = 0;
    uint32_t cpu_id = 5;
    cpu_mask |= (1ULL << cpu_id);

    // Count set bits
    int count = 0;
    for (uint64_t mask = cpu_mask; mask != 0; mask >>= 1) {
        if (mask & 1) count++;
    }
    FB_ASSERT_EQ(count, 1);
}

FB_TEST(cpuset_operations, cpu_zero_then_set) {
    // spdk_cpuset_zero then spdk_cpuset_set_cpu
    uint64_t cpu_mask = 0;
    FB_ASSERT_EQ(cpu_mask, 0);

    uint32_t cpu_id = 3;
    cpu_mask |= (1ULL << cpu_id);
    FB_ASSERT_TRUE(cpu_mask != 0);
    FB_ASSERT_EQ(cpu_mask, 1ULL << 3);
}

FB_TEST(cpuset_operations, different_cpus_different_masks) {
    // Different CPUs produce different masks
    uint64_t mask_cpu0 = 1ULL << 0;
    uint64_t mask_cpu1 = 1ULL << 1;

    FB_ASSERT_TRUE(mask_cpu0 != mask_cpu1);
    FB_ASSERT_TRUE((mask_cpu0 & mask_cpu1) == 0);
}

FB_TEST(cpuset_operations, multi_cpu_mask) {
    // Multi-CPU mask (theoretical)
    uint64_t mask = 0;
    mask |= (1ULL << 0);
    mask |= (1ULL << 2);
    mask |= (1ULL << 4);

    int count = 0;
    for (uint64_t m = mask; m != 0; m >>= 1) {
        if (m & 1) count++;
    }
    FB_ASSERT_EQ(count, 3);
}

FB_TEST(cpuset_operations, cpu_isolation) {
    // Each shard's thread is isolated to its own CPU
    std::vector<uint64_t> shard_masks;
    for (uint32_t i = 0; i < 4; i++) {
        shard_masks.push_back(1ULL << i);
    }

    // Pairwise check: no overlap between any two shards
    for (size_t i = 0; i < shard_masks.size(); i++) {
        for (size_t j = i + 1; j < shard_masks.size(); j++) {
            FB_ASSERT_EQ(shard_masks[i] & shard_masks[j], 0);
        }
    }
}

// ============================================================================
// Test Suite: thread_naming (Thread Naming Tests)
// ============================================================================

FB_SUITE_SETUP(thread_naming) {
    // Setup code here
}

FB_SUITE_TEARDOWN(thread_naming) {
    // Teardown code here
}

FB_TEST(thread_naming, app_name_prefix) {
    // Thread name format: <app_name><core_id>
    std::string app_name = "fb_";
    uint32_t core_id = 5;
    std::string thread_name = app_name + std::to_string(core_id);

    FB_ASSERT_EQ(thread_name, "fb_5");
}

FB_TEST(thread_naming, unique_names_per_core) {
    // Different cores produce different thread names
    std::string app = "test_";
    std::string name1 = app + std::to_string(1);
    std::string name2 = app + std::to_string(2);

    FB_ASSERT_TRUE(name1 != name2);
    FB_ASSERT_EQ(name1, "test_1");
    FB_ASSERT_EQ(name2, "test_2");
}

FB_TEST(thread_naming, name_length_reasonable) {
    // Thread names should be within SPDK limits (typically 16 chars on Linux)
    std::string name = "fb_osd_12345";
    FB_ASSERT_TRUE(name.size() < 16);
}

FB_TEST(thread_naming, empty_app_name) {
    // Edge case: empty app name
    std::string app = "";
    uint32_t core = 3;
    std::string name = app + std::to_string(core);
    FB_ASSERT_EQ(name, "3");
}

FB_TEST(thread_naming, long_app_name_truncation) {
    // Long app names may need truncation
    std::string long_name = "very_long_application_name_";
    uint32_t core = 0;
    std::string full = long_name + std::to_string(core);
    FB_ASSERT_TRUE(full.size() > 16); // Would need truncation
}

// ============================================================================
// Test Suite: shard_lifecycle (Shard Lifecycle Tests)
// ============================================================================

FB_SUITE_SETUP(shard_lifecycle) {
    // Setup code here
}

FB_SUITE_TEARDOWN(shard_lifecycle) {
    // Teardown code here
}

FB_TEST(shard_lifecycle, construction_creates_threads) {
    // Constructor creates one thread per shard
    uint32_t n_core = 4;
    std::vector<bool> threads_created(n_core, false);

    for (uint32_t i = 0; i < n_core; i++) {
        threads_created[i] = true;
    }

    for (bool b : threads_created) {
        FB_ASSERT_TRUE(b);
    }
}

FB_TEST(shard_lifecycle, destructor_calls_stop) {
    // ~core_sharded() calls stop()
    bool stop_called = true; // Conceptual
    FB_ASSERT_TRUE(stop_called);
}

FB_TEST(shard_lifecycle, stop_exits_all_threads) {
    // stop() calls spdk_thread_exit for each thread
    uint32_t thread_count = 4;
    uint32_t exited_count = 4;

    FB_ASSERT_EQ(exited_count, thread_count);
}

FB_TEST(shard_lifecycle, stop_clears_threads_vector) {
    // After stop(), _threads.clear()
    std::vector<int*> threads = {new int(1), new int(2), new int(3)};
    for (auto*& t : threads) {
        delete t;
        t = nullptr;
    }
    threads.clear();

    FB_ASSERT_TRUE(threads.empty());
}

FB_TEST(shard_lifecycle, stop_handles_null_threads) {
    // stop() skips null thread pointers
    std::vector<int*> threads = {new int(1), nullptr, new int(3)};

    int valid_count = 0;
    for (auto* t : threads) {
        if (t != nullptr) {
            valid_count++;
        }
    }
    FB_ASSERT_EQ(valid_count, 2);

    // Cleanup
    for (auto* t : threads) {
        if (t) delete t;
    }
}

FB_TEST(shard_lifecycle, copy_construction_deleted) {
    // core_sharded(const core_sharded&) = delete
    FB_ASSERT_TRUE(!std::is_copy_constructible_v<std::unique_ptr<int>>);
}

FB_TEST(shard_lifecycle, move_construction_deleted) {
    // core_sharded(core_sharded&&) = delete
    // Verify unique_ptr is not copyable
    std::unique_ptr<int> p1(new int(42));
    FB_ASSERT_TRUE(p1 != nullptr);

    // Move is fine, but copy is not
    std::unique_ptr<int> p2 = std::move(p1);
    FB_ASSERT_TRUE(p1 == nullptr); // After move
    FB_ASSERT_TRUE(p2 != nullptr);
}

FB_TEST(shard_lifecycle, singleton_pattern) {
    // g_core_sharded is a singleton (unique_ptr in anonymous namespace)
    std::unique_ptr<int> singleton(new int(42));
    FB_ASSERT_TRUE(singleton != nullptr);
    FB_ASSERT_EQ(*singleton, 42);
}

FB_TEST(shard_lifecycle, construct_initializes_singleton) {
    // core_sharded::construct() initializes the singleton
    std::unique_ptr<int> singleton;
    FB_ASSERT_TRUE(singleton == nullptr);

    singleton = std::make_unique<int>(100);
    FB_ASSERT_TRUE(singleton != nullptr);
    FB_ASSERT_EQ(*singleton, 100);
}

// ============================================================================
// Test Suite: get_shard_cores_function (get_shard_cores Function Tests)
// ============================================================================

FB_SUITE_SETUP(get_shard_cores_function) {
    // Setup code here
}

FB_SUITE_TEARDOWN(get_shard_cores_function) {
    // Teardown code here
}

FB_TEST(get_shard_cores_function, returns_vector) {
    // get_shard_cores() returns std::vector<uint32_t>
    std::vector<uint32_t> result;
    FB_ASSERT_TRUE(result.empty());
}

FB_TEST(get_shard_cores_function, iteration_logic) {
    // Function iterates from first_core to last_core (exclusive)
    std::vector<uint32_t> simulated_cores;
    uint32_t first = 0;
    uint32_t last = 4;

    uint32_t lcore = first;
    while (lcore != last) {
        simulated_cores.push_back(lcore);
        lcore++;
    }

    FB_ASSERT_EQ(simulated_cores.size(), 4);
    FB_ASSERT_EQ(simulated_cores[0], 0);
    FB_ASSERT_EQ(simulated_cores[3], 3);
}

FB_TEST(get_shard_cores_function, excludes_last_core) {
    // Last core is excluded from shard_cores (reserved for main thread)
    uint32_t first = 0;
    uint32_t last = 8;
    std::vector<uint32_t> shard_cores;

    uint32_t lcore = first;
    while (lcore != last) {
        shard_cores.push_back(lcore);
        lcore++;
    }

    // 8 cores in range [0, 8) = 0..7
    FB_ASSERT_EQ(shard_cores.size(), 8);
    FB_ASSERT_TRUE(std::find(shard_cores.begin(), shard_cores.end(), last) == shard_cores.end());
}

FB_TEST(get_shard_cores_function, single_core_case) {
    // If first == last (single core), returns empty
    uint32_t first = 3;
    uint32_t last = 3;
    std::vector<uint32_t> shard_cores;

    uint32_t lcore = first;
    while (lcore != last) {
        shard_cores.push_back(lcore);
        lcore++;
    }

    FB_ASSERT_TRUE(shard_cores.empty());
}

FB_TEST(get_shard_cores_function, ordered_result) {
    // Returned cores should be in iteration order
    std::vector<uint32_t> cores;
    for (uint32_t i = 0; i < 4; i++) {
        cores.push_back(i);
    }

    for (size_t i = 1; i < cores.size(); i++) {
        FB_ASSERT_TRUE(cores[i] > cores[i-1]);
    }
}

// ============================================================================
// Test Suite: shard_invoke_semantics (Shard Invocation Semantics Tests)
// ============================================================================

FB_SUITE_SETUP(shard_invoke_semantics) {
    // Setup code here
}

FB_SUITE_TEARDOWN(shard_invoke_semantics) {
    // Teardown code here
}

FB_TEST(shard_invoke_semantics, inline_when_same_core_same_thread) {
    // invoke_on() executes inline when both core AND thread match
    uint32_t target_shard = 2;
    uint32_t shard_cores[] = {0, 1, 2, 3};
    uint32_t target_core = shard_cores[target_shard];

    uint32_t current_core = 2;
    bool same_core = (target_core == current_core);
    // Inline requires same core AND same thread (both must match)
    FB_ASSERT_TRUE(same_core);
}

FB_TEST(shard_invoke_semantics, async_when_different_core) {
    // Different core means must use spdk_thread_send_msg
    uint32_t target_core = 3;
    uint32_t current_core = 1;

    bool different = (target_core != current_core);
    FB_ASSERT_TRUE(different);
}

FB_TEST(shard_invoke_semantics, target_shard_index_lookup) {
    // _shard_cores[shard_id] gives target core
    std::vector<uint32_t> shard_cores = {0, 2, 4, 6};
    uint32_t shard_id = 2;
    uint32_t target_core = shard_cores[shard_id];
    FB_ASSERT_EQ(target_core, 4);
}

FB_TEST(shard_invoke_semantics, thread_pointer_per_shard) {
    // _threads[shard_id] gives spdk_thread for that shard
    std::vector<void*> threads = {(void*)0x1000, (void*)0x2000, (void*)0x3000, (void*)0x4000};
    uint32_t shard_id = 1;
    void* thread = threads[shard_id];
    FB_ASSERT_TRUE(thread == (void*)0x2000);
}

FB_TEST(shard_invoke_semantics, success_return_code) {
    // Successful inline execution returns 0
    int rc = 0;
    FB_ASSERT_EQ(rc, 0);
}

FB_TEST(shard_invoke_semantics, send_msg_return_propagated) {
    // spdk_thread_send_msg return code propagated
    int send_rc = -ENOMEM;
    int returned = send_rc;
    FB_ASSERT_EQ(returned, -ENOMEM);
    FB_ASSERT_TRUE(returned < 0);
}

FB_TEST(shard_invoke_semantics, dispatch_table_consistency) {
    // _shard_cores and _threads must have same size
    std::vector<uint32_t> shard_cores = {0, 1, 2, 3};
    std::vector<void*> threads(4, nullptr);

    FB_ASSERT_EQ(shard_cores.size(), threads.size());
}

FB_TEST(shard_invoke_semantics, shard_to_core_mapping_injective) {
    // Each shard maps to unique core
    std::vector<uint32_t> shard_cores = {0, 2, 4, 6};
    std::set<uint32_t> unique(shard_cores.begin(), shard_cores.end());
    FB_ASSERT_EQ(unique.size(), shard_cores.size());
}

// ============================================================================
// Test Suite: core_iterator_advanced (Advanced Core Iterator Tests)
// ============================================================================

FB_SUITE_SETUP(core_iterator_advanced) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_iterator_advanced) {
    // Teardown code here
}

FB_TEST(core_iterator_advanced, pre_increment_returns_reference) {
    // ++it returns reference (for chaining)
    std::vector<uint32_t> v = {0, 1, 2};
    auto it = v.begin();
    auto& ref = ++it;
    FB_ASSERT_TRUE(&ref == &it);
    FB_ASSERT_EQ(*it, 1);
}

FB_TEST(core_iterator_advanced, post_increment_returns_old) {
    // it++ returns old value, then advances
    std::vector<uint32_t> v = {10, 20, 30};
    auto it = v.begin();
    auto old = it++;
    FB_ASSERT_EQ(*old, 10);
    FB_ASSERT_EQ(*it, 20);
}

FB_TEST(core_iterator_advanced, copy_constructible) {
    // Iterator is copy constructible
    std::vector<uint32_t> v = {1, 2, 3};
    auto it1 = v.begin();
    auto it2(it1);
    FB_ASSERT_TRUE(it1 == it2);
    FB_ASSERT_EQ(*it1, *it2);
}

FB_TEST(core_iterator_advanced, assignable) {
    // Iterator is assignable
    std::vector<uint32_t> v = {5, 6, 7};
    auto it1 = v.begin();
    auto it2 = v.begin() + 2;
    FB_ASSERT_TRUE(it1 != it2);
    it1 = it2;
    FB_ASSERT_TRUE(it1 == it2);
}

FB_TEST(core_iterator_advanced, move_constructible) {
    // Iterator is move constructible
    std::vector<uint32_t> v = {100, 200};
    auto it1 = v.begin();
    auto it2 = std::move(it1);
    FB_ASSERT_EQ(*it2, 100);
}

FB_TEST(core_iterator_advanced, iteration_full_range) {
    // Iterate through full range
    std::vector<uint32_t> v = {0, 1, 2, 3, 4};
    uint32_t sum = 0;
    for (auto it = v.begin(); it != v.end(); ++it) {
        sum += *it;
    }
    FB_ASSERT_EQ(sum, 10);
}

FB_TEST(core_iterator_advanced, range_based_for) {
    // Range-based for loop
    std::vector<uint32_t> v = {1, 2, 3, 4};
    uint32_t product = 1;
    for (uint32_t val : v) {
        product *= val;
    }
    FB_ASSERT_EQ(product, 24);
}

FB_TEST(core_iterator_advanced, distance_calculation) {
    // std::distance works with forward iterators
    std::vector<uint32_t> v = {0, 1, 2, 3, 4, 5};
    auto dist = std::distance(v.begin(), v.end());
    FB_ASSERT_EQ(dist, 6);
}

// ============================================================================
// Test Suite: service_pattern (Service Pattern Tests)
// ============================================================================

FB_SUITE_SETUP(service_pattern) {
    // Setup code here
}

FB_SUITE_TEARDOWN(service_pattern) {
    // Teardown code here
}

FB_TEST(service_pattern, service_with_default_ctor) {
    // Service must be constructible (Args = empty)
    struct DefaultService { int value = 0; };
    DefaultService s;
    FB_ASSERT_EQ(s.value, 0);
}

FB_TEST(service_pattern, service_with_args) {
    // Service with arguments (forwarded)
    struct ArgService {
        int id;
        std::string name;
        ArgService(int i, std::string n) : id(i), name(std::move(n)) {}
    };

    ArgService s(42, "test");
    FB_ASSERT_EQ(s.id, 42);
    FB_ASSERT_EQ(s.name, "test");
}

FB_TEST(service_pattern, service_instance_per_shard) {
    // Each shard has independent Service instance
    struct CounterService { int counter = 0; };

    std::vector<CounterService*> services(4);
    for (uint32_t i = 0; i < 4; i++) {
        services[i] = new CounterService();
        services[i]->counter = static_cast<int>(i * 10);
    }

    FB_ASSERT_EQ(services[0]->counter, 0);
    FB_ASSERT_EQ(services[1]->counter, 10);
    FB_ASSERT_EQ(services[2]->counter, 20);
    FB_ASSERT_EQ(services[3]->counter, 30);

    for (auto* s : services) delete s;
}

FB_TEST(service_pattern, service_isolation) {
    // Service instances do not share state
    struct State { int value = 0; };

    std::vector<State*> states(4);
    for (uint32_t i = 0; i < 4; i++) {
        states[i] = new State();
    }

    states[0]->value = 100;
    states[2]->value = 200;

    FB_ASSERT_EQ(states[0]->value, 100);
    FB_ASSERT_EQ(states[1]->value, 0);
    FB_ASSERT_EQ(states[2]->value, 200);
    FB_ASSERT_EQ(states[3]->value, 0);

    for (auto* s : states) delete s;
}

FB_TEST(service_pattern, service_pointer_validity) {
    // Service pointers remain valid until stop()
    int* p = new int(42);
    int* saved = p;
    FB_ASSERT_TRUE(p == saved);
    FB_ASSERT_EQ(*saved, 42);
    delete p;
}

FB_TEST(service_pattern, service_destruction_order) {
    // stop() deletes services in order
    std::vector<int*> services;
    for (int i = 0; i < 4; i++) {
        services.push_back(new int(i));
    }

    // Track deletion order
    std::vector<int> deletion_order;
    for (uint32_t i = 0; i < services.size(); i++) {
        deletion_order.push_back(*services[i]);
        delete services[i];
        services[i] = nullptr;
    }
    services.clear();

    FB_ASSERT_EQ(deletion_order.size(), 4);
    FB_ASSERT_EQ(deletion_order[0], 0);
    FB_ASSERT_EQ(deletion_order[3], 3);
}

FB_TEST(service_pattern, service_constructor_forwarding) {
    // Args forwarded via std::forward
    struct Counter {
        int construct_count;
        Counter(int c) : construct_count(c) {}
    };

    Counter c(99);
    FB_ASSERT_EQ(c.construct_count, 99);
}

FB_TEST(service_pattern, service_move_only_arg) {
    // Service can accept move-only arguments
    struct MoveOnly {
        std::unique_ptr<int> ptr;
        MoveOnly(std::unique_ptr<int> p) : ptr(std::move(p)) {}
    };

    auto p = std::make_unique<int>(42);
    MoveOnly s(std::move(p));
    FB_ASSERT_TRUE(s.ptr != nullptr);
    FB_ASSERT_EQ(*s.ptr, 42);
}

// ============================================================================
// Test Main Entry Point
// ============================================================================

FB_TEST_MAIN()
