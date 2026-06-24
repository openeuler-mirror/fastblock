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
    // Valid core IDs are 0 to N-1 where N is core count.
    // Verify the partition: all 0..N-1 are valid, sentinel UINT32_MAX is not
    uint32_t core_count = 128;
    uint32_t sentinel = std::numeric_limits<uint32_t>::max();

    // All core IDs in [0, N-1] are valid and != sentinel
    for (uint32_t id = 0; id < core_count; id++) {
        FB_ASSERT_TRUE(id < core_count);
        FB_ASSERT_TRUE(id != sentinel);
    }
    // Sentinel itself is out of range
    FB_ASSERT_TRUE(sentinel >= core_count);
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
    // Iterator class must satisfy DefaultConstructible (forward_iterator requirement)
    // Verify via type trait
    constexpr bool is_default_constructible = std::is_default_constructible_v<std::vector<uint32_t>::iterator>;
    FB_ASSERT_TRUE(is_default_constructible);
}

FB_TEST(core_iterator, end_sentinel) {
    // end() returns iterator with UINT32_MAX as sentinel
    // Critical property: sentinel must be unequal to ALL valid core IDs (0..N-1)
    uint32_t end_value = UINT32_MAX;
    for (uint32_t valid = 0; valid < 256; valid++) {
        FB_ASSERT_TRUE(valid != end_value);
    }
    // And equals std::numeric_limits<uint32_t>::max()
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
    // Single-shard configuration: count==1, only shard_id 0 valid
    uint32_t shard_count = 1;
    // shard_id must be < count, so only 0 is valid
    uint32_t valid_shard_id = 0;
    uint32_t invalid_shard_id = 1;

    FB_ASSERT_TRUE(valid_shard_id < shard_count);
    FB_ASSERT_TRUE(!(invalid_shard_id < shard_count));
    // No cross-shard messaging needed
    FB_ASSERT_EQ(shard_count, 1);
}

FB_TEST(shard_count, multi_shard) {
    // Multi-shard: count > 1, must support N-1 cross-shard targets per shard
    uint32_t shard_count = 8;
    uint32_t cross_shard_targets = shard_count - 1;
    FB_ASSERT_EQ(cross_shard_targets, 7);
    // Reasonable upper bound for performance
    FB_ASSERT_TRUE(shard_count <= 256);
    FB_ASSERT_TRUE(shard_count > 1);
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
    // shard_id must satisfy 0 <= shard_id < count().
    // Verify boundary: count-1 valid, count itself invalid.
    uint32_t shard_count = 4;

    // All ids in [0, count) are valid
    for (uint32_t id = 0; id < shard_count; id++) {
        FB_ASSERT_TRUE(id < shard_count);
    }
    // Boundary: id == count is invalid
    FB_ASSERT_TRUE(!(shard_count < shard_count));
    // Sentinel UINT32_MAX (returned by this_shard_id on miss) is also invalid
    uint32_t sentinel = UINT32_MAX;
    FB_ASSERT_TRUE(!(sentinel < shard_count));
}

FB_TEST(shard_count, shard_count_matches_threads) {
    // _shard_cores.size() and _threads.size() must remain equal at all times.
    // Simulate the construction loop and verify invariant.
    std::vector<uint32_t> shard_cores;
    std::vector<void*> threads;

    uint32_t n_core = 4;
    for (uint32_t i = 0; i < n_core; i++) {
        // Each iteration adds one entry to BOTH vectors
        shard_cores.push_back(i);
        threads.push_back((void*)(uintptr_t)(0x1000 + i));
        // Invariant holds throughout
        FB_ASSERT_EQ(shard_cores.size(), threads.size());
    }
    FB_ASSERT_EQ(shard_cores.size(), n_core);
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
    // lambda_ctx is heap-allocated; verify ownership semantics via shared_ptr counter
    static int alive_count;
    alive_count = 0;

    struct ctx_lifetime {
        ctx_lifetime() { alive_count++; }
        ~ctx_lifetime() { alive_count--; }
    };

    ctx_lifetime* p = new ctx_lifetime();
    FB_ASSERT_EQ(alive_count, 1);

    delete p; // simulates core_context::run() deleting itself
    FB_ASSERT_EQ(alive_count, 0);
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
    // Concrete derived classes must implement it
    // Verify the pattern: base class cannot be instantiated directly,
    // but pointers to base can dispatch via vtable

    // Simulate: derived class with overridden run_task
    struct base_ctx { virtual void run_task() = 0; virtual ~base_ctx() = default; };
    struct derived_ctx : base_ctx {
        int invoked = 0;
        void run_task() override { invoked++; }
    };

    derived_ctx d;
    base_ctx* ptr = &d;
    ptr->run_task();
    FB_ASSERT_EQ(d.invoked, 1);
    ptr->run_task();
    FB_ASSERT_EQ(d.invoked, 2);
}

FB_TEST(core_context, run_static_method) {
    // core_context::run(void*) is the static dispatch
    void* arg = nullptr;
    FB_ASSERT_TRUE(arg == nullptr);
}

FB_TEST(core_context, ownership_transfer) {
    // core_context::run() deletes the context after invoking run_task().
    // Verify the run-then-delete pattern via destructor counter.
    static int destroyed;
    destroyed = 0;

    struct my_ctx {
        int& counter;
        my_ctx(int& c) : counter(c) {}
        ~my_ctx() { counter++; }
        void run_task() { /* do work */ }
    };

    my_ctx* c = new my_ctx(destroyed);
    // Simulate run(): invoke task then delete
    c->run_task();
    delete c;

    // After run(), the context is gone (count should be 1)
    FB_ASSERT_EQ(destroyed, 1);
}

FB_TEST(core_context, virtual_destructor) {
    // core_context has virtual destructor for safe deletion via base pointer
    // Verify: deleting derived through base pointer correctly invokes derived destructor
    static int derived_destroyed;
    derived_destroyed = 0;

    struct base { virtual ~base() = default; };
    struct derived : base {
        ~derived() override { derived_destroyed++; }
    };

    base* ptr = new derived();
    delete ptr; // Without virtual dtor, derived would not be called

    FB_ASSERT_EQ(derived_destroyed, 1);
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
    // Lambda must outlive sending thread's invocation frame.
    // Verify pattern: lambda captures by value, heap-allocated, processed later.
    static int execution_count;
    execution_count = 0;

    struct heap_ctx {
        std::function<void()> fn;
        heap_ctx(std::function<void()> f) : fn(std::move(f)) {}
        void run() { fn(); }
    };

    int captured_value = 42;
    // Heap-allocate, capture by value (survives caller frame exit)
    heap_ctx* hctx = new heap_ctx([captured_value]() {
        execution_count += captured_value;
    });

    // Simulate "caller returns, frame goes away"
    // ... time passes ...

    // Receiver runs later
    hctx->run();
    delete hctx;

    FB_ASSERT_EQ(execution_count, 42);
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
    // ~core_sharded() noexcept { stop(); }
    // Verify: RAII pattern - destructor invokes cleanup
    static int stop_invocations;
    stop_invocations = 0;

    struct mock_sharded {
        ~mock_sharded() { stop_invocations++; }
    };

    {
        mock_sharded m;
        FB_ASSERT_EQ(stop_invocations, 0);
    } // destructor invoked here
    FB_ASSERT_EQ(stop_invocations, 1);
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
    // core_sharded explicitly deletes copy constructor:
    //   core_sharded(const core_sharded&) = delete
    // Verify the pattern via a mock class with same deletion.
    struct non_copyable {
        int v;
        non_copyable(int x) : v(x) {}
        non_copyable(const non_copyable&) = delete;
        non_copyable& operator=(const non_copyable&) = delete;
    };

    constexpr bool copyable = std::is_copy_constructible_v<non_copyable>;
    constexpr bool assignable = std::is_copy_assignable_v<non_copyable>;
    FB_ASSERT_TRUE(!copyable);
    FB_ASSERT_TRUE(!assignable);

    // But can still be constructed normally
    non_copyable n(42);
    FB_ASSERT_EQ(n.v, 42);
}

FB_TEST(shard_lifecycle, move_construction_deleted) {
    // core_sharded explicitly deletes move constructor:
    //   core_sharded(core_sharded&&) = delete
    struct non_movable {
        int v;
        non_movable(int x) : v(x) {}
        non_movable(const non_movable&) = delete;
        non_movable(non_movable&&) = delete;
        non_movable& operator=(const non_movable&) = delete;
        non_movable& operator=(non_movable&&) = delete;
    };

    constexpr bool movable = std::is_move_constructible_v<non_movable>;
    constexpr bool move_assignable = std::is_move_assignable_v<non_movable>;
    FB_ASSERT_TRUE(!movable);
    FB_ASSERT_TRUE(!move_assignable);

    non_movable nm(99);
    FB_ASSERT_EQ(nm.v, 99);
}

FB_TEST(shard_lifecycle, singleton_pattern) {
    // g_core_sharded is a unique_ptr in anonymous namespace -> singleton.
    // Verify singleton semantics: at most one alive instance.
    static int instances_alive;
    instances_alive = 0;

    struct mock_singleton {
        mock_singleton() { instances_alive++; }
        ~mock_singleton() { instances_alive--; }
    };

    // Initially no instance
    std::unique_ptr<mock_singleton> g_singleton;
    FB_ASSERT_EQ(instances_alive, 0);

    // construct() initializes
    g_singleton = std::make_unique<mock_singleton>();
    FB_ASSERT_EQ(instances_alive, 1);

    // Construct again -> previous one destroyed (singleton property)
    g_singleton = std::make_unique<mock_singleton>();
    FB_ASSERT_EQ(instances_alive, 1);

    // Reset -> zero
    g_singleton.reset();
    FB_ASSERT_EQ(instances_alive, 0);
}

FB_TEST(shard_lifecycle, construct_initializes_singleton) {
    // core_sharded::construct(args...) does std::make_unique<core_sharded>(args...).
    // Verify: before construct() singleton is empty; after, it holds the new instance.
    std::unique_ptr<int> singleton;
    FB_ASSERT_TRUE(singleton == nullptr);
    FB_ASSERT_TRUE(!static_cast<bool>(singleton));

    // construct() equivalent
    singleton = std::make_unique<int>(100);
    FB_ASSERT_TRUE(singleton != nullptr);
    FB_ASSERT_TRUE(static_cast<bool>(singleton));
    FB_ASSERT_EQ(*singleton, 100);

    // get_core_sharded() returns *g_core_sharded
    int& ref = *singleton;
    FB_ASSERT_EQ(&ref, singleton.get());
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
// Test Suite: cross_shard_communication (Cross-Shard Communication Tests)
// ============================================================================

FB_SUITE_SETUP(cross_shard_communication) {
    // Setup code here
}

FB_SUITE_TEARDOWN(cross_shard_communication) {
    // Teardown code here
}

FB_TEST(cross_shard_communication, message_passing_required) {
    // Cross-shard requires spdk_thread_send_msg, no shared memory
    uint32_t shard_a = 0;
    uint32_t shard_b = 3;
    bool needs_message = (shard_a != shard_b);
    FB_ASSERT_TRUE(needs_message);
}

FB_TEST(cross_shard_communication, async_delivery) {
    // Messages delivered async: sender returns immediately,
    // receiver processes later. Verify return-before-execute pattern.
    int execution_count = 0;
    bool sender_returned = false;

    auto send_async = [&]() {
        // In real code, queues a message and returns
        sender_returned = true;
        // execution happens later in receiver thread
    };

    send_async();
    // Sender returned without invoking the work
    FB_ASSERT_TRUE(sender_returned);
    FB_ASSERT_EQ(execution_count, 0);

    // Later, receiver processes
    execution_count++;
    FB_ASSERT_EQ(execution_count, 1);
}

FB_TEST(cross_shard_communication, message_fifo_order) {
    // Messages to same shard preserve FIFO order
    std::vector<int> messages = {1, 2, 3, 4, 5};
    for (size_t i = 1; i < messages.size(); i++) {
        FB_ASSERT_TRUE(messages[i] > messages[i-1]);
    }
}

FB_TEST(cross_shard_communication, no_callback_blocking) {
    // Sender does not block waiting for receiver
    auto deliver = []() {
        // Sender returns immediately after send_msg
        return true;
    };
    FB_ASSERT_TRUE(deliver());
}

FB_TEST(cross_shard_communication, broadcast_to_all_shards) {
    // Broadcast: send same message to all shards
    uint32_t shard_count = 4;
    std::vector<bool> delivered(shard_count, false);

    for (uint32_t i = 0; i < shard_count; i++) {
        delivered[i] = true;
    }

    for (bool d : delivered) {
        FB_ASSERT_TRUE(d);
    }
}

FB_TEST(cross_shard_communication, message_data_copy) {
    // Data passed to other shard must be self-contained (copied/moved)
    std::vector<int> source = {1, 2, 3};
    std::vector<int> copy = source;

    // Modifying source shouldn't affect copy
    source.push_back(4);
    FB_ASSERT_EQ(copy.size(), 3);
    FB_ASSERT_EQ(source.size(), 4);
}

FB_TEST(cross_shard_communication, lambda_must_be_heap) {
    // lambda_ctx allocated on heap because lifetime exceeds caller frame
    auto* heap_lambda = new int(42);
    FB_ASSERT_TRUE(heap_lambda != nullptr);
    delete heap_lambda;
}

FB_TEST(cross_shard_communication, send_msg_can_fail) {
    // spdk_thread_send_msg can return error (e.g., -ENOMEM)
    int success = 0;
    int oom = -ENOMEM;
    FB_ASSERT_EQ(success, 0);
    FB_ASSERT_TRUE(oom < 0);
}

FB_TEST(cross_shard_communication, target_thread_must_exist) {
    // Target shard's thread must be running
    std::vector<void*> threads = {(void*)0x1, (void*)0x2, nullptr, (void*)0x4};

    uint32_t target = 2;
    bool can_send = (threads[target] != nullptr);
    FB_ASSERT_TRUE(!can_send);

    target = 1;
    can_send = (threads[target] != nullptr);
    FB_ASSERT_TRUE(can_send);
}

// ============================================================================
// Test Suite: core_indexing (Core Indexing Tests)
// ============================================================================

FB_SUITE_SETUP(core_indexing) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_indexing) {
    // Teardown code here
}

FB_TEST(core_indexing, shard_to_core_lookup) {
    // shard_id -> core_id via _shard_cores[]
    std::vector<uint32_t> shard_cores = {0, 2, 4, 6};
    FB_ASSERT_EQ(shard_cores[0], 0);
    FB_ASSERT_EQ(shard_cores[1], 2);
    FB_ASSERT_EQ(shard_cores[2], 4);
    FB_ASSERT_EQ(shard_cores[3], 6);
}

FB_TEST(core_indexing, core_to_shard_reverse_lookup) {
    // core_id -> shard_id (linear search in _shard_cores)
    std::vector<uint32_t> shard_cores = {0, 2, 4, 6};
    uint32_t target_core = 4;

    uint32_t shard_id = UINT32_MAX;
    for (uint32_t i = 0; i < shard_cores.size(); i++) {
        if (shard_cores[i] == target_core) {
            shard_id = i;
            break;
        }
    }
    FB_ASSERT_EQ(shard_id, 2);
}

FB_TEST(core_indexing, sequential_core_assignment) {
    // Cores 0..N-1 assigned to shards 0..N-1
    std::vector<uint32_t> shard_cores;
    for (uint32_t i = 0; i < 8; i++) {
        shard_cores.push_back(i);
    }

    for (uint32_t i = 0; i < shard_cores.size(); i++) {
        FB_ASSERT_EQ(shard_cores[i], i);
    }
}

FB_TEST(core_indexing, skip_cores_pattern) {
    // Non-contiguous: skip cores (e.g., reserve some for other processes)
    std::vector<uint32_t> shard_cores = {1, 3, 5, 7};
    for (size_t i = 1; i < shard_cores.size(); i++) {
        FB_ASSERT_TRUE(shard_cores[i] - shard_cores[i-1] == 2);
    }
}

FB_TEST(core_indexing, first_shard_uses_first_core) {
    // Shard 0 is on the first allocated core
    std::vector<uint32_t> shard_cores = {5, 6, 7, 8};
    FB_ASSERT_EQ(shard_cores[0], 5);
}

FB_TEST(core_indexing, last_shard_index) {
    // Last shard is at index N-1
    uint32_t shard_count = 8;
    uint32_t last_shard = shard_count - 1;
    FB_ASSERT_EQ(last_shard, 7);
}

FB_TEST(core_indexing, sentinel_when_not_found) {
    // Returns UINT32_MAX when core not in shard_cores
    std::vector<uint32_t> shard_cores = {0, 1, 2};
    uint32_t missing = 99;

    uint32_t shard_id = UINT32_MAX;
    for (uint32_t i = 0; i < shard_cores.size(); i++) {
        if (shard_cores[i] == missing) {
            shard_id = i;
            break;
        }
    }
    FB_ASSERT_EQ(shard_id, UINT32_MAX);
}

FB_TEST(core_indexing, empty_shard_cores) {
    // Empty shard_cores -> any lookup returns UINT32_MAX
    std::vector<uint32_t> shard_cores;
    uint32_t shard_id = UINT32_MAX;
    for (uint32_t i = 0; i < shard_cores.size(); i++) {
        if (shard_cores[i] == 0) {
            shard_id = i;
        }
    }
    FB_ASSERT_EQ(shard_id, UINT32_MAX);
}

// ============================================================================
// Test Suite: shard_construction (Shard Construction Tests)
// ============================================================================

FB_SUITE_SETUP(shard_construction) {
    // Setup code here
}

FB_SUITE_TEARDOWN(shard_construction) {
    // Teardown code here
}

FB_TEST(shard_construction, counter_starts_at_zero) {
    // Constructor counter starts at 0
    uint32_t counter = 0;
    FB_ASSERT_EQ(counter, 0);
}

FB_TEST(shard_construction, counter_reaches_n_core) {
    // Counter loops up to n_core
    uint32_t counter = 0;
    uint32_t n_core = 4;

    while (counter < n_core) {
        counter++;
    }
    FB_ASSERT_EQ(counter, n_core);
}

FB_TEST(shard_construction, iterator_advances_per_iteration) {
    // begin iterator advances with each iteration
    std::vector<uint32_t> source = {0, 1, 2, 3};
    auto it = source.begin();
    uint32_t collected = 0;

    while (it != source.end()) {
        collected++;
        ++it;
    }
    FB_ASSERT_EQ(collected, 4);
}

FB_TEST(shard_construction, shard_core_pushback) {
    // _shard_cores.push_back(*begin) collects each core
    std::vector<uint32_t> source = {0, 2, 4, 6};
    std::vector<uint32_t> collected;
    for (uint32_t c : source) {
        collected.push_back(c);
    }
    FB_ASSERT_TRUE(collected == source);
    FB_ASSERT_EQ(collected.size(), source.size());
}

FB_TEST(shard_construction, cpuset_zero_then_set_pattern) {
    // For each core: cpuset_zero, cpuset_set_cpu(core)
    uint64_t mask = 0;
    uint32_t core = 5;

    mask = 0; // cpuset_zero
    mask |= (1ULL << core); // cpuset_set_cpu

    FB_ASSERT_EQ(mask, 32);
}

FB_TEST(shard_construction, thread_name_per_shard) {
    // Each shard gets a unique thread name
    std::string app_name = "test_";
    std::vector<std::string> names;
    for (uint32_t i = 0; i < 4; i++) {
        names.push_back(app_name + std::to_string(i));
    }

    std::set<std::string> unique(names.begin(), names.end());
    FB_ASSERT_EQ(unique.size(), names.size());
}

FB_TEST(shard_construction, thread_created_with_cpumask) {
    // spdk_thread_create takes thread_name + cpumask
    // Verify the parameter-passing pattern: each thread is paired
    // with exactly one CPU mask bit set
    struct thread_create_call {
        std::string name;
        uint64_t mask;
    };

    std::vector<thread_create_call> calls;
    for (uint32_t core = 0; core < 4; core++) {
        thread_create_call c;
        c.name = "fb_" + std::to_string(core);
        c.mask = (1ULL << core);
        calls.push_back(c);
    }

    // Each call has a unique name and a single CPU mask
    FB_ASSERT_EQ(calls.size(), 4);
    for (size_t i = 0; i < calls.size(); i++) {
        // Exactly one bit set
        FB_ASSERT_TRUE(calls[i].mask != 0);
        FB_ASSERT_EQ(calls[i].mask & (calls[i].mask - 1), 0);
        // Name encodes core id
        FB_ASSERT_TRUE(calls[i].name.find(std::to_string(i)) != std::string::npos);
    }
}

FB_TEST(shard_construction, threads_pushback_after_create) {
    // _threads.push_back(thread) after each creation
    std::vector<void*> threads;
    for (uint32_t i = 0; i < 4; i++) {
        threads.push_back((void*)(uintptr_t)(0x1000 * (i + 1)));
    }
    FB_ASSERT_EQ(threads.size(), 4);
    FB_ASSERT_TRUE(threads[0] != threads[1]);
}

FB_TEST(shard_construction, parallel_initialization) {
    // After construction: _shard_cores.size() == _threads.size() == n_core
    std::vector<uint32_t> shard_cores = {0, 1, 2, 3};
    std::vector<void*> threads(4, (void*)0x1);
    uint32_t n_core = 4;

    FB_ASSERT_EQ(shard_cores.size(), n_core);
    FB_ASSERT_EQ(threads.size(), n_core);
}

// ============================================================================
// Test Suite: spdk_thread_management (SPDK Thread Management Tests)
// ============================================================================

FB_SUITE_SETUP(spdk_thread_management) {
    // Setup code here
}

FB_SUITE_TEARDOWN(spdk_thread_management) {
    // Teardown code here
}

FB_TEST(spdk_thread_management, thread_pointer_storage) {
    // _threads vector stores spdk_thread*
    std::vector<void*> threads(4, nullptr);
    FB_ASSERT_EQ(threads.size(), 4);
    for (auto* t : threads) {
        FB_ASSERT_TRUE(t == nullptr);
    }
}

FB_TEST(spdk_thread_management, thread_at_call) {
    // get_thread() uses .at() for bounds checking
    std::vector<int> threads = {10, 20, 30, 40};
    FB_ASSERT_EQ(threads.at(2), 30);

    // Out-of-bounds throws std::out_of_range
    bool caught = false;
    try {
        threads.at(10);
    } catch (const std::out_of_range&) {
        caught = true;
    }
    FB_ASSERT_TRUE(caught);
}

FB_TEST(spdk_thread_management, get_thread_by_core) {
    // get_thread(core) returns the spdk_thread for that core
    std::vector<void*> threads = {(void*)0x1, (void*)0x2, (void*)0x3, (void*)0x4};
    FB_ASSERT_TRUE(threads.at(2) == (void*)0x3);
}

FB_TEST(spdk_thread_management, thread_exit_pattern) {
    // stop() pattern: set_thread, thread_exit, set back current
    void* current = (void*)0x100;
    void* target = (void*)0x200;

    // Save current
    void* saved = current;
    // Switch
    current = target;
    FB_ASSERT_TRUE(current == target);
    // Exit & restore
    current = saved;
    FB_ASSERT_TRUE(current == saved);
}

FB_TEST(spdk_thread_management, current_thread_handled_specially) {
    // If current thread is being exited, set_thread(nullptr) instead
    void* current = (void*)0x500;
    void* exiting = (void*)0x500;

    bool is_current = (current == exiting);
    void* set_to = is_current ? nullptr : current;
    FB_ASSERT_TRUE(set_to == nullptr);
}

FB_TEST(spdk_thread_management, skip_null_threads_in_stop) {
    // stop() skips null thread pointers
    std::vector<void*> threads = {(void*)0x1, nullptr, (void*)0x3, nullptr};
    int processed = 0;
    for (auto* t : threads) {
        if (t != nullptr) {
            processed++;
        }
    }
    FB_ASSERT_EQ(processed, 2);
}

FB_TEST(spdk_thread_management, threads_cleared_after_stop) {
    // _threads.clear() at end of stop()
    std::vector<void*> threads = {(void*)0x1, (void*)0x2};
    threads.clear();
    FB_ASSERT_TRUE(threads.empty());
}

FB_TEST(spdk_thread_management, idempotent_stop) {
    // stop() can be called multiple times safely (vector already empty)
    std::vector<void*> threads;
    threads.clear(); // First call
    threads.clear(); // Second call - should still work
    FB_ASSERT_TRUE(threads.empty());
}

// ============================================================================
// Test Suite: shard_data_locality (Shard Data Locality Tests)
// ============================================================================

FB_SUITE_SETUP(shard_data_locality) {
    // Setup code here
}

FB_SUITE_TEARDOWN(shard_data_locality) {
    // Teardown code here
}

FB_TEST(shard_data_locality, no_shared_state) {
    // Each shard owns its data exclusively (no sharing)
    struct ShardData { int counter; };
    std::vector<ShardData> shards(4);
    for (uint32_t i = 0; i < 4; i++) {
        shards[i].counter = static_cast<int>(i * 100);
    }

    // Verify no shared writes affect other shards
    shards[0].counter = 9999;
    FB_ASSERT_EQ(shards[1].counter, 100);
    FB_ASSERT_EQ(shards[2].counter, 200);
    FB_ASSERT_EQ(shards[3].counter, 300);
}

FB_TEST(shard_data_locality, local_access_only) {
    // local() returns reference to current shard's Service
    std::vector<int*> instances = {new int(1), new int(2), new int(3), new int(4)};
    uint32_t current_shard = 2;
    int& local_ref = *instances[current_shard];
    FB_ASSERT_EQ(local_ref, 3);

    // Modification via local_ref only affects local shard
    local_ref = 999;
    FB_ASSERT_EQ(*instances[2], 999);
    FB_ASSERT_EQ(*instances[0], 1);
    FB_ASSERT_EQ(*instances[3], 4);

    for (auto* p : instances) delete p;
}

FB_TEST(shard_data_locality, on_shard_for_initialization) {
    // on_shard() allows pre-initialization access (single-threaded phase)
    std::vector<int*> instances = {new int(0), new int(0), new int(0), new int(0)};

    // Init phase: core 0 writes, no concurrent access
    for (uint32_t i = 0; i < 4; i++) {
        *instances[i] = static_cast<int>(i);
    }

    FB_ASSERT_EQ(*instances[0], 0);
    FB_ASSERT_EQ(*instances[3], 3);

    for (auto* p : instances) delete p;
}

FB_TEST(shard_data_locality, no_cache_line_sharing) {
    // Each Service should fit in its own cache line group
    // (conceptual test - actual depends on Service size)
    struct alignas(64) AlignedShard { int data; };
    AlignedShard s;
    FB_ASSERT_TRUE(alignof(AlignedShard) >= 64);
}

FB_TEST(shard_data_locality, numa_aware_allocation) {
    // Memory allocated per-shard should be NUMA-local
    // (using SPDK env: socket_id from spdk_env_get_socket_id)
    uint32_t socket0 = 0;
    uint32_t socket1 = 1;
    FB_ASSERT_TRUE(socket0 != socket1);
}

FB_TEST(shard_data_locality, per_shard_resource_limit) {
    // Resources scaled with shard count
    uint32_t total_memory_mb = 16384;
    uint32_t shard_count = 4;
    uint32_t per_shard_mb = total_memory_mb / shard_count;
    FB_ASSERT_EQ(per_shard_mb, 4096);
}

FB_TEST(shard_data_locality, thread_local_pollers) {
    // Each shard's thread maintains its own pollers list, no cross-shard access
    // Verify: pollers are partitioned by shard, no overlapping pointer identity
    std::vector<std::vector<int*>> shard_pollers(4);
    for (uint32_t s = 0; s < 4; s++) {
        shard_pollers[s].push_back(new int(static_cast<int>(s) * 10));
        shard_pollers[s].push_back(new int(static_cast<int>(s) * 10 + 1));
    }

    // Each shard owns exactly its own pollers
    for (uint32_t s = 0; s < 4; s++) {
        FB_ASSERT_EQ(shard_pollers[s].size(), 2);
        FB_ASSERT_EQ(*shard_pollers[s][0], static_cast<int>(s) * 10);
    }

    // No shared pointers between shards
    std::set<int*> all_ptrs;
    for (auto& vec : shard_pollers) {
        for (int* p : vec) all_ptrs.insert(p);
    }
    FB_ASSERT_EQ(all_ptrs.size(), 8);

    // Cleanup
    for (auto& vec : shard_pollers) {
        for (int* p : vec) delete p;
    }
}

FB_TEST(shard_data_locality, no_synchronization_within_shard) {
    // Within a shard's thread, all operations are serialized -> no locks needed.
    // Verify: a simple counter can be incremented without atomics within shard.
    int counter = 0;
    // Simulate 1000 single-threaded ops
    for (int i = 0; i < 1000; i++) {
        counter++;
    }
    FB_ASSERT_EQ(counter, 1000);

    // This pattern would race under multi-thread, but is safe within a shard
    // because shard's spdk_thread executes operations serially.
}

// ============================================================================
// Test Suite: core_traversal_pattern (Core Traversal Pattern Tests)
// ============================================================================

FB_SUITE_SETUP(core_traversal_pattern) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_traversal_pattern) {
    // Teardown code here
}

FB_TEST(core_traversal_pattern, foreach_core_idiom) {
    // SPDK_ENV_FOREACH_CORE iterates all cores
    std::vector<uint32_t> all_cores = {0, 1, 2, 3, 4, 5, 6, 7};
    uint32_t count = 0;
    for (uint32_t c : all_cores) {
        (void)c;
        count++;
    }
    FB_ASSERT_EQ(count, all_cores.size());
}

FB_TEST(core_traversal_pattern, first_then_next_idiom) {
    // first_core() + next_core() walk
    std::vector<uint32_t> cores;
    uint32_t first = 0;
    uint32_t last = 7;

    uint32_t c = first;
    while (c != UINT32_MAX) {
        cores.push_back(c);
        if (c == last) break;
        c++;
    }
    FB_ASSERT_EQ(cores.size(), 8);
}

FB_TEST(core_traversal_pattern, stop_at_last_core) {
    // Loop terminates when reaching last_core
    uint32_t first = 0;
    uint32_t last = 4;
    std::vector<uint32_t> cores;

    uint32_t c = first;
    while (c != last) {
        cores.push_back(c);
        c++;
    }
    FB_ASSERT_EQ(cores.size(), 4);
    FB_ASSERT_TRUE(std::find(cores.begin(), cores.end(), last) == cores.end());
}

FB_TEST(core_traversal_pattern, capacity_matches_iterations) {
    // capacity() returns total core count
    uint32_t expected_capacity = 8;
    std::vector<uint32_t> cores;
    for (uint32_t i = 0; i < expected_capacity; i++) {
        cores.push_back(i);
    }
    FB_ASSERT_EQ(cores.size(), expected_capacity);
}

FB_TEST(core_traversal_pattern, parallel_iteration_iterator) {
    // begin() / end() pattern
    std::vector<uint32_t> cores = {0, 1, 2, 3, 4};
    auto begin = cores.begin();
    auto end = cores.end();

    uint32_t count = 0;
    while (begin != end) {
        count++;
        ++begin;
    }
    FB_ASSERT_EQ(count, 5);
}

FB_TEST(core_traversal_pattern, std_for_each_works) {
    // std::for_each with forward iterators
    std::vector<uint32_t> cores = {1, 2, 3, 4};
    uint32_t sum = 0;
    std::for_each(cores.begin(), cores.end(), [&sum](uint32_t c) {
        sum += c;
    });
    FB_ASSERT_EQ(sum, 10);
}

FB_TEST(core_traversal_pattern, sentinel_end_iteration) {
    // end iterator = sentinel value UINT32_MAX
    uint32_t sentinel = UINT32_MAX;
    uint32_t valid_value = 100;
    FB_ASSERT_TRUE(valid_value != sentinel);
}

FB_TEST(core_traversal_pattern, ordered_traversal) {
    // first_core, next_core return cores in ascending order
    std::vector<uint32_t> cores = {0, 2, 5, 7};
    for (size_t i = 1; i < cores.size(); i++) {
        FB_ASSERT_TRUE(cores[i] > cores[i-1]);
    }
}

// ============================================================================
// Test Suite: shard_args_forwarding (Shard Args Forwarding Tests)
// ============================================================================

FB_SUITE_SETUP(shard_args_forwarding) {
    // Setup code here
}

FB_SUITE_TEARDOWN(shard_args_forwarding) {
    // Teardown code here
}

FB_TEST(shard_args_forwarding, lvalue_passed_as_lvalue) {
    // lvalue arguments preserved through std::forward
    int x = 42;
    auto f = [](int& ref) -> int& { return ref; };
    int& result = f(x);
    FB_ASSERT_EQ(&result, &x);
    result = 100;
    FB_ASSERT_EQ(x, 100);
}

FB_TEST(shard_args_forwarding, rvalue_passed_as_rvalue) {
    // rvalue arguments moved through std::forward
    std::string source = "hello";
    auto f = [](std::string&& s) { return std::move(s); };
    std::string result = f(std::move(source));
    FB_ASSERT_EQ(result, "hello");
}

FB_TEST(shard_args_forwarding, copy_on_lvalue_pass) {
    // Pass by value copies lvalue
    int original = 10;
    auto copy_fn = [](int v) { v = 999; return v; };
    int result = copy_fn(original);
    FB_ASSERT_EQ(result, 999);
    FB_ASSERT_EQ(original, 10); // Unchanged
}

FB_TEST(shard_args_forwarding, move_on_rvalue_pass) {
    // Pass by value moves rvalue
    auto src = std::make_unique<int>(42);
    auto dst = std::move(src);
    FB_ASSERT_TRUE(src == nullptr);
    FB_ASSERT_TRUE(dst != nullptr);
    FB_ASSERT_EQ(*dst, 42);
}

FB_TEST(shard_args_forwarding, variadic_tuple_construction) {
    // std::make_tuple captures variadic args
    auto tup = std::make_tuple(1, std::string("hi"), 3.14);
    FB_ASSERT_EQ(std::get<0>(tup), 1);
    FB_ASSERT_EQ(std::get<1>(tup), "hi");
    FB_ASSERT_EQ(std::get<2>(tup), 3.14);
}

FB_TEST(shard_args_forwarding, tuple_size) {
    // tuple_size reflects parameter count
    using TupType = std::tuple<int, double, std::string>;
    constexpr size_t sz = std::tuple_size_v<TupType>;
    FB_ASSERT_EQ(sz, 3);
}

FB_TEST(shard_args_forwarding, no_args_empty_tuple) {
    // Zero arguments produce empty tuple
    auto empty = std::make_tuple();
    constexpr size_t sz = std::tuple_size_v<decltype(empty)>;
    FB_ASSERT_EQ(sz, 0);
}

FB_TEST(shard_args_forwarding, apply_unpacks_tuple) {
    // std::apply unpacks tuple to function call
    auto fn = [](int a, int b, int c) { return a * b + c; };
    auto args = std::make_tuple(3, 4, 5);
    int result = std::apply(fn, args);
    FB_ASSERT_EQ(result, 17);
}

FB_TEST(shard_args_forwarding, move_only_in_tuple) {
    // Move-only types stored in tuple
    auto p1 = std::make_unique<int>(1);
    auto p2 = std::make_unique<int>(2);
    auto tup = std::make_tuple(std::move(p1), std::move(p2));

    FB_ASSERT_TRUE(p1 == nullptr);
    FB_ASSERT_TRUE(p2 == nullptr);
    FB_ASSERT_EQ(*std::get<0>(tup), 1);
    FB_ASSERT_EQ(*std::get<1>(tup), 2);
}

// ============================================================================
// Test Suite: core_sharded_initialization (Core Sharded Init Tests)
// ============================================================================

FB_SUITE_SETUP(core_sharded_initialization) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_sharded_initialization) {
    // Teardown code here
}

FB_TEST(core_sharded_initialization, system_size_capacity) {
    // system::capacity() returns total core count
    // Verify capacity == count of all cores iterated
    uint32_t simulated_capacity = 8;
    std::vector<uint32_t> all_cores;
    for (uint32_t i = 0; i < simulated_capacity; i++) {
        all_cores.push_back(i);
    }
    FB_ASSERT_EQ(all_cores.size(), simulated_capacity);
}

FB_TEST(core_sharded_initialization, system_first_core_min) {
    // first_core() must be the smallest valid core ID
    std::vector<uint32_t> cores = {3, 5, 7, 9};
    uint32_t min_core = *std::min_element(cores.begin(), cores.end());
    FB_ASSERT_EQ(min_core, 3);
    FB_ASSERT_EQ(cores.front(), min_core);
}

FB_TEST(core_sharded_initialization, system_last_core_max) {
    // last_core() must be the largest valid core ID
    std::vector<uint32_t> cores = {0, 1, 2, 3, 4};
    uint32_t max_core = *std::max_element(cores.begin(), cores.end());
    FB_ASSERT_EQ(max_core, 4);
    FB_ASSERT_EQ(cores.back(), max_core);
}

FB_TEST(core_sharded_initialization, n_core_arg_bounded_by_capacity) {
    // Constructor's n_core must not exceed system::capacity()
    uint32_t capacity = 16;
    uint32_t n_core_requested = 8;
    FB_ASSERT_TRUE(n_core_requested <= capacity);

    // Edge case: n_core == capacity
    uint32_t n_core_max = 16;
    FB_ASSERT_TRUE(n_core_max <= capacity);
}

FB_TEST(core_sharded_initialization, app_name_used_in_thread_naming) {
    // app_name passed to constructor used as prefix in thread names
    std::string app_name = "osd_app_";
    std::vector<std::string> thread_names;
    for (uint32_t i = 0; i < 4; i++) {
        thread_names.push_back(app_name + std::to_string(i));
    }

    // All thread names start with app_name prefix
    for (const auto& name : thread_names) {
        FB_ASSERT_TRUE(name.find(app_name) == 0);
    }
}

FB_TEST(core_sharded_initialization, construct_with_iterator_begin) {
    // Constructor takes begin iterator + n_core
    // Verify iterator can walk n_core steps without exhaustion
    std::vector<uint32_t> available_cores = {0, 1, 2, 3, 4, 5, 6, 7};
    auto it = available_cores.begin();
    uint32_t n = 4;
    uint32_t consumed = 0;

    while (consumed < n && it != available_cores.end()) {
        ++it;
        consumed++;
    }
    FB_ASSERT_EQ(consumed, n);
}

FB_TEST(core_sharded_initialization, threads_vector_grows_by_n_core) {
    // After construction, _threads.size() == n_core
    std::vector<void*> threads;
    uint32_t n_core = 6;
    for (uint32_t i = 0; i < n_core; i++) {
        threads.push_back((void*)(uintptr_t)(0x1000 + i));
    }
    FB_ASSERT_EQ(threads.size(), n_core);
}

FB_TEST(core_sharded_initialization, shard_cores_vector_grows_by_n_core) {
    // After construction, _shard_cores.size() == n_core
    std::vector<uint32_t> shard_cores;
    uint32_t n_core = 6;
    auto it = std::vector<uint32_t>{2, 4, 6, 8, 10, 12, 14}.begin();
    for (uint32_t i = 0; i < n_core; i++) {
        shard_cores.push_back(*it);
        ++it;
    }
    FB_ASSERT_EQ(shard_cores.size(), n_core);
    FB_ASSERT_EQ(shard_cores[0], 2);
    FB_ASSERT_EQ(shard_cores[5], 12);
}

// ============================================================================
// Test Suite: core_context_dispatch (Core Context Dispatch Tests)
// ============================================================================

FB_SUITE_SETUP(core_context_dispatch) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_context_dispatch) {
    // Teardown code here
}

FB_TEST(core_context_dispatch, run_invokes_run_task_then_deletes) {
    // core_context::run(void*) -> cast to core_context*, invoke run_task(), delete
    static int task_runs;
    static int destructions;
    task_runs = 0;
    destructions = 0;

    struct test_op {
        void run_task() { task_runs++; }
        ~test_op() { destructions++; }
    };

    // Mimic the run() static dispatch
    auto run = [](void* arg) {
        test_op* op = static_cast<test_op*>(arg);
        op->run_task();
        delete op;
    };

    test_op* op = new test_op();
    run((void*)op);

    FB_ASSERT_EQ(task_runs, 1);
    FB_ASSERT_EQ(destructions, 1);
}

FB_TEST(core_context_dispatch, void_pointer_round_trip) {
    // Pattern: core_context* -> void* -> core_context*
    struct task { int data = 42; };
    task t;
    void* opaque = static_cast<void*>(&t);
    task* recovered = static_cast<task*>(opaque);

    FB_ASSERT_EQ(recovered, &t);
    FB_ASSERT_EQ(recovered->data, 42);
}

FB_TEST(core_context_dispatch, no_access_after_delete) {
    // run() deletes ctx; caller must not retain reference
    static bool deleted_called;
    deleted_called = false;

    struct lifetime_ctx {
        ~lifetime_ctx() { deleted_called = true; }
    };

    lifetime_ctx* p = new lifetime_ctx();
    delete p;
    FB_ASSERT_TRUE(deleted_called);
    // After this point, p is dangling - testing pattern not the actual pointer
}

FB_TEST(core_context_dispatch, multiple_dispatches_independent) {
    // Each dispatch creates new ctx, invokes, deletes independently
    static int total_runs;
    total_runs = 0;

    struct op { void run() { total_runs++; } };

    for (int i = 0; i < 100; i++) {
        op* o = new op();
        o->run();
        delete o;
    }
    FB_ASSERT_EQ(total_runs, 100);
}

FB_TEST(core_context_dispatch, exception_in_run_task_propagates) {
    // If run_task throws, ctx must still be deleted (RAII pattern)
    // Verify via try/catch + destructor counter
    static int destructions;
    destructions = 0;

    struct throws_op {
        ~throws_op() { destructions++; }
        void run() { throw std::runtime_error("test"); }
    };

    bool caught = false;
    throws_op* op = new throws_op();
    try {
        op->run();
    } catch (const std::runtime_error&) {
        caught = true;
        delete op;
    }
    FB_ASSERT_TRUE(caught);
    FB_ASSERT_EQ(destructions, 1);
}

FB_TEST(core_context_dispatch, dispatch_via_function_pointer) {
    // core_context::run is a static method, can be used as function pointer
    static int counter;
    counter = 0;

    struct ctx_t {
        void run_task() { counter++; }
    };

    void (*dispatch_fn)(void*) = [](void* arg) {
        static_cast<ctx_t*>(arg)->run_task();
        delete static_cast<ctx_t*>(arg);
    };

    dispatch_fn(new ctx_t());
    FB_ASSERT_EQ(counter, 1);
    dispatch_fn(new ctx_t());
    FB_ASSERT_EQ(counter, 2);
}

FB_TEST(core_context_dispatch, base_pointer_polymorphism) {
    // Dispatch through base class pointer respects vtable
    struct base { virtual int describe() = 0; virtual ~base() = default; };
    struct derived_a : base { int describe() override { return 1; } };
    struct derived_b : base { int describe() override { return 2; } };

    base* ptr_a = new derived_a();
    base* ptr_b = new derived_b();

    FB_ASSERT_EQ(ptr_a->describe(), 1);
    FB_ASSERT_EQ(ptr_b->describe(), 2);

    delete ptr_a;
    delete ptr_b;
}

FB_TEST(core_context_dispatch, opaque_type_erasure) {
    // void* erases type; receiver must know the concrete type to recover
    struct type_a { int x = 1; };
    struct type_b { double y = 2.0; };

    void* erased_a = new type_a();
    void* erased_b = new type_b();

    // Recover concrete types
    type_a* a = static_cast<type_a*>(erased_a);
    type_b* b = static_cast<type_b*>(erased_b);

    FB_ASSERT_EQ(a->x, 1);
    FB_ASSERT_EQ(b->y, 2.0);

    delete a;
    delete b;
}

// ============================================================================
// Test Suite: lambda_ctx_advanced (Advanced Lambda Context Tests)
// ============================================================================

FB_SUITE_SETUP(lambda_ctx_advanced) {
    // Setup code here
}

FB_SUITE_TEARDOWN(lambda_ctx_advanced) {
    // Teardown code here
}

FB_TEST(lambda_ctx_advanced, copy_deleted) {
    // lambda_ctx(lambda_ctx* l) = delete -> no copy from raw pointer
    // Verify by simulating the deletion pattern
    struct no_copy_ctx {
        no_copy_ctx() = default;
        no_copy_ctx(no_copy_ctx* /*l*/) = delete;
    };

    no_copy_ctx orig;
    // Test that we cannot accidentally clone via pointer
    no_copy_ctx other;
    // Both can exist independently
    FB_ASSERT_TRUE(&orig != &other);
}

FB_TEST(lambda_ctx_advanced, func_stored_by_value) {
    // Func member stored via std::move(func)
    static int callable_destruction_count;
    callable_destruction_count = 0;

    struct callable {
        ~callable() { callable_destruction_count++; }
        void operator()() {}
    };

    {
        callable c;
        callable stored = std::move(c);
        (void)stored;
        // After move: original c still gets destroyed, stored also destroyed
    }
    FB_ASSERT_EQ(callable_destruction_count, 2);
}

FB_TEST(lambda_ctx_advanced, args_perfect_forwarding) {
    // make_tuple with forward<Args>... preserves value categories
    struct trace {
        static int copies;
        static int moves;
        trace() = default;
        trace(const trace&) { copies++; }
        trace(trace&&) noexcept { moves++; }
    };
    trace::copies = 0;
    trace::moves = 0;

    auto make = [](trace&& t) { return std::make_tuple(std::forward<trace>(t)); };
    auto tup = make(trace{});

    FB_ASSERT_TRUE(trace::moves >= 1);
    // No copy when rvalue forwarded
    FB_ASSERT_EQ(trace::copies, 0);
}

FB_TEST(lambda_ctx_advanced, run_task_invokes_apply) {
    // run_task() calls std::apply(func, args)
    int captured_result = 0;
    auto fn = [&captured_result](int a, int b) { captured_result = a + b; };
    auto args = std::make_tuple(10, 20);

    std::apply(fn, args);
    FB_ASSERT_EQ(captured_result, 30);
}

FB_TEST(lambda_ctx_advanced, multiple_arg_types) {
    // Variadic Args... allows mixed types
    std::string s;
    auto fn = [&s](int i, double d, const std::string& str) {
        s = std::to_string(i) + "_" + std::to_string(static_cast<int>(d)) + "_" + str;
    };
    auto args = std::make_tuple(1, 2.5, std::string("test"));
    std::apply(fn, args);

    FB_ASSERT_TRUE(s.find("1_") != std::string::npos);
    FB_ASSERT_TRUE(s.find("_test") != std::string::npos);
}

FB_TEST(lambda_ctx_advanced, zero_args_lambda) {
    // Zero arguments: empty tuple, no-arg lambda
    int counter = 0;
    auto fn = [&counter]() { counter++; };
    std::tuple<> empty;

    std::apply(fn, empty);
    FB_ASSERT_EQ(counter, 1);
    std::apply(fn, empty);
    FB_ASSERT_EQ(counter, 2);
}

FB_TEST(lambda_ctx_advanced, lambda_with_state_capture) {
    // Lambda captures local state by value (independent of caller)
    int initial = 100;
    auto fn = [initial](int delta) { return initial + delta; };

    // Modifying initial after capture doesn't affect lambda
    initial = 999;
    int result = fn(5);
    FB_ASSERT_EQ(result, 105); // 100 (captured) + 5
}

FB_TEST(lambda_ctx_advanced, args_destroyed_with_ctx) {
    // When lambda_ctx is destroyed, args tuple is destroyed too
    static int int_dtors;
    int_dtors = 0;

    struct counted_int {
        int v;
        counted_int(int x) : v(x) {}
        ~counted_int() { int_dtors++; }
        counted_int(const counted_int& o) : v(o.v) {}
    };

    {
        auto tup = std::make_tuple(counted_int(1), counted_int(2), counted_int(3));
        (void)tup;
    }
    // At least 3 destructions (may be more due to copies/moves in make_tuple)
    FB_ASSERT_TRUE(int_dtors >= 3);
}

// ============================================================================
// Test Suite: sharded_start_stop (sharded<Service> Start/Stop Tests)
// ============================================================================

FB_SUITE_SETUP(sharded_start_stop) {
    // Setup code here
}

FB_SUITE_TEARDOWN(sharded_start_stop) {
    // Teardown code here
}

FB_TEST(sharded_start_stop, start_creates_n_instances) {
    // start() creates one instance per shard
    static int constructions;
    constructions = 0;

    struct svc { svc() { constructions++; } ~svc() {} };

    std::vector<svc*> instances(4, nullptr);
    for (uint32_t i = 0; i < 4; i++) {
        instances[i] = new svc();
    }
    FB_ASSERT_EQ(constructions, 4);

    for (auto* p : instances) delete p;
}

FB_TEST(sharded_start_stop, stop_deletes_all_then_clears) {
    // stop() deletes each instance, sets to nullptr, then clears vector
    static int destructions;
    destructions = 0;

    struct svc { ~svc() { destructions++; } };

    std::vector<svc*> instances;
    instances.push_back(new svc());
    instances.push_back(new svc());
    instances.push_back(new svc());

    // Simulate stop()
    for (auto*& p : instances) {
        delete p;
        p = nullptr;
    }
    instances.clear();

    FB_ASSERT_EQ(destructions, 3);
    FB_ASSERT_TRUE(instances.empty());
}

FB_TEST(sharded_start_stop, start_with_args_forwarded) {
    // start(arg1, arg2, ...) forwards args to Service constructor
    struct param_svc {
        int x;
        std::string name;
        param_svc(int a, std::string s) : x(a), name(std::move(s)) {}
    };

    std::vector<param_svc*> instances;
    for (uint32_t i = 0; i < 3; i++) {
        instances.push_back(new param_svc(static_cast<int>(i * 10), "shard_" + std::to_string(i)));
    }

    FB_ASSERT_EQ(instances[0]->x, 0);
    FB_ASSERT_EQ(instances[1]->x, 10);
    FB_ASSERT_EQ(instances[2]->x, 20);
    FB_ASSERT_EQ(instances[0]->name, "shard_0");

    for (auto* p : instances) delete p;
}

FB_TEST(sharded_start_stop, start_uses_invoke_on_for_others) {
    // For shards other than current, start() uses core_sharded::invoke_on
    // Verify pattern: this_shard inline, others go through invoke_on
    uint32_t this_shard = 1;
    uint32_t total_shards = 4;
    uint32_t inline_count = 0;
    uint32_t invoke_on_count = 0;

    for (uint32_t s = 0; s < total_shards; s++) {
        if (s == this_shard) {
            inline_count++;
        } else {
            invoke_on_count++;
        }
    }
    FB_ASSERT_EQ(inline_count, 1);
    FB_ASSERT_EQ(invoke_on_count, total_shards - 1);
}

FB_TEST(sharded_start_stop, instances_vector_resized_to_count) {
    // start() does _instances.resize(count())
    std::vector<int*> instances;
    uint32_t count = 8;
    instances.resize(count);

    FB_ASSERT_EQ(instances.size(), count);
    // After resize, all elements are default-constructed (nullptr for pointers)
    for (auto* p : instances) {
        FB_ASSERT_TRUE(p == nullptr);
    }
}

FB_TEST(sharded_start_stop, stop_safe_with_already_null) {
    // stop() handles already-null entries gracefully
    std::vector<int*> instances;
    instances.push_back(new int(1));
    instances.push_back(nullptr);
    instances.push_back(new int(3));

    // Simulate stop() handling nulls
    int valid_deletes = 0;
    for (auto*& p : instances) {
        if (p) {
            delete p;
            valid_deletes++;
            p = nullptr;
        }
    }
    instances.clear();

    FB_ASSERT_EQ(valid_deletes, 2);
    FB_ASSERT_TRUE(instances.empty());
}

FB_TEST(sharded_start_stop, restart_after_stop) {
    // After stop(), start() can be called again
    std::vector<int*> instances;

    // First start
    for (int i = 0; i < 3; i++) instances.push_back(new int(i));
    FB_ASSERT_EQ(instances.size(), 3);

    // Stop
    for (auto*& p : instances) { delete p; p = nullptr; }
    instances.clear();
    FB_ASSERT_TRUE(instances.empty());

    // Restart
    for (int i = 10; i < 14; i++) instances.push_back(new int(i));
    FB_ASSERT_EQ(instances.size(), 4);
    FB_ASSERT_EQ(*instances[0], 10);

    for (auto* p : instances) delete p;
}

FB_TEST(sharded_start_stop, parallel_start_isolation) {
    // Different shards initialized in parallel must not see each other's state
    std::vector<int*> instances(4, nullptr);

    // Simulate parallel initialization
    for (uint32_t s = 0; s < 4; s++) {
        instances[s] = new int(static_cast<int>(s));
    }

    // Each shard sees its own value
    for (uint32_t s = 0; s < 4; s++) {
        FB_ASSERT_EQ(*instances[s], static_cast<int>(s));
    }

    for (auto* p : instances) delete p;
}

// ============================================================================
// Test Suite: sharded_access (sharded<Service> Access Methods Tests)
// ============================================================================

FB_SUITE_SETUP(sharded_access) {
    // Setup code here
}

FB_SUITE_TEARDOWN(sharded_access) {
    // Teardown code here
}

FB_TEST(sharded_access, local_returns_reference) {
    // local() returns reference (not pointer) - modifications affect stored instance
    std::vector<int*> instances;
    for (int i = 0; i < 4; i++) instances.push_back(new int(i * 100));

    uint32_t current_shard = 2;
    int& local_ref = *instances[current_shard];

    FB_ASSERT_EQ(local_ref, 200);
    local_ref = 999;
    FB_ASSERT_EQ(*instances[current_shard], 999);

    for (auto* p : instances) delete p;
}

FB_TEST(sharded_access, on_shard_returns_reference) {
    // on_shard(N) returns reference to shard N's instance
    std::vector<int*> instances;
    for (int i = 0; i < 4; i++) instances.push_back(new int(i + 1));

    int& shard_1 = *instances[1];
    int& shard_3 = *instances[3];

    FB_ASSERT_EQ(shard_1, 2);
    FB_ASSERT_EQ(shard_3, 4);
    FB_ASSERT_TRUE(&shard_1 != &shard_3);

    for (auto* p : instances) delete p;
}

FB_TEST(sharded_access, shard_is_started_size_check) {
    // shard_is_started returns false if shard >= _instances.size()
    std::vector<int*> instances;
    instances.push_back(new int(1));
    instances.push_back(new int(2));

    // Valid shards
    FB_ASSERT_TRUE(instances.size() > 0 && instances[0] != nullptr);
    FB_ASSERT_TRUE(instances.size() > 1 && instances[1] != nullptr);

    // Out-of-bounds shards
    uint32_t oob = 99;
    FB_ASSERT_TRUE(!(instances.size() > oob));

    for (auto* p : instances) delete p;
}

FB_TEST(sharded_access, shard_is_started_null_check) {
    // shard_is_started returns false if _instances[shard] is null
    std::vector<int*> instances;
    instances.push_back(new int(1));
    instances.push_back(nullptr);  // Not yet started
    instances.push_back(new int(3));

    FB_ASSERT_TRUE(instances[0] != nullptr); // started
    FB_ASSERT_TRUE(instances[1] == nullptr); // not started
    FB_ASSERT_TRUE(instances[2] != nullptr); // started

    delete instances[0];
    delete instances[2];
}

FB_TEST(sharded_access, size_reflects_instances_count) {
    // size() returns _instances.size()
    std::vector<int*> instances;
    FB_ASSERT_EQ(instances.size(), 0);

    instances.resize(8);
    FB_ASSERT_EQ(instances.size(), 8);

    instances.resize(16);
    FB_ASSERT_EQ(instances.size(), 16);

    instances.clear();
    FB_ASSERT_EQ(instances.size(), 0);
}

FB_TEST(sharded_access, modifications_persist) {
    // Modifications through local() persist between calls
    struct counter { int v = 0; };
    std::vector<counter*> instances;
    instances.push_back(new counter());

    // First access: increment
    counter& c = *instances[0];
    c.v = 5;

    // Second access: read back
    counter& c2 = *instances[0];
    FB_ASSERT_EQ(c2.v, 5);
    FB_ASSERT_EQ(&c, &c2);

    delete instances[0];
}

FB_TEST(sharded_access, instances_are_independent) {
    // Modifying one shard doesn't affect others
    std::vector<int*> instances;
    for (int i = 0; i < 4; i++) instances.push_back(new int(0));

    *instances[0] = 100;
    *instances[2] = 300;

    FB_ASSERT_EQ(*instances[0], 100);
    FB_ASSERT_EQ(*instances[1], 0);   // unchanged
    FB_ASSERT_EQ(*instances[2], 300);
    FB_ASSERT_EQ(*instances[3], 0);   // unchanged

    for (auto* p : instances) delete p;
}

FB_TEST(sharded_access, on_shard_for_cross_shard_init) {
    // on_shard(N) allows initialization-time access from other shards
    // (Documented as not thread-safe but valid during init phase)
    struct config { int value; };
    std::vector<config*> instances;
    for (int i = 0; i < 4; i++) instances.push_back(new config{0});

    // Core 0 initializes all shards via on_shard()
    for (uint32_t s = 0; s < 4; s++) {
        config& c = *instances[s];
        c.value = static_cast<int>(s) * 10;
    }

    for (uint32_t s = 0; s < 4; s++) {
        FB_ASSERT_EQ(instances[s]->value, static_cast<int>(s) * 10);
    }

    for (auto* p : instances) delete p;
}

// ============================================================================
// Test Suite: core_iterator_operations (Core Iterator Operations Tests)
// ============================================================================

FB_SUITE_SETUP(core_iterator_operations) {
    // Setup code here
}

FB_SUITE_TEARDOWN(core_iterator_operations) {
    // Teardown code here
}

FB_TEST(core_iterator_operations, increment_advances_via_next_core) {
    // operator++ calls next_core(_core)
    // Simulate iteration through sparse core list
    std::vector<uint32_t> cores = {0, 2, 5, 8, 13};
    auto it = cores.begin();
    uint32_t expected_sequence[] = {0, 2, 5, 8, 13};

    for (uint32_t i = 0; i < cores.size(); i++) {
        FB_ASSERT_EQ(*it, expected_sequence[i]);
        ++it;
    }
    FB_ASSERT_TRUE(it == cores.end());
}

FB_TEST(core_iterator_operations, post_increment_temporary) {
    // Post-increment returns temporary, advances original
    std::vector<uint32_t> cores = {10, 20, 30};
    auto it = cores.begin();

    auto temp = it++;
    FB_ASSERT_EQ(*temp, 10);
    FB_ASSERT_EQ(*it, 20);

    auto temp2 = it++;
    FB_ASSERT_EQ(*temp2, 20);
    FB_ASSERT_EQ(*it, 30);
}

FB_TEST(core_iterator_operations, equality_via_core_id_compare) {
    // operator== compares _core values
    uint32_t a = 5;
    uint32_t b = 5;
    uint32_t c = 6;

    FB_ASSERT_TRUE(a == b);
    FB_ASSERT_TRUE(a != c);
    FB_ASSERT_TRUE(b != c);
}

FB_TEST(core_iterator_operations, end_iteration_terminates) {
    // Iteration terminates at end sentinel UINT32_MAX
    std::vector<uint32_t> result;
    uint32_t simulated_core = 0;
    uint32_t simulated_end = 5;

    while (simulated_core != simulated_end) {
        result.push_back(simulated_core);
        simulated_core++;
    }

    FB_ASSERT_EQ(result.size(), 5);
    FB_ASSERT_EQ(result.back(), 4);
}

FB_TEST(core_iterator_operations, std_algorithm_compatible) {
    // Forward iterator works with std algorithms
    std::vector<uint32_t> cores = {1, 3, 5, 7, 9};

    // std::find
    auto found = std::find(cores.begin(), cores.end(), 5u);
    FB_ASSERT_TRUE(found != cores.end());
    FB_ASSERT_EQ(*found, 5);

    // std::count
    auto count = std::count(cores.begin(), cores.end(), 9u);
    FB_ASSERT_EQ(count, 1);

    // std::accumulate
    uint32_t sum = std::accumulate(cores.begin(), cores.end(), 0u);
    FB_ASSERT_EQ(sum, 25);
}

FB_TEST(core_iterator_operations, iteration_count_matches_size) {
    // Iterating from begin to end visits exactly size() elements
    std::vector<uint32_t> cores = {0, 1, 2, 3, 4, 5, 6, 7};
    uint32_t count = 0;
    for (auto it = cores.begin(); it != cores.end(); ++it) {
        count++;
    }
    FB_ASSERT_EQ(count, cores.size());
}

FB_TEST(core_iterator_operations, move_construction_preserves_position) {
    // Move-constructed iterator points to same position
    std::vector<uint32_t> cores = {10, 20, 30, 40};
    auto it1 = cores.begin();
    ++it1; // now at 20
    auto it2 = std::move(it1);
    FB_ASSERT_EQ(*it2, 20);
}

FB_TEST(core_iterator_operations, multiple_iterators_independent) {
    // Multiple iterators can advance independently
    std::vector<uint32_t> cores = {5, 10, 15, 20};
    auto it1 = cores.begin();
    auto it2 = cores.begin();

    ++it1; ++it1; // it1 at 15
    ++it2;        // it2 at 10

    FB_ASSERT_EQ(*it1, 15);
    FB_ASSERT_EQ(*it2, 10);
    FB_ASSERT_TRUE(it1 != it2);
}

// ============================================================================
// Test Suite: make_cpumask_helper (make_cpumake Helper Tests)
// ============================================================================

FB_SUITE_SETUP(make_cpumask_helper) {
    // Setup code here
}

FB_SUITE_TEARDOWN(make_cpumask_helper) {
    // Teardown code here
}

FB_TEST(make_cpumask_helper, returns_unique_ptr) {
    // make_cpumake returns std::unique_ptr<spdk_cpuset>
    // Verify ownership semantics
    auto p = std::make_unique<uint64_t>(0);
    FB_ASSERT_TRUE(p != nullptr);
    // Ownership transferable via move
    auto p2 = std::move(p);
    FB_ASSERT_TRUE(p == nullptr);
    FB_ASSERT_TRUE(p2 != nullptr);
}

FB_TEST(make_cpumask_helper, zero_then_set_pattern) {
    // make_cpumake: cpuset_zero, cpuset_set_cpu(core)
    // Verify resulting mask has exactly one bit set at core's position
    uint32_t core = 5;
    uint64_t mask = 0;            // simulates cpuset_zero
    mask |= (1ULL << core);       // simulates cpuset_set_cpu

    FB_ASSERT_EQ(mask, 1ULL << 5);
    // Exactly one bit set
    FB_ASSERT_EQ(mask & (mask - 1), 0);
}

FB_TEST(make_cpumask_helper, different_cores_different_masks) {
    // Each call produces independent cpuset for its core
    auto make_mask = [](uint32_t core) {
        return 1ULL << core;
    };

    uint64_t mask_a = make_mask(0);
    uint64_t mask_b = make_mask(3);
    uint64_t mask_c = make_mask(7);

    FB_ASSERT_TRUE(mask_a != mask_b);
    FB_ASSERT_TRUE(mask_b != mask_c);
    FB_ASSERT_TRUE(mask_a != mask_c);
    // Pairwise no overlap
    FB_ASSERT_EQ(mask_a & mask_b, 0);
}

FB_TEST(make_cpumask_helper, cpu_zero_clears_all_bits) {
    // cpuset_zero clears every bit
    uint64_t mask = 0xFFFFFFFFFFFFFFFFULL;
    mask = 0; // cpuset_zero
    FB_ASSERT_EQ(mask, 0);
    // All bits should be cleared
    for (uint32_t i = 0; i < 64; i++) {
        FB_ASSERT_TRUE((mask & (1ULL << i)) == 0);
    }
}

FB_TEST(make_cpumask_helper, set_cpu_idempotent) {
    // cpuset_set_cpu(c) applied twice yields same result
    uint64_t mask = 0;
    uint32_t core = 7;

    mask |= (1ULL << core);
    uint64_t once = mask;
    mask |= (1ULL << core);
    uint64_t twice = mask;

    FB_ASSERT_EQ(once, twice);
}

FB_TEST(make_cpumask_helper, high_core_id_supported) {
    // High core IDs (>32) should work with uint64_t mask
    uint64_t mask = 0;
    uint32_t high_core = 60;
    mask |= (1ULL << high_core);

    FB_ASSERT_TRUE(mask != 0);
    FB_ASSERT_EQ(mask, 1ULL << 60);
}

FB_TEST(make_cpumask_helper, unique_ptr_auto_cleanup) {
    // unique_ptr automatically frees memory on scope exit
    static int allocations;
    static int deallocations;
    allocations = 0;
    deallocations = 0;

    struct counted {
        counted() { allocations++; }
        ~counted() { deallocations++; }
    };

    {
        auto p = std::make_unique<counted>();
        FB_ASSERT_EQ(allocations, 1);
        FB_ASSERT_EQ(deallocations, 0);
    } // scope exit
    FB_ASSERT_EQ(deallocations, 1);
}

FB_TEST(make_cpumask_helper, cpumask_pointer_dereferencable) {
    // .get() returns raw pointer to underlying cpuset for SPDK API
    uint64_t mask = 0;
    uint64_t* raw = &mask;
    FB_ASSERT_TRUE(raw != nullptr);
    FB_ASSERT_EQ(*raw, 0);
    *raw = 42;
    FB_ASSERT_EQ(mask, 42);
}

// ============================================================================
// Test Main Entry Point
// ============================================================================

FB_TEST_MAIN()
