/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 *
 * Regression tests for raft/localstore contracts that must stay valid across
 * run-to-complete lifecycle changes. No SPDK app runtime is required.
 */

#include <cstdio>
#include <string>
#include <type_traits>

#include "fastblock/utils/utils.h"
#include "localstore/spdk_buffer.h"
#include "raft/raft_cache.h"
#include "raft/raft_log.h"

static int g_failures = 0;

#define EXPECT_TRUE(cond)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b)                                                         \
    do {                                                                        \
        const auto _a = (a);                                                    \
        const auto _b = (b);                                                    \
        if (!(_a == _b)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d\n", __FILE__, __LINE__);           \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

class counting_context : public utils::context {
public:
    explicit counting_context(int* count)
      : utils::context(false), _count(count) {}

private:
    void finish(int) override {}

    void finish_del(int) override {
        ++(*_count);
    }

    int* _count;
};

class reentrant_remove_context : public utils::context {
public:
    reentrant_remove_context(entry_cache* cache, raft_index_t idx, int* count)
      : utils::context(false), _cache(cache), _idx(idx), _count(count) {}

private:
    void finish(int) override {}

    void finish_del(int) override {
        ++(*_count);
        _cache->remove_entry_between(_idx, _idx);
    }

    entry_cache* _cache;
    raft_index_t _idx;
    int* _count;
};

static std::shared_ptr<raft_entry_t> make_entry(raft_index_t idx, raft_term_t term) {
    auto entry = std::make_shared<raft_entry_t>();
    entry->set_idx(idx);
    entry->set_term(term);
    entry->set_type(0);
    return entry;
}

static void test_raft_log_null_disk_contract() {
    raft_log log(nullptr);
    raft_term_t term = 77;

    EXPECT_FALSE(log.disk_log_available());
    EXPECT_FALSE(log.disk_get_term(1, term));
    EXPECT_EQ(term, 77L);
}

static void test_entry_cache_reentrant_current_remove() {
    entry_cache cache;
    int completed = 0;
    reentrant_remove_context c1(&cache, 1, &completed);
    counting_context c2(&completed);
    counting_context c3(&completed);

    cache.add(make_entry(1, 10), &c1);
    cache.add(make_entry(2, 10), &c2);
    cache.add(make_entry(3, 11), &c3);

    cache.complete_entry_between(1, 3, 0);

    EXPECT_EQ(completed, 3);
    EXPECT_EQ(cache.count(), 2u);
    EXPECT_TRUE(cache.get(1) == nullptr);
    EXPECT_TRUE(cache.get(2) != nullptr);
    EXPECT_TRUE(cache.get(3) != nullptr);
}

static void test_entry_cache_completion_is_one_shot() {
    entry_cache cache;
    int completed = 0;
    counting_context c1(&completed);
    counting_context c2(&completed);

    cache.add(make_entry(4, 12), &c1);
    cache.add(make_entry(5, 12), &c2);

    cache.complete_entry_between(4, 5, 0);
    cache.complete_entry_between(4, 5, 0);

    EXPECT_EQ(completed, 2);
    EXPECT_EQ(cache.count(), 2u);
}

static void test_entry_cache_range_removal() {
    entry_cache cache;
    int completed = 0;
    counting_context c1(&completed);
    counting_context c2(&completed);
    counting_context c3(&completed);
    counting_context c4(&completed);

    cache.add(make_entry(10, 1), &c1);
    cache.add(make_entry(11, 1), &c2);
    cache.add(make_entry(12, 2), &c3);
    cache.add(make_entry(13, 2), &c4);

    cache.remove_entry_between(11, 12);

    EXPECT_EQ(cache.count(), 2u);
    EXPECT_TRUE(cache.get(10) != nullptr);
    EXPECT_TRUE(cache.get(11) == nullptr);
    EXPECT_TRUE(cache.get(12) == nullptr);
    EXPECT_TRUE(cache.get(13) != nullptr);
    EXPECT_EQ(cache.get_last_cache_entry(), 13L);
}

static void test_buffer_list_move_only_contract() {
    static_assert(!std::is_copy_constructible_v<buffer_list>);
    static_assert(!std::is_copy_assignable_v<buffer_list>);
    static_assert(std::is_move_constructible_v<buffer_list>);
    static_assert(std::is_move_assignable_v<buffer_list>);

    char one[16]{};
    char two[32]{};
    char three[8]{};

    buffer_list a;
    a.append_buffer(spdk_buffer(one, sizeof(one)));
    a.append_buffer(spdk_buffer(two, sizeof(two)));
    EXPECT_EQ(a.bytes(), 48u);

    buffer_list b(std::move(a));
    EXPECT_EQ(a.bytes(), 0u);
    EXPECT_EQ(b.bytes(), 48u);

    buffer_list c;
    c.append_buffer(spdk_buffer(three, sizeof(three)));
    c = std::move(b);
    EXPECT_EQ(b.bytes(), 0u);
    EXPECT_EQ(c.bytes(), 48u);

    buffer_list d;
    d.append_buffer(std::move(c));
    EXPECT_EQ(c.bytes(), 0u);
    EXPECT_EQ(d.bytes(), 48u);
}

int main() {
    test_raft_log_null_disk_contract();
    test_entry_cache_reentrant_current_remove();
    test_entry_cache_completion_is_one_shot();
    test_entry_cache_range_removal();
    test_buffer_list_move_only_contract();

    if (g_failures != 0) {
        std::fprintf(stderr, "raft_contract_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("raft_contract_test: all passed\n");
    return 0;
}
