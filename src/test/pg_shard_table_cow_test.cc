/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 *
 * Regression tests for pg→shard COW publish semantics (PR-4).
 */

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

struct shard_revision {
    uint32_t _shard;
    int64_t _revision;
};

using pg_shard_table = std::map<std::string, shard_revision>;

static int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define EXPECT_EQ(a, b)                                                        \
    do {                                                                       \
        const auto _a = (a);                                                   \
        const auto _b = (b);                                                   \
        if (!(_a == _b)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d\n", __FILE__, __LINE__);          \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Minimal COW publisher used by partition_manager::mutate_shard_table
class cow_table {
public:
    cow_table() : _ro(std::make_shared<const pg_shard_table>()) {}

    bool get(const std::string& name, uint32_t& shard) const {
        auto t = _ro.load(std::memory_order_acquire);
        auto it = t->find(name);
        if (it == t->end()) {
            return false;
        }
        shard = it->second._shard;
        return true;
    }

    void add(const std::string& name, uint32_t shard, int64_t rev) {
        auto cur = _ro.load(std::memory_order_acquire);
        auto next = std::make_shared<pg_shard_table>(*cur);
        (*next)[name] = shard_revision{shard, rev};
        _ro.store(std::shared_ptr<const pg_shard_table>(std::move(next)),
                  std::memory_order_release);
    }

    bool remove(const std::string& name) {
        auto cur = _ro.load(std::memory_order_acquire);
        auto next = std::make_shared<pg_shard_table>(*cur);
        const bool erased = next->erase(name) > 0;
        _ro.store(std::shared_ptr<const pg_shard_table>(std::move(next)),
                  std::memory_order_release);
        return erased;
    }

    // Reader holds old snapshot while writer publishes — no UAF.
    std::shared_ptr<const pg_shard_table> snapshot() const {
        return _ro.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::shared_ptr<const pg_shard_table>> _ro;
};

static void test_add_get_remove() {
    cow_table t;
    uint32_t shard = 99;
    EXPECT_TRUE(!t.get("1.1", shard));
    t.add("1.1", 2, 10);
    EXPECT_TRUE(t.get("1.1", shard));
    EXPECT_EQ(shard, 2u);
    EXPECT_TRUE(t.remove("1.1"));
    EXPECT_TRUE(!t.get("1.1", shard));
    EXPECT_TRUE(!t.remove("1.1"));
}

static void test_reader_holds_old_snapshot() {
    cow_table t;
    t.add("1.2", 0, 1);
    auto snap = t.snapshot();
    t.add("1.2", 3, 2); // overwrite home
    // Old snapshot still sees previous value
    EXPECT_EQ(snap->at("1.2")._shard, 0u);
    uint32_t shard = 0;
    EXPECT_TRUE(t.get("1.2", shard));
    EXPECT_EQ(shard, 3u);
}

int main() {
    test_add_get_remove();
    test_reader_holds_old_snapshot();

    if (g_failures != 0) {
        std::fprintf(stderr, "pg_shard_table_cow_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("pg_shard_table_cow_test: all passed\n");
    return 0;
}
