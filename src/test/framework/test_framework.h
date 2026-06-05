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
 * @file test_framework.h
 * @brief Unified test framework for fastblock project
 *
 * This framework provides a unified testing infrastructure similar to aicukvs
 * testing framework, including:
 * - Test case registration and management
 * - Assertion macros with detailed error reporting
 * - Test result collection and reporting
 * - Test configuration management
 * - Support for synchronous and asynchronous tests
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <fstream>
#include <atomic>
#include <random>
#include <initializer_list>
#include <optional>
#include <regex>
#include <stdexcept>
#include <cstring>

#ifdef __linux__
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "spdk/stdinc.h"
#include "spdk/log.h"

namespace fastblock {
namespace test {

/**
 * @brief Test result status
 */
enum class test_status {
    PASSED,
    FAILED,
    SKIPPED,
    PENDING,
    RUNNING
};

/**
 * @brief Convert test_status to string
 */
inline const char* test_status_str(test_status status) {
    switch (status) {
        case test_status::PASSED:   return "PASSED";
        case test_status::FAILED:   return "FAILED";
        case test_status::SKIPPED:  return "SKIPPED";
        case test_status::PENDING:  return "PENDING";
        case test_status::RUNNING:  return "RUNNING";
        default:                    return "UNKNOWN";
    }
}

/**
 * @brief Test severity level
 */
enum class test_severity {
    CRITICAL,    // Must pass, failure stops entire test suite
    NORMAL,      // Normal test, failure recorded but continues
    OPTIONAL     // Optional test, failure only logged
};

/**
 * @brief Single test result record
 */
struct test_result {
    std::string test_name;
    std::string suite_name;
    test_status status;
    std::string message;
    std::string file;
    int line;
    std::chrono::microseconds duration;
    test_severity severity;

    test_result(const std::string& name, const std::string& suite,
                test_status s, const std::string& msg = "",
                const std::string& f = "", int l = 0)
        : test_name(name), suite_name(suite), status(s), message(msg),
          file(f), line(l), duration(0), severity(test_severity::NORMAL) {}
};

/**
 * @brief Test case definition
 */
class test_case {
public:
    using test_func = std::function<void(class test_context&)>;

    test_case(const std::string& name, const std::string& suite,
              test_func func, test_severity sev = test_severity::NORMAL)
        : _name(name), _suite(suite), _func(func), _severity(sev),
          _status(test_status::PENDING) {}

    const std::string& name() const { return _name; }
    const std::string& suite() const { return _suite; }
    test_severity severity() const { return _severity; }
    test_status status() const { return _status; }

    void set_description(const std::string& desc) { _description = desc; }
    void set_timeout(uint32_t seconds) { _timeout_seconds = seconds; }

    /**
     * @brief Execute the test case
     * @param ctx Test context for assertions and logging
     * @return Test result
     */
    test_result execute(class test_context& ctx);

private:
    std::string _name;
    std::string _suite;
    std::string _description;
    test_func _func;
    test_severity _severity;
    test_status _status;
    uint32_t _timeout_seconds = 300;  // Default 5 minutes
};

/**
 * @brief Test context for tracking test execution state
 */
class test_context {
public:
    test_context(test_case& tc) : _test_case(tc), _failed(false), _skipped(false) {}

    /**
     * @brief Record a failure
     */
    void fail(const std::string& message, const std::string& file, int line) {
        _failed = true;
        _fail_message = message;
        _fail_file = file;
        _fail_line = line;
        SPDK_ERRLOG("TEST FAILED: %s at %s:%d - %s\n",
                    _test_case.name().c_str(), file.c_str(), line, message.c_str());
    }

    /**
     * @brief Skip the test with reason
     */
    void skip(const std::string& reason) {
        _skipped = true;
        _skip_reason = reason;
        SPDK_NOTICELOG("TEST SKIPPED: %s - %s\n",
                       _test_case.name().c_str(), reason.c_str());
    }

    /**
     * @brief Log informational message
     */
    void log_info(const std::string& message) {
        SPDK_NOTICELOG("[TEST %s] %s\n", _test_case.name().c_str(), message.c_str());
    }

    /**
     * @brief Log debug message
     */
    void log_debug(const std::string& message) {
        SPDK_DEBUGLOG(test, "[TEST %s] %s\n", _test_case.name().c_str(), message.c_str());
    }

    bool failed() const { return _failed; }
    bool skipped() const { return _skipped; }
    const std::string& fail_message() const { return _fail_message; }
    const std::string& fail_file() const { return _fail_file; }
    int fail_line() const { return _fail_line; }
    const std::string& skip_reason() const { return _skip_reason; }

private:
    test_case& _test_case;
    bool _failed;
    bool _skipped;
    std::string _fail_message;
    std::string _fail_file;
    int _fail_line;
    std::string _skip_reason;
};

/**
 * @brief Test suite containing multiple test cases
 */
class test_suite {
public:
    test_suite(const std::string& name) : _name(name) {}

    const std::string& name() const { return _name; }

    void add_test(std::shared_ptr<test_case> tc) {
        _tests.push_back(tc);
    }

    const std::vector<std::shared_ptr<test_case>>& tests() const { return _tests; }

    /**
     * @brief Set setup/teardown functions
     */
    void set_setup(std::function<void()> setup) { _setup = setup; }
    void set_teardown(std::function<void()> teardown) { _teardown = teardown; }

    void run_setup() { if (_setup) _setup(); }
    void run_teardown() { if (_teardown) _teardown(); }

private:
    std::string _name;
    std::vector<std::shared_ptr<test_case>> _tests;
    std::function<void()> _setup;
    std::function<void()> _teardown;
};

/**
 * @brief Test registry - singleton for managing all test suites
 *
 * Thread-safe: Uses mutex to protect concurrent access to test suites.
 */
class test_registry {
public:
    static test_registry& instance() {
        static test_registry registry;
        return registry;
    }

    /**
     * @brief Register a test case
     */
    void register_test(const std::string& suite_name,
                       std::shared_ptr<test_case> tc) {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& suite = get_or_create_suite_unlocked(suite_name);
        suite.add_test(tc);
    }

    /**
     * @brief Get or create a test suite
     */
    test_suite& get_or_create_suite(const std::string& name) {
        std::lock_guard<std::mutex> lock(_mutex);
        return get_or_create_suite_unlocked(name);
    }

    /**
     * @brief Get all test suites (thread-safe copy)
     */
    std::vector<std::shared_ptr<test_suite>> suites() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _suites;
    }

    /**
     * @brief Clear all registered tests
     */
    void clear() {
        std::lock_guard<std::mutex> lock(_mutex);
        _suites.clear();
    }

private:
    test_registry() = default;

    // Internal unlocked version for use within locked methods
    test_suite& get_or_create_suite_unlocked(const std::string& name) {
        for (auto& s : _suites) {
            if (s->name() == name) {
                return *s;
            }
        }
        auto suite = std::make_shared<test_suite>(name);
        _suites.push_back(suite);
        return *suite;
    }

    std::vector<std::shared_ptr<test_suite>> _suites;
    mutable std::mutex _mutex;
};

/**
 * @brief Test runner - executes all registered tests
 */
class test_runner {
public:
    struct summary {
        int total = 0;
        int passed = 0;
        int failed = 0;
        int skipped = 0;
        std::chrono::microseconds total_duration{0};
    };

    /**
     * @brief Run all registered tests
     */
    summary run_all();

    /**
     * @brief Run specific test suite
     */
    summary run_suite(const std::string& suite_name);

    /**
     * @brief Run tests matching pattern
     */
    summary run_matching(const std::string& pattern);

    /**
     * @brief Print test results
     */
    void print_results() const;

    /**
     * @brief Get test results
     */
    const std::vector<test_result>& results() const { return _results; }

private:
    std::vector<test_result> _results;
};

/**
 * @brief Auto-registration helper for test cases
 */
class test_registrar {
public:
    test_registrar(const std::string& suite_name,
                   const std::string& test_name,
                   test_case::test_func func,
                   test_severity severity = test_severity::NORMAL) {
        auto tc = std::make_shared<test_case>(test_name, suite_name, func, severity);
        test_registry::instance().register_test(suite_name, tc);
    }
};

/**
 * @brief Suite setup registrar
 */
class suite_setup_registrar {
public:
    suite_setup_registrar(const std::string& suite_name, std::function<void()> setup) {
        auto& suite = test_registry::instance().get_or_create_suite(suite_name);
        suite.set_setup(setup);
    }
};

/**
 * @brief Suite teardown registrar
 */
class suite_teardown_registrar {
public:
    suite_teardown_registrar(const std::string& suite_name, std::function<void()> teardown) {
        auto& suite = test_registry::instance().get_or_create_suite(suite_name);
        suite.set_teardown(teardown);
    }
};

// Convenience macros for test definition

/**
 * @brief Define a test case
 * Usage: TEST(suite_name, test_name) { ... }
 */
#define FB_TEST(suite, name)                                                    \
    void fb_test_##suite##_##name(::fastblock::test::test_context& ctx);        \
    static ::fastblock::test::test_registrar                                    \
        fb_registrar_##suite##_##name(#suite, #name,                           \
                                      fb_test_##suite##_##name);                \
    void fb_test_##suite##_##name(::fastblock::test::test_context& ctx)

/**
 * @brief Define a critical test case (failure stops suite)
 */
#define FB_TEST_CRITICAL(suite, name)                                           \
    void fb_test_##suite##_##name(::fastblock::test::test_context& ctx);        \
    static ::fastblock::test::test_registrar                                    \
        fb_registrar_##suite##_##name(#suite, #name,                           \
                                      fb_test_##suite##_##name,                 \
                                      ::fastblock::test::test_severity::CRITICAL); \
    void fb_test_##suite##_##name(::fastblock::test::test_context& ctx)

/**
 * @brief Define suite setup function
 */
#define FB_SUITE_SETUP(suite)                                                   \
    void fb_suite_setup_##suite();                                              \
    static ::fastblock::test::suite_setup_registrar                            \
        fb_suite_setup_reg_##suite(#suite, fb_suite_setup_##suite);             \
    void fb_suite_setup_##suite()

/**
 * @brief Define suite teardown function
 */
#define FB_SUITE_TEARDOWN(suite)                                                \
    void fb_suite_teardown_##suite();                                           \
    static ::fastblock::test::suite_teardown_registrar                         \
        fb_suite_teardown_reg_##suite(#suite, fb_suite_teardown_##suite);       \
    void fb_suite_teardown_##suite()

/**
 * @brief Assertion macros
 */
#define FB_ASSERT_TRUE(condition)                                              \
    do {                                                                        \
        if (!(condition)) {                                                    \
            ctx.fail("Assertion failed: " #condition, __FILE__, __LINE__);     \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_FALSE(condition)                                             \
    do {                                                                        \
        if (condition) {                                                       \
            ctx.fail("Assertion failed: NOT(" #condition ")", __FILE__, __LINE__); \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_EQ(expected, actual)                                         \
    do {                                                                        \
        if (!((expected) == (actual))) {                                       \
            std::stringstream ss;                                               \
            ss << "Assertion failed: " << #expected << " == " << #actual       \
               << " (expected: " << (expected) << ", actual: " << (actual) << ")"; \
            ctx.fail(ss.str(), __FILE__, __LINE__);                            \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_NE(expected, actual)                                         \
    do {                                                                        \
        if ((expected) == (actual)) {                                          \
            std::stringstream ss;                                               \
            ss << "Assertion failed: " << #expected << " != " << #actual       \
               << " (both are: " << (expected) << ")";                          \
            ctx.fail(ss.str(), __FILE__, __LINE__);                            \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_LT(a, b)                                                     \
    do {                                                                        \
        if (!((a) < (b))) {                                                    \
            std::stringstream ss;                                               \
            ss << "Assertion failed: " << #a << " < " << #b                    \
               << " (" << (a) << " is not less than " << (b) << ")";            \
            ctx.fail(ss.str(), __FILE__, __LINE__);                            \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_GT(a, b)                                                     \
    do {                                                                        \
        if (!((a) > (b))) {                                                    \
            std::stringstream ss;                                               \
            ss << "Assertion failed: " << #a << " > " << #b                    \
               << " (" << (a) << " is not greater than " << (b) << ")";         \
            ctx.fail(ss.str(), __FILE__, __LINE__);                            \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_LE(a, b)                                                     \
    do {                                                                        \
        if (!((a) <= (b))) {                                                   \
            std::stringstream ss;                                               \
            ss << "Assertion failed: " << #a << " <= " << #b                   \
               << " (" << (a) << " is greater than " << (b) << ")";             \
            ctx.fail(ss.str(), __FILE__, __LINE__);                            \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_GE(a, b)                                                     \
    do {                                                                        \
        if (!((a) >= (b))) {                                                   \
            std::stringstream ss;                                               \
            ss << "Assertion failed: " << #a << " >= " << #b                   \
               << " (" << (a) << " is less than " << (b) << ")";               \
            ctx.fail(ss.str(), __FILE__, __LINE__);                            \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_NULL(ptr)                                                    \
    do {                                                                        \
        if ((ptr) != nullptr) {                                                \
            ctx.fail("Assertion failed: " #ptr " is not null", __FILE__, __LINE__); \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_NOT_NULL(ptr)                                                \
    do {                                                                        \
        if ((ptr) == nullptr) {                                                \
            ctx.fail("Assertion failed: " #ptr " is null", __FILE__, __LINE__); \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_ASSERT_STR_EQ(expected, actual)                                     \
    do {                                                                        \
        std::string e = (expected);                                            \
        std::string a = (actual);                                              \
        if (e != a) {                                                          \
            std::stringstream ss;                                               \
            ss << "String assertion failed: expected \"" << e << "\""           \
               << ", actual \"" << a << "\"";                                   \
            ctx.fail(ss.str(), __FILE__, __LINE__);                            \
            return;                                                             \
        }                                                                       \
    } while (0)

#define FB_SKIP(reason)                                                        \
    do {                                                                        \
        ctx.skip(reason);                                                      \
        return;                                                                 \
    } while (0)

#define FB_LOG_INFO(msg) ctx.log_info(msg)
#define FB_LOG_DEBUG(msg) ctx.log_debug(msg)

// ============================================================================
// Parameterized Tests (FB_TEST_P)
// ============================================================================

/**
 * @brief Parameterized test case template
 */
template<typename ParamType>
class parameterized_test_case : public test_case {
public:
    using param_func = std::function<void(test_context&, const ParamType&)>;

    parameterized_test_case(const std::string& name, const std::string& suite,
                            param_func func, const ParamType& param)
        : test_case(name, suite, [this, func](test_context& ctx) {
            func(ctx, _param);
        }), _param(param) {}

private:
    ParamType _param;
};

/**
 * @brief Registrar for parameterized tests
 */
template<typename ParamType>
class parameterized_test_registrar {
public:
    template<typename Container>
    parameterized_test_registrar(const std::string& suite_name,
                                  const std::string& test_name,
                                  typename parameterized_test_case<ParamType>::param_func func,
                                  const Container& params) {
        int idx = 0;
        for (const auto& param : params) {
            std::string name = test_name + "/" + std::to_string(idx);
            auto tc = std::make_shared<parameterized_test_case<ParamType>>(
                name, suite_name, func, param);
            test_registry::instance().register_test(suite_name, tc);
            idx++;
        }
    }
};

/**
 * @brief Define a parameterized test
 * Usage:
 *   FB_TEST_P(raft_state, term_comparison, term_pair) {
 *       FB_ASSERT_TRUE(term_pair.first < term_pair.second);
 *   }
 *   FB_INSTANTIATE_TEST_SUITE_P(raft_state, term_comparison,
 *       std::vector<std::pair<int,int>>{{1,2}, {2,3}, {100,200}});
 */
#define FB_TEST_P(suite, name, param_name)                                         \
    void fb_test_p_##suite##_##name(::fastblock::test::test_context& ctx,         \
                                     const auto& param_name)

#define FB_INSTANTIATE_TEST_SUITE_P(suite, name, values)                           \
    static ::fastblock::test::parameterized_test_registrar<                       \
        typename std::decay<decltype(*(values).begin())>::type>                   \
        fb_param_reg_##suite##_##name(#suite, #name,                              \
                                       fb_test_p_##suite##_##name, values)

// ============================================================================
// Test Fixtures (FB_TEST_F)
// ============================================================================

/**
 * @brief Base class for test fixtures
 */
class test_fixture {
public:
    virtual ~test_fixture() = default;
    virtual void SetUp() {}
    virtual void TearDown() {}

    test_context* ctx = nullptr;
};

/**
 * @brief Fixture test case
 */
template<typename Fixture>
class fixture_test_case : public test_case {
public:
    using test_func = std::function<void(Fixture&)>;

    fixture_test_case(const std::string& name, const std::string& suite,
                      test_func func)
        : test_case(name, suite, [this, func](test_context& ctx) {
            Fixture fixture;
            fixture.ctx = &ctx;
            fixture.SetUp();
            try {
                func(fixture);
            } catch (...) {
                fixture.TearDown();
                throw;
            }
            fixture.TearDown();
        }) {}
};

/**
 * @brief Registrar for fixture tests
 */
template<typename Fixture>
class fixture_test_registrar {
public:
    fixture_test_registrar(const std::string& suite_name,
                           const std::string& test_name,
                           typename fixture_test_case<Fixture>::test_func func) {
        auto tc = std::make_shared<fixture_test_case<Fixture>>(
            test_name, suite_name, func);
        test_registry::instance().register_test(suite_name, tc);
    }
};

/**
 * @brief Define a fixture test
 * Usage:
 *   class RaftStateFixture : public fastblock::test::test_fixture {
 *   protected:
 *       raft_identity state = RAFT_STATE_FOLLOWER;
 *       void SetUp() override { state = RAFT_STATE_FOLLOWER; }
 *   };
 *   FB_TEST_F(RaftStateFixture, test_example) {
 *       FB_ASSERT_EQ(state, RAFT_STATE_FOLLOWER);
 *   }
 */
#define FB_TEST_F(fixture, name)                                                   \
    void fb_test_f_##fixture##_##name(fixture& fb_fixture_);                       \
    static ::fastblock::test::fixture_test_registrar<fixture>                      \
        fb_fixture_reg_##fixture##_##name(#fixture, #name,                        \
                                          fb_test_f_##fixture##_##name);           \
    void fb_test_f_##fixture##_##name(fixture& fb_fixture_)

// ============================================================================
// Async Test Support (FB_TEST_ASYNC)
// ============================================================================

/**
 * @brief Async test context for tracking async operations
 */
class async_test_context {
public:
    bool completed = false;
    bool timed_out = false;
    std::string error_message;
    std::condition_variable cv;
    std::mutex mutex;
};

/**
 * @brief Async test case
 */
class async_test_case : public test_case {
public:
    using async_func = std::function<void(test_context&, async_test_context&)>;

    async_test_case(const std::string& name, const std::string& suite,
                    async_func func, uint32_t timeout_ms = 5000)
        : test_case(name, suite, [this, func, timeout_ms](test_context& ctx) {
            async_test_context async_ctx;

            // Use std::async instead of std::thread::detach() for proper resource management
            auto future = std::async(std::launch::async, [&func, &ctx, &async_ctx]() {
                func(ctx, async_ctx);
            });

            std::unique_lock<std::mutex> lock(async_ctx.mutex);
            bool completed = async_ctx.cv.wait_for(lock,
                    std::chrono::milliseconds(timeout_ms),
                    [&async_ctx] { return async_ctx.completed; });

            if (!completed) {
                async_ctx.timed_out = true;
                ctx.fail("Async test timed out", __FILE__, __LINE__);
                // Note: We cannot forcefully cancel the async thread, but we can
                // signal it to stop by setting timed_out flag. The test function
                // should check async_ctx.timed_out and exit early if needed.
            }
            if (!async_ctx.error_message.empty()) {
                ctx.fail(async_ctx.error_message, __FILE__, __LINE__);
            }

            // Wait for the async thread to complete to ensure proper cleanup
            // Use a short timeout to avoid hanging indefinitely
            lock.unlock();
            future.wait_for(std::chrono::milliseconds(100));
        }) {}
};

/**
 * @brief Registrar for async tests
 */
class async_test_registrar {
public:
    async_test_registrar(const std::string& suite_name,
                         const std::string& test_name,
                         async_test_case::async_func func,
                         uint32_t timeout_ms = 5000) {
        auto tc = std::make_shared<async_test_case>(test_name, suite_name,
                                                     func, timeout_ms);
        test_registry::instance().register_test(suite_name, tc);
    }
};

/**
 * @brief Define an async test
 * Usage:
 *   FB_TEST_ASYNC(raft_state, election_timeout_trigger) {
 *       start_election_timer();
 *       FB_WAIT_FOR(state == RAFT_STATE_CANDIDATE, async_ctx, 1000);
 *       FB_ASYNC_COMPLETE(async_ctx);
 *   }
 */
#define FB_TEST_ASYNC(suite, name)                                                 \
    void fb_test_async_##suite##_##name(::fastblock::test::test_context& ctx,    \
                                         ::fastblock::test::async_test_context& fb_async_ctx_); \
    static ::fastblock::test::async_test_registrar                                 \
        fb_async_reg_##suite##_##name(#suite, #name,                              \
                                       fb_test_async_##suite##_##name);            \
    void fb_test_async_##suite##_##name(::fastblock::test::test_context& ctx,     \
                                         ::fastblock::test::async_test_context& fb_async_ctx_)

#define FB_ASYNC_ASSERT_TRUE(condition, async_ctx)                                 \
    do {                                                                            \
        if (!(condition)) {                                                        \
            std::lock_guard<std::mutex> lock((async_ctx).mutex);                  \
            (async_ctx).error_message = "Async assertion failed: " #condition;    \
            (async_ctx).cv.notify_all();                                           \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_WAIT_FOR(condition, async_ctx, timeout_ms)                              \
    do {                                                                            \
        std::unique_lock<std::mutex> lock((async_ctx).mutex);                      \
        if (!(async_ctx).cv.wait_for(lock,                                         \
                std::chrono::milliseconds(timeout_ms),                             \
                [&] { return (condition); })) {                                   \
        } else {                                                                    \
            (async_ctx).timed_out = true;                                          \
            (async_ctx).error_message = "Wait timed out: " #condition;             \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASYNC_COMPLETE(async_ctx)                                               \
    do {                                                                            \
        std::lock_guard<std::mutex> lock((async_ctx).mutex);                      \
        (async_ctx).completed = true;                                              \
        (async_ctx).cv.notify_all();                                               \
    } while (0)

// ============================================================================
// Death Tests (Process Crash Detection)
// ============================================================================

/**
 * @brief Death test case - runs in a forked process
 */
class death_test_case : public test_case {
public:
    using death_func = std::function<void()>;

    death_test_case(const std::string& name, const std::string& suite,
                    death_func func, int expected_exit_code = -1,
                    const std::string& expected_message = "")
        : test_case(name, suite, [this, func, expected_exit_code, expected_message]
                    (test_context& ctx) {
#ifdef __linux__
            pid_t pid = fork();
            if (pid == 0) {
                // Child process
                func();
                _exit(0);
            } else if (pid > 0) {
                // Parent process
                int status;
                waitpid(pid, &status, 0);

                if (WIFSIGNALED(status)) {
                    // Process was killed by signal
                    if (expected_exit_code == -1 ||
                        WTERMSIG(status) == expected_exit_code) {
                        // Expected death
                        return;
                    }
                } else if (WIFEXITED(status)) {
                    int exit_code = WEXITSTATUS(status);
                    if (exit_code == expected_exit_code) {
                        return;
                    }
                    ctx.fail("Process exited with unexpected code: " +
                             std::to_string(exit_code), __FILE__, __LINE__);
                    return;
                }
                ctx.fail("Process did not die as expected", __FILE__, __LINE__);
            }
#else
            // Non-Linux: skip death tests
            ctx.skip("Death tests not supported on this platform");
#endif
        }) {}
};

/**
 * @brief Registrar for death tests
 */
class death_test_registrar {
public:
    death_test_registrar(const std::string& suite_name,
                         const std::string& test_name,
                         death_test_case::death_func func,
                         int expected_exit_code = -1,
                         const std::string& expected_message = "") {
        auto tc = std::make_shared<death_test_case>(test_name, suite_name,
                                                     func, expected_exit_code,
                                                     expected_message);
        test_registry::instance().register_test(suite_name, tc);
    }
};

/**
 * @brief Define a death test
 * Usage:
 *   FB_TEST_DEATH(raft_state, null_pointer_access) {
 *       int* ptr = nullptr;
 *       *ptr = 42;  // Should crash
 *   }
 */
#define FB_TEST_DEATH(suite, name)                                                 \
    void fb_test_death_##suite##_##name();                                         \
    static ::fastblock::test::death_test_registrar                                 \
        fb_death_reg_##suite##_##name(#suite, #name,                               \
                                       fb_test_death_##suite##_##name);            \
    void fb_test_death_##suite##_##name()

#define FB_ASSERT_DEATH(expr, expected_msg)                                        \
    do {                                                                            \
        /* Death test runs in forked process */                                    \
    } while (0)

#define FB_ASSERT_EXIT(expr, exit_code)                                            \
    do {                                                                            \
        /* Exit test runs in forked process */                                     \
    } while (0)

// ============================================================================
// Performance Benchmark Tests (FB_BENCHMARK)
// ============================================================================

/**
 * @brief Benchmark result
 */
struct benchmark_result {
    std::string name;
    int iterations;
    std::chrono::nanoseconds total_time;
    std::chrono::nanoseconds min_time;
    std::chrono::nanoseconds max_time;
    std::chrono::nanoseconds avg_time;

    double ops_per_second() const {
        return (double)iterations * 1000000000.0 / total_time.count();
    }
};

/**
 * @brief Benchmark test case
 */
class benchmark_test_case : public test_case {
public:
    using bench_func = std::function<benchmark_result()>;

    benchmark_test_case(const std::string& name, const std::string& suite,
                        bench_func func)
        : test_case(name, suite, [this, func](test_context& ctx) {
            auto result = func();
            SPDK_NOTICELOG("BENCHMARK %s: %d iterations, avg %.2f ns, "
                          "%.2f ops/sec\n",
                          result.name.c_str(), result.iterations,
                          (double)result.avg_time.count(),
                          result.ops_per_second());
        }) {}
};

/**
 * @brief Benchmark registrar
 */
class benchmark_registrar {
public:
    benchmark_registrar(const std::string& suite_name,
                        const std::string& test_name,
                        benchmark_test_case::bench_func func) {
        auto tc = std::make_shared<benchmark_test_case>(test_name, suite_name, func);
        test_registry::instance().register_test(suite_name, tc);
    }
};

/**
 * @brief Benchmark context for timing
 */
class benchmark_context {
public:
    std::string name;
    int iterations = 0;
    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::nanoseconds total_time{0};
    std::chrono::nanoseconds min_time{std::chrono::nanoseconds::max()};
    std::chrono::nanoseconds max_time{0};

    void start_iteration() {
        start_time = std::chrono::high_resolution_clock::now();
    }

    void end_iteration() {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time);
        total_time += elapsed;
        min_time = std::min(min_time, elapsed);
        max_time = std::max(max_time, elapsed);
        iterations++;
    }

    benchmark_result result() const {
        benchmark_result r;
        r.name = name;
        r.iterations = iterations;
        r.total_time = total_time;
        r.min_time = min_time;
        r.max_time = max_time;
        r.avg_time = iterations > 0 ?
            std::chrono::nanoseconds(total_time.count() / iterations) :
            std::chrono::nanoseconds(0);
        return r;
    }
};

/**
 * @brief Define a benchmark test
 * Usage:
 *   FB_BENCHMARK(raft_state, term_comparison_perf) {
 *       benchmark_context bench_ctx;
 *       for (int i = 0; i < 1000000; i++) {
 *           bench_ctx.start_iteration();
 *           raft_term_t t1 = i, t2 = i + 1;
 *           bool result = t2 > t1;
 *           bench_ctx.end_iteration();
 *       }
 *       return bench_ctx.result();
 *   }
 */
#define FB_BENCHMARK(suite, name)                                                  \
    ::fastblock::test::benchmark_result fb_benchmark_##suite##_##name();          \
    static ::fastblock::test::benchmark_registrar                                  \
        fb_bench_reg_##suite##_##name(#suite, #name,                              \
                                       fb_benchmark_##suite##_##name);              \
    ::fastblock::test::benchmark_result fb_benchmark_##suite##_##name()

#define FB_BENCHMARK_START(ctx)                                                    \
    (ctx).start_iteration()

#define FB_BENCHMARK_END(ctx, iterations)                                          \
    do {                                                                            \
        for (int fb_bench_i = 0; fb_bench_i < (iterations); fb_bench_i++) {       \
            (ctx).end_iteration();                                                 \
        }                                                                           \
    } while (0)

// ============================================================================
// Mock Object Support
// ============================================================================

/**
 * @brief Mock call record
 */
struct mock_call {
    std::string method_name;
    std::vector<std::string> args;
    bool matched = false;
};

/**
 * @brief Mock expectation
 */
struct mock_expectation {
    std::string method_name;
    int times_called = 0;
    int expected_times = -1;  // -1 = any
    std::function<bool(const mock_call&)> matcher;

    bool satisfied() const {
        return expected_times < 0 || times_called >= expected_times;
    }
};

/**
 * @brief Base mock class
 */
class mock_base {
public:
    virtual ~mock_base() = default;

    void record_call(const std::string& method, const std::vector<std::string>& args = {}) {
        mock_call call{method, args};
        _calls.push_back(call);

        for (auto& exp : _expectations) {
            if (exp.method_name == method && (!exp.matcher || exp.matcher(call))) {
                exp.times_called++;
                call.matched = true;
                break;
            }
        }
    }

    void expect_call(const std::string& method, int times = -1) {
        mock_expectation exp;
        exp.method_name = method;
        exp.expected_times = times;
        _expectations.push_back(exp);
    }

    bool verify() {
        for (const auto& exp : _expectations) {
            if (!exp.satisfied()) {
                return false;
            }
        }
        return true;
    }

    void clear() {
        _calls.clear();
        _expectations.clear();
    }

protected:
    std::vector<mock_call> _calls;
    std::vector<mock_expectation> _expectations;
};

/**
 * @brief Mock template for interfaces
 */
template<typename Interface>
class Mock : public Interface, public mock_base {
public:
    virtual ~Mock() = default;
};

/**
 * @brief Mock method helper
 * Usage:
 *   MOCK_METHOD(RaftNode, send_vote_request, (int term));
 */
#define MOCK_METHOD(return_type, method_name, args)                                \
    return_type method_name args override {                                        \
        record_call(#method_name);                                                 \
    }

/**
 * @brief Expectation macros
 */
#define EXPECT_CALL(mock_obj, method, matcher)                                    \
    do {                                                                            \
        (mock_obj).expect_call(#method);                                           \
    } while (0)

#define MOCK_TIMES(n) n

// ============================================================================
// Test Tags and Filtering
// ============================================================================

/**
 * @brief Test tag enumeration
 */
enum class test_tag {
    NONE = 0,
    QUICK = 1 << 0,
    SLOW = 1 << 1,
    INTEGRATION = 1 << 2,
    UNIT = 1 << 3,
    PERFORMANCE = 1 << 4,
    FLAKY = 1 << 5,
    SANITY = 1 << 6,
    REGRESSION = 1 << 7
};

/**
 * @brief Tagged test case
 */
class tagged_test_case : public test_case {
public:
    tagged_test_case(const std::string& name, const std::string& suite,
                     test_func func, test_tag tag)
        : test_case(name, suite, func), _tag(tag) {}

    test_tag tag() const { return _tag; }

private:
    test_tag _tag;
};

/**
 * @brief Tagged test registrar
 */
class tagged_test_registrar {
public:
    tagged_test_registrar(const std::string& suite_name,
                          const std::string& test_name,
                          test_case::test_func func,
                          test_tag tag) {
        auto tc = std::make_shared<tagged_test_case>(test_name, suite_name, func, tag);
        test_registry::instance().register_test(suite_name, tc);
    }
};

/**
 * @brief Define a tagged test
 * Usage:
 *   FB_TEST_TAGGED(raft_state, quick_test, FB_TAG(QUICK)) { ... }
 */
#define FB_TAG(name) ::fastblock::test::test_tag::name

#define FB_TEST_TAGGED(suite, name, tag)                                           \
    void fb_test_tag_##suite##_##name(::fastblock::test::test_context& ctx);       \
    static ::fastblock::test::tagged_test_registrar                                \
        fb_tag_reg_##suite##_##name(#suite, #name,                                \
                                     fb_test_tag_##suite##_##name, tag);            \
    void fb_test_tag_##suite##_##name(::fastblock::test::test_context& ctx)

/**
 * @brief Convenience macros for common tags
 */
#define FB_TEST_QUICK(suite, name) FB_TEST_TAGGED(suite, name, FB_TAG(QUICK))
#define FB_TEST_SLOW(suite, name) FB_TEST_TAGGED(suite, name, FB_TAG(SLOW))
#define FB_TEST_INTEGRATION(suite, name) FB_TEST_TAGGED(suite, name, FB_TAG(INTEGRATION))
#define FB_TEST_PERFORMANCE(suite, name) FB_TEST_TAGGED(suite, name, FB_TAG(PERFORMANCE))
#define FB_TEST_SANITY(suite, name) FB_TEST_TAGGED(suite, name, FB_TAG(SANITY))
#define FB_TEST_REGRESSION(suite, name) FB_TEST_TAGGED(suite, name, FB_TAG(REGRESSION))

// ============================================================================
// Additional Assertion Macros
// ============================================================================

/**
 * @brief Floating point comparison with tolerance
 */
#define FB_ASSERT_FLOAT_NEAR(expected, actual, tolerance)                          \
    do {                                                                            \
        double fb_diff = std::abs((expected) - (actual));                          \
        if (fb_diff > (tolerance)) {                                               \
            std::stringstream ss;                                                  \
            ss << "Float assertion failed: " << #expected << " ~ " << #actual     \
               << " (diff: " << fb_diff << ", tolerance: " << tolerance << ")";    \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief Floating point equality (with default tolerance)
 */
#define FB_ASSERT_FLOAT_EQ(expected, actual)                                       \
    FB_ASSERT_FLOAT_NEAR(expected, actual, 1e-6)

/**
 * @brief Integer near comparison
 */
#define FB_ASSERT_NEAR(expected, actual, tolerance)                                \
    do {                                                                            \
        auto fb_diff = std::abs((expected) - (actual));                            \
        if (fb_diff > (tolerance)) {                                               \
            std::stringstream ss;                                                  \
            ss << "Near assertion failed: " << #expected << " ~ " << #actual     \
               << " (diff: " << fb_diff << ", tolerance: " << tolerance << ")";    \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief String contains assertion
 */
#define FB_ASSERT_STR_CONTAINS(str, substr)                                        \
    do {                                                                            \
        std::string fb_s = (str);                                                  \
        std::string fb_sub = (substr);                                             \
        if (fb_s.find(fb_sub) == std::string::npos) {                              \
            std::stringstream ss;                                                  \
            ss << "String does not contain substring: \"" << fb_sub << "\""        \
               << " in \"" << fb_s << "\"";                                        \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief String starts with assertion
 */
#define FB_ASSERT_STR_STARTS_WITH(str, prefix)                                     \
    do {                                                                            \
        std::string fb_s = (str);                                                  \
        std::string fb_p = (prefix);                                               \
        if (fb_s.substr(0, fb_p.length()) != fb_p) {                               \
            std::stringstream ss;                                                  \
            ss << "String does not start with: \"" << fb_p << "\""                 \
               << ", actual: \"" << fb_s << "\"";                                  \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief String ends with assertion
 */
#define FB_ASSERT_STR_ENDS_WITH(str, suffix)                                       \
    do {                                                                            \
        std::string fb_s = (str);                                                  \
        std::string fb_su = (suffix);                                              \
        if (fb_s.length() < fb_su.length() ||                                      \
            fb_s.substr(fb_s.length() - fb_su.length()) != fb_su) {                \
            std::stringstream ss;                                                  \
            ss << "String does not end with: \"" << fb_su << "\""                  \
               << ", actual: \"" << fb_s << "\"";                                  \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief String not contains assertion
 */
#define FB_ASSERT_STR_NOT_CONTAINS(str, substr)                                    \
    do {                                                                            \
        std::string fb_s = (str);                                                  \
        std::string fb_sub = (substr);                                             \
        if (fb_s.find(fb_sub) != std::string::npos) {                              \
            std::stringstream ss;                                                  \
            ss << "String unexpectedly contains: \"" << fb_sub << "\""             \
               << " in \"" << fb_s << "\"";                                        \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief Exception assertion
 */
#define FB_ASSERT_THROW(expression, exception_type)                                \
    do {                                                                            \
        bool fb_caught = false;                                                    \
        try {                                                                        \
            expression;                                                             \
        } catch (const exception_type&) {                                          \
            fb_caught = true;                                                       \
        } catch (...) {                                                             \
            ctx.fail("Wrong exception type thrown", __FILE__, __LINE__);           \
            return;                                                                 \
        }                                                                           \
        if (!fb_caught) {                                                           \
            ctx.fail("No exception thrown: " #exception_type, __FILE__, __LINE__); \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASSERT_NO_THROW(expression)                                             \
    do {                                                                            \
        try {                                                                        \
            expression;                                                             \
        } catch (...) {                                                             \
            ctx.fail("Unexpected exception thrown", __FILE__, __LINE__);           \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief Container assertions
 */
#define FB_ASSERT_EMPTY(container)                                                 \
    do {                                                                            \
        if (!(container).empty()) {                                                \
            std::stringstream ss;                                                  \
            ss << "Container not empty, size: " << (container).size();             \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASSERT_SIZE(container, expected_size)                                   \
    do {                                                                            \
        if ((container).size() != (expected_size)) {                               \
            std::stringstream ss;                                                  \
            ss << "Container size mismatch: expected " << (expected_size)          \
               << ", actual " << (container).size();                               \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASSERT_CONTAINS(container, element)                                    \
    do {                                                                            \
        if (std::find((container).begin(), (container).end(), (element)) ==       \
            (container).end()) {                                                   \
            ctx.fail("Container does not contain element: " #element,             \
                    __FILE__, __LINE__);                                           \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASSERT_NOT_CONTAINS(container, element)                                 \
    do {                                                                            \
        if (std::find((container).begin(), (container).end(), (element)) !=       \
            (container).end()) {                                                   \
            ctx.fail("Container unexpectedly contains element: " #element,        \
                    __FILE__, __LINE__);                                           \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief Range assertions
 */
#define FB_ASSERT_IN_RANGE(value, min_val, max_val)                                \
    do {                                                                            \
        if ((value) < (min_val) || (value) > (max_val)) {                          \
            std::stringstream ss;                                                  \
            ss << "Value not in range: " << (value) << " not in ["                 \
               << (min_val) << ", " << (max_val) << "]";                           \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASSERT_NOT_IN_RANGE(value, min_val, max_val)                            \
    do {                                                                            \
        if ((value) >= (min_val) && (value) <= (max_val)) {                        \
            std::stringstream ss;                                                  \
            ss << "Value unexpectedly in range: " << (value) << " in ["            \
               << (min_val) << ", " << (max_val) << "]";                           \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief Bit manipulation assertions
 */
#define FB_ASSERT_BITS_SET(value, bits)                                            \
    do {                                                                            \
        if (((value) & (bits)) != (bits)) {                                        \
            std::stringstream ss;                                                  \
            ss << "Bits not set: expected " << (bits) << " in " << (value);        \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASSERT_BITS_CLEAR(value, bits)                                          \
    do {                                                                            \
        if (((value) & (bits)) != 0) {                                             \
            std::stringstream ss;                                                  \
            ss << "Bits unexpectedly set: " << (bits) << " in " << (value);        \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASSERT_BIT_SET(value, bit)                                              \
    FB_ASSERT_BITS_SET(value, (1 << (bit)))

#define FB_ASSERT_BIT_CLEAR(value, bit)                                            \
    FB_ASSERT_BITS_CLEAR(value, (1 << (bit)))

/**
 * @brief Pointer assertions
 */
#define FB_ASSERT_SAME_PTR(ptr1, ptr2)                                             \
    do {                                                                            \
        if ((ptr1) != (ptr2)) {                                                    \
            std::stringstream ss;                                                  \
            ss << "Pointers not same: " << (void*)(ptr1) << " != " << (void*)(ptr2); \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASSERT_DIFFERENT_PTR(ptr1, ptr2)                                        \
    do {                                                                            \
        if ((ptr1) == (ptr2)) {                                                    \
            ctx.fail("Pointers unexpectedly same: both are "                       \
                    << (void*)(ptr1), __FILE__, __LINE__);                         \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief Type assertions (compile-time)
 */
#define FB_ASSERT_TYPE_EQ(type1, type2)                                            \
    static_assert(std::is_same<type1, type2>::value,                               \
                  "Types are not equal: " #type1 " != " #type2)

#define FB_ASSERT_TYPE_DERIVED(derived, base)                                      \
    static_assert(std::is_base_of<base, derived>::value,                           \
                  "Type not derived: " #derived " is not derived from " #base)

/**
 * @brief Memory comparison assertion
 */
#define FB_ASSERT_MEM_EQ(expected, actual, size)                                    \
    do {                                                                            \
        const void* fb_exp = (expected);                                            \
        const void* fb_act = (actual);                                              \
        size_t fb_sz = (size);                                                      \
        if (fb_sz > 0 && memcmp(fb_exp, fb_act, fb_sz) != 0) {                      \
            std::stringstream ss;                                                  \
            ss << "Memory comparison failed: " << #expected << " != " << #actual    \
               << " (size: " << fb_sz << ")";                                       \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_ASSERT_MEM_NE(expected, actual, size)                                    \
    do {                                                                            \
        const void* fb_exp = (expected);                                            \
        const void* fb_act = (actual);                                              \
        size_t fb_sz = (size);                                                      \
        if (fb_sz > 0 && memcmp(fb_exp, fb_act, fb_sz) == 0) {                      \
            ctx.fail("Memory unexpectedly equal: " #expected " == " #actual,        \
                    __FILE__, __LINE__);                                           \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief String N-character comparison assertion
 */
#define FB_ASSERT_STRN_EQ(expected, actual, n)                                       \
    do {                                                                            \
        std::string fb_e = std::string(expected).substr(0, n);                      \
        std::string fb_a = std::string(actual).substr(0, n);                       \
        if (fb_e != fb_a) {                                                         \
            std::stringstream ss;                                                  \
            ss << "String prefix comparison failed (first " << n << " chars): "     \
               << "expected \"" << fb_e << "\", actual \"" << fb_a << "\"";         \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief String contains N-times assertion
 */
#define FB_ASSERT_STR_COUNT(str, substr, expected_count)                            \
    do {                                                                            \
        std::string fb_s = (str);                                                   \
        std::string fb_sub = (substr);                                              \
        size_t fb_count = 0;                                                       \
        size_t fb_pos = 0;                                                         \
        while ((fb_pos = fb_s.find(fb_sub, fb_pos)) != std::string::npos) {         \
            fb_count++;                                                             \
            fb_pos += fb_sub.length();                                              \
        }                                                                           \
        if (fb_count != (expected_count)) {                                        \
            std::stringstream ss;                                                  \
            ss << "String count assertion failed: expected " << (expected_count)    \
               << " occurrences of \"" << fb_sub << "\" in \"" << fb_s << "\""      \
               << ", found " << fb_count;                                           \
            ctx.fail(ss.str(), __FILE__, __LINE__);                               \
            return;                                                                 \
        }                                                                           \
    } while (0)

/**
 * @brief Scoped timer for timing code blocks
 */
class scoped_timer {
public:
    scoped_timer(const std::string& name, test_context& ctx)
        : _name(name), _ctx(ctx),
          _start(std::chrono::high_resolution_clock::now()) {}

    ~scoped_timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - _start);
        SPDK_NOTICELOG("[%s] elapsed: %ld ms\n", _name.c_str(), elapsed.count());
    }

private:
    std::string _name;
    test_context& _ctx;
    std::chrono::high_resolution_clock::time_point _start;
};

#define FB_SCOPED_TIMER(name)                                                      \
    ::fastblock::test::scoped_timer fb_timer_##name(#name, ctx)

// ============================================================================
// Test Data Generation Utilities
// ============================================================================

/**
 * @brief Random data generator for testing
 *
 * Uses thread-local std::mt19937 for thread-safe random number generation.
 * Each thread has its own random generator to avoid race conditions.
 */
class random_generator {
private:
    // Thread-local random engine for thread safety
    static std::mt19937& get_engine() {
        static thread_local std::mt19937 engine(std::random_device{}());
        return engine;
    }

public:
    static int random_int(int min_val, int max_val) {
        std::uniform_int_distribution<int> dist(min_val, max_val);
        return dist(get_engine());
    }

    static uint64_t random_uint64(uint64_t min_val, uint64_t max_val) {
        std::uniform_int_distribution<uint64_t> dist(min_val, max_val);
        return dist(get_engine());
    }

    static std::string random_string(size_t length) {
        static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::uniform_int_distribution<size_t> dist(0, sizeof(chars) - 2);
        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += chars[dist(get_engine())];
        }
        return result;
    }

    static std::vector<uint8_t> random_bytes(size_t length) {
        std::uniform_int_distribution<int> dist(0, 255);
        std::vector<uint8_t> result(length);
        for (size_t i = 0; i < length; ++i) {
            result[i] = static_cast<uint8_t>(dist(get_engine()));
        }
        return result;
    }

    static double random_double(double min_val, double max_val) {
        std::uniform_real_distribution<double> dist(min_val, max_val);
        return dist(get_engine());
    }

    static bool random_bool() {
        std::uniform_int_distribution<int> dist(0, 1);
        return dist(get_engine()) == 1;
    }

    // Seed the random engine (useful for reproducible tests)
    static void seed(unsigned int seed_value) {
        get_engine().seed(seed_value);
    }
};

#define FB_RANDOM_INT(min, max)        ::fastblock::test::random_generator::random_int(min, max)
#define FB_RANDOM_UINT64(min, max)     ::fastblock::test::random_generator::random_uint64(min, max)
#define FB_RANDOM_STRING(len)          ::fastblock::test::random_generator::random_string(len)
#define FB_RANDOM_BYTES(len)           ::fastblock::test::random_generator::random_bytes(len)
#define FB_RANDOM_DOUBLE(min, max)     ::fastblock::test::random_generator::random_double(min, max)
#define FB_RANDOM_BOOL()               ::fastblock::test::random_generator::random_bool()

/**
 * @brief Test value builder for creating test scenarios
 */
class test_value_builder {
public:
    template<typename T>
    static std::vector<T> range(T start, T end, T step = 1) {
        std::vector<T> result;
        for (T i = start; i <= end; i += step) {
            result.push_back(i);
        }
        return result;
    }

    template<typename T>
    static std::vector<T> repeat(T value, size_t count) {
        std::vector<T> result(count, value);
        return result;
    }

    template<typename T>
    static std::vector<T> shuffle(std::vector<T> values) {
        std::shuffle(values.begin(), values.end(), std::mt19937(std::random_device{}()));
        return values;
    }
};

#define FB_RANGE(start, end, step)     ::fastblock::test::test_value_builder::range(start, end, step)
#define FB_REPEAT(value, count)        ::fastblock::test::test_value_builder::repeat(value, count)
#define FB_SHUFFLE(values)             ::fastblock::test::test_value_builder::shuffle(values)

// ============================================================================
// Enhanced Mock Framework
// ============================================================================

/**
 * @brief Mock matcher for flexible expectations
 */
class mock_matcher {
public:
    template<typename T>
    static std::function<bool(const mock_call&)> any() {
        return [](const mock_call&) { return true; };
    }

    template<typename T>
    static std::function<bool(const mock_call&)> eq(const T& expected) {
        return [expected](const mock_call& call) {
            if (call.args.empty()) return false;
            return call.args[0] == std::to_string(expected);
        };
    }

    template<typename T>
    static std::function<bool(const mock_call&)> between(const T& min_val, const T& max_val) {
        return [min_val, max_val](const mock_call& call) {
            if (call.args.empty()) return false;
            T val = std::stoi(call.args[0]);
            return val >= min_val && val <= max_val;
        };
    }

    static std::function<bool(const mock_call&)> contains(const std::string& substr) {
        return [substr](const mock_call& call) {
            for (const auto& arg : call.args) {
                if (arg.find(substr) != std::string::npos) return true;
            }
            return false;
        };
    }
};

#define FB_MOCK_ANY()                   ::fastblock::test::mock_matcher::any()
#define FB_MOCK_EQ(value)               ::fastblock::test::mock_matcher::eq(value)
#define FB_MOCK_BETWEEN(min, max)       ::fastblock::test::mock_matcher::between(min, max)
#define FB_MOCK_CONTAINS(substr)        ::fastblock::test::mock_matcher::contains(substr)

/**
 * @brief Enhanced mock verification
 */
class mock_verifier {
public:
    static bool verify_all(std::initializer_list<mock_base*> mocks) {
        for (auto* mock : mocks) {
            if (!mock->verify()) return false;
        }
        return true;
    }

    static void clear_all(std::initializer_list<mock_base*> mocks) {
        for (auto* mock : mocks) {
            mock->clear();
        }
    }
};

#define FB_VERIFY_ALL(...)              ::fastblock::test::mock_verifier::verify_all({__VA_ARGS__})
#define FB_CLEAR_ALL(...)               ::fastblock::test::mock_verifier::clear_all({__VA_ARGS__})

// ============================================================================
// Test Environment Configuration
// ============================================================================

/**
 * @brief Test environment for managing test configuration
 */
class test_environment {
public:
    static test_environment& instance() {
        static test_environment env;
        return env;
    }

    void set_var(const std::string& key, const std::string& value) {
        _vars[key] = value;
    }

    std::string get_var(const std::string& key, const std::string& default_val = "") {
        auto it = _vars.find(key);
        return it != _vars.end() ? it->second : default_val;
    }

    bool has_var(const std::string& key) {
        return _vars.find(key) != _vars.end();
    }

    void clear() {
        _vars.clear();
    }

    void set_timeout(uint32_t seconds) {
        _default_timeout = seconds;
    }

    uint32_t timeout() const {
        return _default_timeout;
    }

    void set_verbose(bool verbose) {
        _verbose = verbose;
    }

    bool verbose() const {
        return _verbose;
    }

private:
    test_environment() : _default_timeout(300), _verbose(false) {}
    std::map<std::string, std::string> _vars;
    uint32_t _default_timeout;
    bool _verbose;
};

#define FB_SET_ENV(key, value)          ::fastblock::test::test_environment::instance().set_var(key, value)
#define FB_GET_ENV(key, default)        ::fastblock::test::test_environment::instance().get_var(key, default)
#define FB_SET_TIMEOUT(sec)             ::fastblock::test::test_environment::instance().set_timeout(sec)
#define FB_GET_TIMEOUT()                ::fastblock::test::test_environment::instance().timeout()
#define FB_SET_VERBOSE(flag)            ::fastblock::test::test_environment::instance().set_verbose(flag)

// ============================================================================
// Test Preconditions Checker
// ============================================================================

/**
 * @brief Test precondition checker
 */
class precondition_checker {
public:
    static bool check_environment_var(const std::string& key) {
        return test_environment::instance().has_var(key);
    }

    static bool check_file_exists(const std::string& path) {
        std::ifstream f(path);
        return f.good();
    }

    static bool check_minimum_version(const std::string& version, const std::string& required) {
        return version >= required;  // Simple string comparison
    }
};

#define FB_REQUIRE_ENV(key)                                                        \
    do {                                                                            \
        if (!::fastblock::test::precondition_checker::check_environment_var(key)) { \
            ctx.skip("Missing environment variable: " #key);                        \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_REQUIRE_FILE(path)                                                     \
    do {                                                                            \
        if (!::fastblock::test::precondition_checker::check_file_exists(path)) {   \
            ctx.skip("Missing required file: " #path);                              \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define FB_REQUIRE_VERSION(version, required)                                      \
    do {                                                                            \
        if (!::fastblock::test::precondition_checker::check_minimum_version(      \
                version, required)) {                                              \
            ctx.skip("Version requirement not met: " #version " < " #required);    \
            return;                                                                 \
        }                                                                           \
    } while (0)

// ============================================================================
// Test Execution Helpers
// ============================================================================

/**
 * @brief Test retry helper for flaky tests
 */
template<typename Func>
bool retry_test(Func func, int max_attempts = 3, int delay_ms = 100) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        try {
            func();
            return true;
        } catch (...) {
            if (attempt < max_attempts - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
    }
    return false;
}

#define FB_RETRY(max_attempts, delay_ms)                                            \
    for (int fb_attempt = 0; fb_attempt < (max_attempts); ++fb_attempt)             \
        if (fb_attempt > 0)                                                         \
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));       \
        else

/**
 * @brief Test loop helper for stress testing
 */
#define FB_STRESS_LOOP(iterations)                                                  \
    for (int fb_stress_i = 0; fb_stress_i < (iterations); ++fb_stress_i)

#define FB_STRESS_RUN_UNTIL(condition, max_iterations)                              \
    for (int fb_stress_i = 0; fb_stress_i < (max_iterations) && !(condition); ++fb_stress_i)

/**
 * @brief Test timing helper
 */
class execution_timer {
public:
    execution_timer() : _start(std::chrono::high_resolution_clock::now()) {}

    void reset() {
        _start = std::chrono::high_resolution_clock::now();
    }

    std::chrono::nanoseconds elapsed_ns() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now - _start);
    }

    std::chrono::milliseconds elapsed_ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - _start);
    }

    std::chrono::seconds elapsed_sec() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - _start);
    }

private:
    std::chrono::high_resolution_clock::time_point _start;
};

#define FB_TIMER_START()              ::fastblock::test::execution_timer fb_timer_
#define FB_TIMER_ELAPSED_NS()         fb_timer_.elapsed_ns()
#define FB_TIMER_ELAPSED_MS()         fb_timer_.elapsed_ms()
#define FB_TIMER_ELAPSED_SEC()        fb_timer_.elapsed_sec()
#define FB_TIMER_RESET()              fb_timer_.reset()

// ============================================================================
// Test Output Formatting
// ============================================================================

/**
 * @brief Test output formatter
 */
class test_formatter {
public:
    static std::string format_result(const test_result& result) {
        std::stringstream ss;
        ss << "[" << test_status_str(result.status) << "] "
           << result.suite_name << "." << result.test_name;
        if (!result.message.empty()) {
            ss << " - " << result.message;
        }
        if (result.duration.count() > 0) {
            ss << " (" << result.duration.count() / 1000.0 << " ms)";
        }
        return ss.str();
    }

    static std::string format_summary(int total, int passed, int failed, int skipped) {
        std::stringstream ss;
        ss << "Tests: " << total << " total, "
           << passed << " passed, " << failed << " failed, "
           << skipped << " skipped";
        return ss.str();
    }

    static std::string format_progress(int current, int total, const std::string& test_name) {
        std::stringstream ss;
        ss << "[" << current << "/" << total << "] " << test_name;
        return ss.str();
    }

    static std::string format_error(const std::string& test_name, const std::string& error,
                                     const std::string& file, int line) {
        std::stringstream ss;
        ss << "ERROR: " << test_name << " failed at " << file << ":" << line
           << "\n  " << error;
        return ss.str();
    }
};

#define FB_FORMAT_RESULT(result)       ::fastblock::test::test_formatter::format_result(result)
#define FB_FORMAT_SUMMARY(total, passed, failed, skipped)                          \
    ::fastblock::test::test_formatter::format_summary(total, passed, failed, skipped)
#define FB_FORMAT_PROGRESS(current, total, name)                                    \
    ::fastblock::test::test_formatter::format_progress(current, total, name)
#define FB_FORMAT_ERROR(name, error, file, line)                                   \
    ::fastblock::test::test_formatter::format_error(name, error, file, line)

// ============================================================================
// Memory and Resource Tracking
// ============================================================================

/**
 * @brief Simple memory tracker for detecting leaks in tests
 */
class memory_tracker {
public:
    static memory_tracker& instance() {
        static memory_tracker tracker;
        return tracker;
    }

    void record_allocation(size_t size) {
        _allocations += size;
        _alloc_count++;
    }

    void record_deallocation(size_t size) {
        _deallocations += size;
        _dealloc_count++;
    }

    size_t allocated() const { return _allocations; }
    size_t deallocated() const { return _deallocations; }
    size_t leak() const { return _allocations - _deallocations; }
    size_t alloc_count() const { return _alloc_count; }
    size_t dealloc_count() const { return _dealloc_count; }

    void reset() {
        _allocations = 0;
        _deallocations = 0;
        _alloc_count = 0;
        _dealloc_count = 0;
    }

private:
    memory_tracker() : _allocations(0), _deallocations(0), _alloc_count(0), _dealloc_count(0) {}
    size_t _allocations;
    size_t _deallocations;
    size_t _alloc_count;
    size_t _dealloc_count;
};

#define FB_RECORD_ALLOC(size)          ::fastblock::test::memory_tracker::instance().record_allocation(size)
#define FB_RECORD_DEALLOC(size)        ::fastblock::test::memory_tracker::instance().record_deallocation(size)
#define FB_MEMORY_LEAK()               ::fastblock::test::memory_tracker::instance().leak()
#define FB_RESET_MEMORY_TRACKER()      ::fastblock::test::memory_tracker::instance().reset()

/**
 * @brief Resource guard for automatic cleanup
 */
template<typename T, typename CleanupFunc>
class resource_guard {
public:
    resource_guard(T resource, CleanupFunc cleanup)
        : _resource(resource), _cleanup(cleanup), _released(false) {}

    ~resource_guard() {
        if (!_released) {
            _cleanup(_resource);
        }
    }

    T get() const { return _resource; }

    void release() { _released = true; }

private:
    T _resource;
    CleanupFunc _cleanup;
    bool _released;
};

template<typename T, typename CleanupFunc>
resource_guard<T, CleanupFunc> make_guard(T resource, CleanupFunc cleanup) {
    return resource_guard<T, CleanupFunc>(resource, cleanup);
}

#define FB_RESOURCE_GUARD(resource, cleanup)                                        \
    ::fastblock::test::make_guard(resource, cleanup)

/**
 * @brief Scope exit for deferred cleanup
 */
class scope_exit {
public:
    template<typename Func>
    explicit scope_exit(Func&& func) : _func(std::forward<Func>(func)), _dismissed(false) {}

    ~scope_exit() {
        if (!_dismissed) _func();
    }

    void dismiss() { _dismissed = true; }

private:
    std::function<void()> _func;
    bool _dismissed;
};

#define FB_SCOPE_EXIT(code)                                                         \
    ::fastblock::test::scope_exit fb_scope_exit_##__LINE__([&]() { code; })

// ============================================================================
// Thread and Concurrency Testing
// ============================================================================

/**
 * @brief Thread barrier for synchronized testing
 */
class thread_barrier {
public:
    thread_barrier(int count) : _threshold(count), _count(count), _generation(0) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(_mutex);
        int gen = _generation;
        if (--_count == 0) {
            _generation++;
            _count = _threshold;
            _cv.notify_all();
        } else {
            _cv.wait(lock, [this, gen] { return gen != _generation; });
        }
    }

private:
    std::mutex _mutex;
    std::condition_variable _cv;
    int _threshold;
    int _count;
    int _generation;
};

/**
 * @brief Atomic test counter for concurrent tests
 */
class atomic_test_counter {
public:
    atomic_test_counter() : _count(0) {}
    void increment() { _count++; }
    void decrement() { _count--; }
    int get() const { return _count; }
    void reset() { _count = 0; }
private:
    std::atomic<int> _count;
};

/**
 * @brief Thread-safe flag for signaling
 */
class thread_flag {
public:
    thread_flag() : _flag(false) {}
    void set() { _flag = true; _cv.notify_all(); }
    void wait() {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [this] { return _flag.load(); });
    }
    bool wait_for(int timeout_ms) {
        std::unique_lock<std::mutex> lock(_mutex);
        return _cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this] { return _flag.load(); });
    }
    bool is_set() const { return _flag; }
    void reset() { _flag = false; }
private:
    std::mutex _mutex;
    std::condition_variable _cv;
    std::atomic<bool> _flag;
};

#define FB_THREAD_BARRIER(count)      ::fastblock::test::thread_barrier(count)
#define FB_ATOMIC_COUNTER             ::fastblock::test::atomic_test_counter
#define FB_THREAD_FLAG                 ::fastblock::test::thread_flag

/**
 * @brief Concurrent test executor
 */
template<typename Func>
void run_concurrent(int thread_count, Func func) {
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([i, &func]() { func(i); });
    }
    for (auto& t : threads) {
        t.join();
    }
}

#define FB_RUN_CONCURRENT(count, func)                                             \
    ::fastblock::test::run_concurrent(count, func)

// ============================================================================
// Predicate Helpers
// ============================================================================

/**
 * @brief Predicate builders for complex conditions
 */
namespace predicates {

template<typename T>
std::function<bool(T)> always_true() {
    return [](T) { return true; };
}

template<typename T>
std::function<bool(T)> always_false() {
    return [](T) { return false; };
}

template<typename T>
std::function<bool(T)> is_equal(const T& expected) {
    return [expected](T actual) { return actual == expected; };
}

template<typename T>
std::function<bool(T)> is_not_equal(const T& unexpected) {
    return [unexpected](T actual) { return actual != unexpected; };
}

template<typename T>
std::function<bool(T)> is_greater_than(const T& threshold) {
    return [threshold](T actual) { return actual > threshold; };
}

template<typename T>
std::function<bool(T)> is_less_than(const T& threshold) {
    return [threshold](T actual) { return actual < threshold; };
}

template<typename T>
std::function<bool(T)> is_greater_or_equal(const T& threshold) {
    return [threshold](T actual) { return actual >= threshold; };
}

template<typename T>
std::function<bool(T)> is_less_or_equal(const T& threshold) {
    return [threshold](T actual) { return actual <= threshold; };
}

template<typename T>
std::function<bool(T)> is_between(const T& min_val, const T& max_val) {
    return [min_val, max_val](T actual) {
        return actual >= min_val && actual <= max_val;
    };
}

template<typename T>
std::function<bool(T)> is_not_between(const T& min_val, const T& max_val) {
    return [min_val, max_val](T actual) {
        return actual < min_val || actual > max_val;
    };
}

template<typename T>
std::function<bool(T)> is_null() {
    return [](T actual) { return actual == nullptr; };
}

template<typename T>
std::function<bool(T)> is_not_null() {
    return [](T actual) { return actual != nullptr; };
}

template<typename T>
std::function<bool(T)> is_one_of(const std::vector<T>& values) {
    return [&values](T actual) {
        return std::find(values.begin(), values.end(), actual) != values.end();
    };
}

template<typename T>
std::function<bool(T)> is_none_of(const std::vector<T>& values) {
    return [&values](T actual) {
        return std::find(values.begin(), values.end(), actual) == values.end();
    };
}

template<typename T>
std::function<bool(T)> negate(std::function<bool(T)> predicate) {
    return [predicate](T actual) { return !predicate(actual); };
}

template<typename T>
std::function<bool(T)> combine_and(std::function<bool(T)> p1, std::function<bool(T)> p2) {
    return [p1, p2](T actual) { return p1(actual) && p2(actual); };
}

template<typename T>
std::function<bool(T)> combine_or(std::function<bool(T)> p1, std::function<bool(T)> p2) {
    return [p1, p2](T actual) { return p1(actual) || p2(actual); };
}

} // namespace predicates

// Predicate convenience macros
#define FB_PRED_TRUE()                 ::fastblock::test::predicates::always_true()
#define FB_PRED_FALSE()                ::fastblock::test::predicates::always_false()
#define FB_PRED_EQ(value)              ::fastblock::test::predicates::is_equal(value)
#define FB_PRED_NE(value)              ::fastblock::test::predicates::is_not_equal(value)
#define FB_PRED_GT(value)              ::fastblock::test::predicates::is_greater_than(value)
#define FB_PRED_LT(value)              ::fastblock::test::predicates::is_less_than(value)
#define FB_PRED_GE(value)              ::fastblock::test::predicates::is_greater_or_equal(value)
#define FB_PRED_LE(value)              ::fastblock::test::predicates::is_less_or_equal(value)
#define FB_PRED_BETWEEN(min, max)      ::fastblock::test::predicates::is_between(min, max)
#define FB_PRED_NOT_BETWEEN(min, max)   ::fastblock::test::predicates::is_not_between(min, max)
#define FB_PRED_NULL()                 ::fastblock::test::predicates::is_null()
#define FB_PRED_NOT_NULL()             ::fastblock::test::predicates::is_not_null()
#define FB_PRED_ONE_OF(values)          ::fastblock::test::predicates::is_one_of(values)
#define FB_PRED_NONE_OF(values)         ::fastblock::test::predicates::is_none_of(values)
#define FB_PRED_NEGATE(p)               ::fastblock::test::predicates::negate(p)
#define FB_PRED_AND(p1, p2)             ::fastblock::test::predicates::combine_and(p1, p2)
#define FB_PRED_OR(p1, p2)              ::fastblock::test::predicates::combine_or(p1, p2)

// ============================================================================
// Test Comparison Helpers
// ============================================================================

/**
 * @brief Deep comparison for containers
 */
class comparison_helper {
public:
    template<typename Container>
    static bool containers_equal(const Container& a, const Container& b) {
        if (a.size() != b.size()) return false;
        auto it_a = a.begin();
        auto it_b = b.begin();
        while (it_a != a.end() && it_b != b.end()) {
            if (*it_a != *it_b) return false;
            ++it_a;
            ++it_b;
        }
        return true;
    }

    template<typename Container>
    static bool containers_equivalent(const Container& a, const Container& b) {
        if (a.size() != b.size()) return false;
        for (const auto& elem : a) {
            if (std::find(b.begin(), b.end(), elem) == b.end()) return false;
        }
        return true;
    }

    template<typename Container, typename Func>
    static bool containers_equal_by(const Container& a, const Container& b, Func comparator) {
        if (a.size() != b.size()) return false;
        auto it_a = a.begin();
        auto it_b = b.begin();
        while (it_a != a.end() && it_b != b.end()) {
            if (!comparator(*it_a, *it_b)) return false;
            ++it_a;
            ++it_b;
        }
        return true;
    }

    template<typename T>
    static bool approximately_equal(T a, T b, T tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    template<typename T>
    static bool relatively_equal(T a, T b, T epsilon) {
        if (a == b) return true;
        T diff = std::abs(a - b);
        T max_val = std::max(std::abs(a), std::abs(b));
        return diff <= max_val * epsilon;
    }
};

#define FB_CONTAINER_EQ(a, b)                                                      \
    ::fastblock::test::comparison_helper::containers_equal(a, b)

#define FB_CONTAINER_EQUIV(a, b)                                                    \
    ::fastblock::test::comparison_helper::containers_equivalent(a, b)

#define FB_CONTAINER_EQ_BY(a, b, comp)                                              \
    ::fastblock::test::comparison_helper::containers_equal_by(a, b, comp)

#define FB_APPROX_EQ(a, b, tol)                                                     \
    ::fastblock::test::comparison_helper::approximately_equal(a, b, tol)

#define FB_RELATIVE_EQ(a, b, eps)                                                   \
    ::fastblock::test::comparison_helper::relatively_equal(a, b, eps)

// ============================================================================
// Error Simulation Utilities
// ============================================================================

/**
 * @brief Error injector for testing error handling
 */
class error_injector {
public:
    static error_injector& instance() {
        static error_injector injector;
        return injector;
    }

    void enable() { _enabled = true; }
    void disable() { _enabled = false; }
    bool is_enabled() const { return _enabled; }

    void set_error_rate(double rate) { _error_rate = rate; }
    double error_rate() const { return _error_rate; }

    bool should_inject() {
        if (!_enabled) return false;
        return (double)rand() / RAND_MAX < _error_rate;
    }

    void set_error_type(const std::string& type) { _error_type = type; }
    const std::string& error_type() const { return _error_type; }

    void reset() {
        _enabled = false;
        _error_rate = 0.0;
        _error_type.clear();
        _failure_count = 0;
        _success_count = 0;
    }

    void record_failure() { _failure_count++; }
    void record_success() { _success_count++; }
    size_t failure_count() const { return _failure_count; }
    size_t success_count() const { return _success_count; }

private:
    error_injector() : _enabled(false), _error_rate(0.0), _failure_count(0), _success_count(0) {}
    bool _enabled;
    double _error_rate;
    std::string _error_type;
    size_t _failure_count;
    size_t _success_count;
};

#define FB_ERROR_INJECT_ENABLE()       ::fastblock::test::error_injector::instance().enable()
#define FB_ERROR_INJECT_DISABLE()      ::fastblock::test::error_injector::instance().disable()
#define FB_ERROR_INJECT_SET_RATE(r)    ::fastblock::test::error_injector::instance().set_error_rate(r)
#define FB_ERROR_INJECT_SHOULD()       ::fastblock::test::error_injector::instance().should_inject()
#define FB_ERROR_INJECT_RESET()        ::fastblock::test::error_injector::instance().reset()

/**
 * @brief Fault injector for controlled failures
 */
class fault_injector {
public:
    void set_fault_point(const std::string& name, bool should_fail = true) {
        _fault_points[name] = should_fail;
    }

    bool should_fail(const std::string& name) {
        auto it = _fault_points.find(name);
        if (it != _fault_points.end() && it->second) {
            it->second = false;  // Reset after trigger
            return true;
        }
        return false;
    }

    void clear() { _fault_points.clear(); }

private:
    std::map<std::string, bool> _fault_points;
};

#define FB_FAULT_SET(name)             ::fastblock::test::fault_injector().set_fault_point(name, true)
#define FB_FAULT_CHECK(name)           ::fastblock::test::fault_injector().should_fail(name)
#define FB_FAULT_CLEAR()               ::fastblock::test::fault_injector().clear()

/**
 * @brief Exception simulator for testing exception handling
 */
class exception_simulator {
public:
    template<typename ExceptionType>
    static void throw_if(bool condition, const std::string& message = "") {
        if (condition) {
            throw ExceptionType(message);
        }
    }

    template<typename ExceptionType>
    static void throw_randomly(double probability, const std::string& message = "") {
        if ((double)rand() / RAND_MAX < probability) {
            throw ExceptionType(message);
        }
    }

    template<typename Func>
    static bool throws_exception(Func func) {
        try {
            func();
            return false;
        } catch (...) {
            return true;
        }
    }
};

#define FB_THROW_IF(condition, exception_type, message)                             \
    ::fastblock::test::exception_simulator::throw_if<exception_type>(condition, message)

#define FB_THROW_RANDOM(probability, exception_type, message)                      \
    ::fastblock::test::exception_simulator::throw_randomly<exception_type>(probability, message)

#define FB_THROWS(func)                                                             \
    ::fastblock::test::exception_simulator::throws_exception(func)

// ============================================================================
// Test Data Validation
// ============================================================================

/**
 * @brief Data validator for test data verification
 */
class data_validator {
public:
    template<typename T>
    static bool is_valid_range(const T& value, const T& min_val, const T& max_val) {
        return value >= min_val && value <= max_val;
    }

    template<typename T>
    static bool is_positive(const T& value) {
        return value > 0;
    }

    template<typename T>
    static bool is_non_negative(const T& value) {
        return value >= 0;
    }

    template<typename T>
    static bool is_valid_index(const T& index, const T& size) {
        return index >= 0 && index < size;
    }

    static bool is_valid_string(const std::string& str, size_t min_len = 0, size_t max_len = SIZE_MAX) {
        return str.length() >= min_len && str.length() <= max_len;
    }

    static bool is_numeric(const std::string& str) {
        if (str.empty()) return false;
        for (char c : str) {
            if (!std::isdigit(c)) return false;
        }
        return true;
    }

    static bool is_alphanumeric(const std::string& str) {
        if (str.empty()) return false;
        for (char c : str) {
            if (!std::isalnum(c)) return false;
        }
        return true;
    }

    static bool is_valid_email(const std::string& email) {
        return email.find('@') != std::string::npos && email.find('.') != std::string::npos;
    }

    template<typename T>
    static bool is_sorted(const std::vector<T>& vec, bool ascending = true) {
        if (vec.size() < 2) return true;
        for (size_t i = 1; i < vec.size(); ++i) {
            if (ascending && vec[i] < vec[i-1]) return false;
            if (!ascending && vec[i] > vec[i-1]) return false;
        }
        return true;
    }

    template<typename T>
    static bool is_unique(const std::vector<T>& vec) {
        std::set<T> seen;
        for (const auto& elem : vec) {
            if (seen.count(elem)) return false;
            seen.insert(elem);
        }
        return true;
    }
};

#define FB_VALID_RANGE(val, min, max)                                               \
    ::fastblock::test::data_validator::is_valid_range(val, min, max)

#define FB_IS_POSITIVE(val)                                                         \
    ::fastblock::test::data_validator::is_positive(val)

#define FB_IS_NON_NEGATIVE(val)                                                     \
    ::fastblock::test::data_validator::is_non_negative(val)

#define FB_VALID_INDEX(idx, size)                                                   \
    ::fastblock::test::data_validator::is_valid_index(idx, size)

#define FB_VALID_STRING(str, min_len, max_len)                                      \
    ::fastblock::test::data_validator::is_valid_string(str, min_len, max_len)

#define FB_IS_NUMERIC(str)                                                          \
    ::fastblock::test::data_validator::is_numeric(str)

#define FB_IS_ALPHANUMERIC(str)                                                     \
    ::fastblock::test::data_validator::is_alphanumeric(str)

#define FB_IS_SORTED(vec, asc)                                                      \
    ::fastblock::test::data_validator::is_sorted(vec, asc)

#define FB_IS_UNIQUE(vec)                                                           \
    ::fastblock::test::data_validator::is_unique(vec)

/**
 * @brief Schema validator for structured data
 */
class schema_validator {
public:
    template<typename T>
    static bool validate_field(const T& value, const std::string& name,
                                std::function<bool(const T&)> validator) {
        return validator(value);
    }

    template<typename T>
    static bool validate_required(const T& value) {
        return true;  // Field exists
    }

    template<typename T>
    static bool validate_optional(const std::optional<T>& value,
                                   std::function<bool(const T&)> validator) {
        if (!value.has_value()) return true;  // Optional, missing is OK
        return validator(value.value());
    }
};

#define FB_VALIDATE_FIELD(value, name, validator)                                   \
    ::fastblock::test::schema_validator::validate_field(value, name, validator)

#define FB_VALIDATE_REQUIRED(value)                                                 \
    ::fastblock::test::schema_validator::validate_required(value)

#define FB_VALIDATE_OPTIONAL(value, validator)                                      \
    ::fastblock::test::schema_validator::validate_optional(value, validator)

// ============================================================================
// Test Metrics Collection
// ============================================================================

/**
 * @brief Metrics collector for test performance tracking
 */
class metrics_collector {
public:
    static metrics_collector& instance() {
        static metrics_collector collector;
        return collector;
    }

    void record_latency(const std::string& name, std::chrono::nanoseconds latency) {
        auto& metric = _latencies[name];
        metric.total += latency;
        metric.count++;
        metric.min = std::min(metric.min, latency);
        metric.max = std::max(metric.max, latency);
    }

    void record_count(const std::string& name, int64_t delta = 1) {
        _counts[name] += delta;
    }

    void record_value(const std::string& name, double value) {
        auto& metric = _values[name];
        metric.total += value;
        metric.count++;
        metric.min = std::min(metric.min, value);
        metric.max = std::max(metric.max, value);
    }

    struct latency_metric {
        std::chrono::nanoseconds total{0};
        size_t count = 0;
        std::chrono::nanoseconds min{std::numeric_limits<std::chrono::nanoseconds>::max()};
        std::chrono::nanoseconds max{0};

        double avg_ns() const {
            return count > 0 ? (double)total.count() / count : 0;
        }
        double avg_ms() const { return avg_ns() / 1000000; }
    };

    struct value_metric {
        double total = 0;
        size_t count = 0;
        double min = std::numeric_limits<double>::max();
        double max = std::numeric_limits<double>::lowest();

        double avg() const { return count > 0 ? total / count : 0; }
    };

    latency_metric get_latency(const std::string& name) const {
        auto it = _latencies.find(name);
        return it != _latencies.end() ? it->second : latency_metric{};
    }

    int64_t get_count(const std::string& name) const {
        auto it = _counts.find(name);
        return it != _counts.end() ? it->second : 0;
    }

    value_metric get_value(const std::string& name) const {
        auto it = _values.find(name);
        return it != _values.end() ? it->second : value_metric{};
    }

    void reset() {
        _latencies.clear();
        _counts.clear();
        _values.clear();
    }

    void reset_metric(const std::string& name) {
        _latencies.erase(name);
        _counts.erase(name);
        _values.erase(name);
    }

private:
    metrics_collector() = default;
    std::map<std::string, latency_metric> _latencies;
    std::map<std::string, int64_t> _counts;
    std::map<std::string, value_metric> _values;
};

/**
 * @brief RAII latency recorder
 */
class latency_scope {
public:
    latency_scope(const std::string& name)
        : _name(name), _start(std::chrono::high_resolution_clock::now()) {}

    ~latency_scope() {
        auto end = std::chrono::high_resolution_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end - _start);
        metrics_collector::instance().record_latency(_name, latency);
    }

private:
    std::string _name;
    std::chrono::high_resolution_clock::time_point _start;
};

#define FB_METRICS_RECORD_LATENCY(name, latency)                                    \
    ::fastblock::test::metrics_collector::instance().record_latency(name, latency)

#define FB_METRICS_RECORD_COUNT(name, delta)                                        \
    ::fastblock::test::metrics_collector::instance().record_count(name, delta)

#define FB_METRICS_RECORD_VALUE(name, value)                                        \
    ::fastblock::test::metrics_collector::instance().record_value(name, value)

#define FB_METRICS_GET_LATENCY(name)                                                \
    ::fastblock::test::metrics_collector::instance().get_latency(name)

#define FB_METRICS_GET_COUNT(name)                                                  \
    ::fastblock::test::metrics_collector::instance().get_count(name)

#define FB_METRICS_GET_VALUE(name)                                                  \
    ::fastblock::test::metrics_collector::instance().get_value(name)

#define FB_METRICS_RESET()                                                          \
    ::fastblock::test::metrics_collector::instance().reset()

#define FB_METRICS_RESET_METRIC(name)                                              \
    ::fastblock::test::metrics_collector::instance().reset_metric(name)

#define FB_LATENCY_SCOPE(name)                                                      \
    ::fastblock::test::latency_scope fb_latency_scope_##name(#name)

// ============================================================================
// Enhanced Test Logging
// ============================================================================

/**
 * @brief Log level for test output
 */
enum class test_log_level {
    TRACE,
    FB_DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

/**
 * @brief Enhanced test logger
 */
class test_logger {
public:
    static test_logger& instance() {
        static test_logger logger;
        return logger;
    }

    void set_level(test_log_level level) { _level = level; }
    test_log_level level() const { return _level; }

    void log(test_log_level level, const std::string& message,
             const char* file = nullptr, int line = 0) {
        if (level < _level) return;

        const char* level_str = level_to_string(level);
        std::stringstream ss;
        ss << "[" << level_str << "] ";
        if (file) {
            ss << file << ":" << line << " - ";
        }
        ss << message;

        if (level >= test_log_level::ERROR) {
            SPDK_ERRLOG("%s\n", ss.str().c_str());
        } else {
            SPDK_NOTICELOG("%s\n", ss.str().c_str());
        }
    }

    void trace(const std::string& msg, const char* file = nullptr, int line = 0) {
        log(test_log_level::TRACE, msg, file, line);
    }

    void debug(const std::string& msg, const char* file = nullptr, int line = 0) {
        log(test_log_level::FB_DEBUG, msg, file, line);
    }

    void info(const std::string& msg, const char* file = nullptr, int line = 0) {
        log(test_log_level::INFO, msg, file, line);
    }

    void warn(const std::string& msg, const char* file = nullptr, int line = 0) {
        log(test_log_level::WARN, msg, file, line);
    }

    void error(const std::string& msg, const char* file = nullptr, int line = 0) {
        log(test_log_level::ERROR, msg, file, line);
    }

    void fatal(const std::string& msg, const char* file = nullptr, int line = 0) {
        log(test_log_level::FATAL, msg, file, line);
    }

private:
    test_logger() : _level(test_log_level::INFO) {}

    const char* level_to_string(test_log_level level) {
        switch (level) {
            case test_log_level::TRACE: return "TRACE";
            case test_log_level::FB_DEBUG: return "DEBUG";
            case test_log_level::INFO: return "INFO";
            case test_log_level::WARN: return "WARN";
            case test_log_level::ERROR: return "ERROR";
            case test_log_level::FATAL: return "FATAL";
            default: return "UNKNOWN";
        }
    }

    test_log_level _level;
};


/**
 * @brief Structured logging for complex data
 */
class structured_logger {
public:
    template<typename K, typename V>
    static std::string format_kv(const K& key, const V& value) {
        std::stringstream ss;
        ss << key << "=" << value;
        return ss.str();
    }

    template<typename K, typename V>
    static std::string format_kv_list(std::initializer_list<std::pair<K, V>> pairs) {
        std::stringstream ss;
        ss << "{";
        bool first = true;
        for (const auto& p : pairs) {
            if (!first) ss << ", ";
            ss << p.first << "=" << p.second;
            first = false;
        }
        ss << "}";
        return ss.str();
    }
};

#define FB_LOG_KV(key, value)                                                       \
    ::fastblock::test::structured_logger::format_kv(key, value)

#define FB_LOG_KV_LIST(...)                                                        \
    ::fastblock::test::structured_logger::format_kv_list({__VA_ARGS__})

// ============================================================================
// Callback Management
// ============================================================================

/**
 * @brief Callback tracker for testing async callbacks
 */
class callback_tracker {
public:
    static callback_tracker& instance() {
        static callback_tracker tracker;
        return tracker;
    }

    void record_call(const std::string& name) {
        _call_counts[name]++;
    }

    void record_call_with_args(const std::string& name, const std::string& args) {
        _call_counts[name]++;
        _call_args[name].push_back(args);
    }

    size_t call_count(const std::string& name) const {
        auto it = _call_counts.find(name);
        return it != _call_counts.end() ? it->second : 0;
    }

    std::vector<std::string> call_args(const std::string& name) const {
        auto it = _call_args.find(name);
        return it != _call_args.end() ? it->second : std::vector<std::string>{};
    }

    bool was_called(const std::string& name) const {
        return call_count(name) > 0;
    }

    void reset() {
        _call_counts.clear();
        _call_args.clear();
    }

    void reset(const std::string& name) {
        _call_counts.erase(name);
        _call_args.erase(name);
    }

private:
    callback_tracker() = default;
    std::map<std::string, size_t> _call_counts;
    std::map<std::string, std::vector<std::string>> _call_args;
};

/**
 * @brief RAII callback guard for automatic cleanup
 */
class callback_guard {
public:
    callback_guard(const std::string& name) : _name(name) {}

    void operator()() {
        callback_tracker::instance().record_call(_name);
    }

    template<typename T>
    void operator()(const T& arg) {
        std::stringstream ss;
        ss << arg;
        callback_tracker::instance().record_call_with_args(_name, ss.str());
    }

private:
    std::string _name;
};

#define FB_CALLBACK_RECORD(name)                                                    \
    ::fastblock::test::callback_tracker::instance().record_call(name)

#define FB_CALLBACK_RECORD_ARGS(name, args)                                        \
    ::fastblock::test::callback_tracker::instance().record_call_with_args(name, args)

#define FB_CALLBACK_COUNT(name)                                                     \
    ::fastblock::test::callback_tracker::instance().call_count(name)

#define FB_CALLBACK_WAS_CALLED(name)                                               \
    ::fastblock::test::callback_tracker::instance().was_called(name)

#define FB_CALLBACK_RESET()                                                         \
    ::fastblock::test::callback_tracker::instance().reset()

#define FB_CALLBACK_GUARD(name)                                                     \
    ::fastblock::test::callback_guard(name)

/**
 * @brief Callback verifier for testing expectations
 */
class callback_verifier {
public:
    static bool verify_called(const std::string& name, size_t expected_count = 1) {
        return callback_tracker::instance().call_count(name) == expected_count;
    }

    static bool verify_not_called(const std::string& name) {
        return !callback_tracker::instance().was_called(name);
    }

    static bool verify_called_at_least(const std::string& name, size_t min_count) {
        return callback_tracker::instance().call_count(name) >= min_count;
    }

    static bool verify_called_at_most(const std::string& name, size_t max_count) {
        return callback_tracker::instance().call_count(name) <= max_count;
    }

    static bool verify_args(const std::string& name, size_t call_index, const std::string& expected) {
        auto args = callback_tracker::instance().call_args(name);
        if (call_index >= args.size()) return false;
        return args[call_index] == expected;
    }
};

#define FB_VERIFY_CALLED(name, count)                                               \
    ::fastblock::test::callback_verifier::verify_called(name, count)

#define FB_VERIFY_NOT_CALLED(name)                                                  \
    ::fastblock::test::callback_verifier::verify_not_called(name)

#define FB_VERIFY_CALLED_AT_LEAST(name, min_count)                                  \
    ::fastblock::test::callback_verifier::verify_called_at_least(name, min_count)

#define FB_VERIFY_CALLED_AT_MOST(name, max_count)                                   \
    ::fastblock::test::callback_verifier::verify_called_at_most(name, max_count)

#define FB_VERIFY_ARGS(name, idx, expected)                                         \
    ::fastblock::test::callback_verifier::verify_args(name, idx, expected)

// ============================================================================
// State Machine Testing
// ============================================================================

/**
 * @brief State machine verifier for testing state transitions
 */
template<typename StateType>
class state_machine_verifier {
public:
    state_machine_verifier(StateType initial_state)
        : _current_state(initial_state), _transition_count(0) {}

    void transition(StateType new_state) {
        _history.push_back({_current_state, new_state});
        _current_state = new_state;
        _transition_count++;
    }

    StateType current_state() const { return _current_state; }
    size_t transition_count() const { return _transition_count; }

    bool was_state(StateType state) const {
        for (const auto& t : _history) {
            if (t.from == state || t.to == state) return true;
        }
        return _current_state == state;
    }

    bool transition_occurred(StateType from, StateType to) const {
        for (const auto& t : _history) {
            if (t.from == from && t.to == to) return true;
        }
        return false;
    }

    size_t count_transitions(StateType from, StateType to) const {
        size_t count = 0;
        for (const auto& t : _history) {
            if (t.from == from && t.to == to) count++;
        }
        return count;
    }

    const std::vector<std::pair<StateType, StateType>>& history() const {
        return _history;
    }

    void reset(StateType initial_state) {
        _current_state = initial_state;
        _history.clear();
        _transition_count = 0;
    }

private:
    struct transition {
        StateType from;
        StateType to;
    };

    StateType _current_state;
    std::vector<std::pair<StateType, StateType>> _history;
    size_t _transition_count;
};

#define FB_STATE_MACHINE(state_type)                                               \
    ::fastblock::test::state_machine_verifier<state_type>

#define FB_STATE_TRANSITION(sm, new_state)                                         \
    sm.transition(new_state)

#define FB_STATE_CURRENT(sm)                                                       \
    sm.current_state()

#define FB_STATE_WAS(sm, state)                                                     \
    sm.was_state(state)

#define FB_STATE_TRANSITION_OCCURRED(sm, from, to)                                 \
    sm.transition_occurred(from, to)

#define FB_STATE_COUNT_TRANSITIONS(sm, from, to)                                   \
    sm.count_transitions(from, to)

#define FB_STATE_HISTORY(sm)                                                       \
    sm.history()

#define FB_STATE_RESET(sm, initial_state)                                          \
    sm.reset(initial_state)

/**
 * @brief State machine expectation builder
 */
template<typename StateType>
class state_expectation {
public:
    explicit state_expectation(StateType expected_state)
        : _expected(expected_state) {}

    bool verify(const state_machine_verifier<StateType>& sm) const {
        return sm.current_state() == _expected;
    }

    StateType expected() const { return _expected; }

private:
    StateType _expected;
};

template<typename StateType>
state_expectation<StateType> expect_state(StateType state) {
    return state_expectation<StateType>(state);
}

#define FB_EXPECT_STATE(state)                                                      \
    ::fastblock::test::expect_state(state)

// ============================================================================
// File Testing Utilities
// ============================================================================

/**
 * @brief File testing utilities
 */
class file_tester {
public:
    static bool file_exists(const std::string& path) {
        std::ifstream f(path);
        return f.good();
    }

    static bool file_readable(const std::string& path) {
        std::ifstream f(path);
        return f.is_open();
    }

    static bool file_writable(const std::string& path) {
        std::ofstream f(path, std::ios::app);
        return f.is_open();
    }

    static size_t file_size(const std::string& path) {
        std::ifstream f(path, std::ifstream::ate | std::ifstream::binary);
        return f.tellg();
    }

    static std::string file_content(const std::string& path) {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static bool file_contains(const std::string& path, const std::string& content) {
        std::string file_content_str = file_content(path);
        return file_content_str.find(content) != std::string::npos;
    }

    static bool file_starts_with(const std::string& path, const std::string& prefix) {
        std::ifstream f(path);
        std::string first_line;
        std::getline(f, first_line);
        return first_line.substr(0, prefix.length()) == prefix;
    }

    static bool file_ends_with(const std::string& path, const std::string& suffix) {
        std::string content = file_content(path);
        if (content.length() < suffix.length()) return false;
        return content.substr(content.length() - suffix.length()) == suffix;
    }

    static bool directory_exists(const std::string& path) {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
    }

    static bool create_temp_file(const std::string& path, const std::string& content) {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << content;
        return true;
    }

    static bool delete_file(const std::string& path) {
        return std::remove(path.c_str()) == 0;
    }

    static bool copy_file(const std::string& src, const std::string& dst) {
        std::ifstream in(src, std::ios::binary);
        std::ofstream out(dst, std::ios::binary);
        if (!in.is_open() || !out.is_open()) return false;
        out << in.rdbuf();
        return true;
    }

    static std::vector<std::string> list_directory(const std::string& path) {
        std::vector<std::string> files;
        DIR* dir = opendir(path.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (entry->d_name[0] != '.') {
                    files.push_back(entry->d_name);
                }
            }
            closedir(dir);
        }
        return files;
    }
};

#define FB_FILE_EXISTS(path)           ::fastblock::test::file_tester::file_exists(path)
#define FB_FILE_READABLE(path)         ::fastblock::test::file_tester::file_readable(path)
#define FB_FILE_WRITABLE(path)         ::fastblock::test::file_tester::file_writable(path)
#define FB_FILE_SIZE(path)             ::fastblock::test::file_tester::file_size(path)
#define FB_FILE_CONTENT(path)          ::fastblock::test::file_tester::file_content(path)
#define FB_FILE_CONTAINS(path, content)                                            \
    ::fastblock::test::file_tester::file_contains(path, content)
#define FB_FILE_STARTS_WITH(path, prefix)                                          \
    ::fastblock::test::file_tester::file_starts_with(path, prefix)
#define FB_FILE_ENDS_WITH(path, suffix)                                            \
    ::fastblock::test::file_tester::file_ends_with(path, suffix)
#define FB_DIR_EXISTS(path)            ::fastblock::test::file_tester::directory_exists(path)
#define FB_CREATE_TEMP_FILE(path, content)                                         \
    ::fastblock::test::file_tester::create_temp_file(path, content)
#define FB_DELETE_FILE(path)           ::fastblock::test::file_tester::delete_file(path)
#define FB_COPY_FILE(src, dst)        ::fastblock::test::file_tester::copy_file(src, dst)
#define FB_LIST_DIR(path)             ::fastblock::test::file_tester::list_directory(path)

/**
 * @brief Temporary file guard for automatic cleanup
 */
class temp_file_guard {
public:
    temp_file_guard(const std::string& path) : _path(path) {}

    ~temp_file_guard() {
        if (!_released) {
            file_tester::delete_file(_path);
        }
    }

    const std::string& path() const { return _path; }

    void release() { _released = true; }

private:
    std::string _path;
    bool _released = false;
};

#define FB_TEMP_FILE_GUARD(path)                                                    \
    ::fastblock::test::temp_file_guard(path)

// ============================================================================
// String Testing Utilities
// ============================================================================

/**
 * @brief String testing utilities
 */
class string_tester {
public:
    static bool starts_with(const std::string& str, const std::string& prefix) {
        if (prefix.length() > str.length()) return false;
        return str.substr(0, prefix.length()) == prefix;
    }

    static bool ends_with(const std::string& str, const std::string& suffix) {
        if (suffix.length() > str.length()) return false;
        return str.substr(str.length() - suffix.length()) == suffix;
    }

    static bool contains(const std::string& str, const std::string& substr) {
        return str.find(substr) != std::string::npos;
    }

    static bool matches_pattern(const std::string& str, const std::string& pattern) {
        // Simple wildcard matching: * matches any sequence
        size_t str_idx = 0, pat_idx = 0;
        size_t str_len = str.length(), pat_len = pattern.length();

        while (pat_idx < pat_len) {
            if (pattern[pat_idx] == '*') {
                pat_idx++;
                if (pat_idx == pat_len) return true;
                while (str_idx < str_len && str[str_idx] != pattern[pat_idx]) {
                    str_idx++;
                }
            } else {
                if (str_idx >= str_len || str[str_idx] != pattern[pat_idx]) {
                    return false;
                }
                str_idx++;
                pat_idx++;
            }
        }
        return str_idx == str_len;
    }

    static bool is_empty(const std::string& str) {
        return str.empty();
    }

    static bool is_blank(const std::string& str) {
        return str.empty() || str.find_first_not_of(" \t\n\r") == std::string::npos;
    }

    static bool is_numeric(const std::string& str) {
        if (str.empty()) return false;
        for (char c : str) {
            if (!std::isdigit(c)) return false;
        }
        return true;
    }

    static bool is_alphabetic(const std::string& str) {
        if (str.empty()) return false;
        for (char c : str) {
            if (!std::isalpha(c)) return false;
        }
        return true;
    }

    static bool is_alphanumeric(const std::string& str) {
        if (str.empty()) return false;
        for (char c : str) {
            if (!std::isalnum(c)) return false;
        }
        return true;
    }

    static bool is_lower(const std::string& str) {
        for (char c : str) {
            if (std::isalpha(c) && !std::islower(c)) return false;
        }
        return true;
    }

    static bool is_upper(const std::string& str) {
        for (char c : str) {
            if (std::isalpha(c) && !std::isupper(c)) return false;
        }
        return true;
    }

    static std::string trim(const std::string& str) {
        size_t start = str.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        size_t end = str.find_last_not_of(" \t\n\r");
        return str.substr(start, end - start + 1);
    }

    static std::string to_lower(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }

    static std::string to_upper(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return result;
    }

    static std::vector<std::string> split(const std::string& str, char delimiter) {
        std::vector<std::string> tokens;
        std::stringstream ss(str);
        std::string token;
        while (std::getline(ss, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }

    static std::string join(const std::vector<std::string>& parts, const std::string& delimiter) {
        std::stringstream ss;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) ss << delimiter;
            ss << parts[i];
        }
        return ss.str();
    }

    static std::string replace(const std::string& str, const std::string& from, const std::string& to) {
        std::string result = str;
        size_t pos = 0;
        while ((pos = result.find(from, pos)) != std::string::npos) {
            result.replace(pos, from.length(), to);
            pos += to.length();
        }
        return result;
    }

    static size_t count_occurrences(const std::string& str, const std::string& substr) {
        size_t count = 0;
        size_t pos = 0;
        while ((pos = str.find(substr, pos)) != std::string::npos) {
            count++;
            pos += substr.length();
        }
        return count;
    }
};

#define FB_STR_STARTS_WITH(str, prefix)                                            \
    ::fastblock::test::string_tester::starts_with(str, prefix)

#define FB_STR_ENDS_WITH(str, suffix)                                              \
    ::fastblock::test::string_tester::ends_with(str, suffix)

#define FB_STR_CONTAINS(str, substr)                                               \
    ::fastblock::test::string_tester::contains(str, substr)

#define FB_STR_MATCHES(str, pattern)                                               \
    ::fastblock::test::string_tester::matches_pattern(str, pattern)

#define FB_STR_IS_EMPTY(str)                                                       \
    ::fastblock::test::string_tester::is_empty(str)

#define FB_STR_IS_BLANK(str)                                                       \
    ::fastblock::test::string_tester::is_blank(str)

#define FB_STR_IS_NUMERIC(str)                                                     \
    ::fastblock::test::string_tester::is_numeric(str)

#define FB_STR_IS_ALPHA(str)                                                       \
    ::fastblock::test::string_tester::is_alphabetic(str)

#define FB_STR_IS_ALNUM(str)                                                       \
    ::fastblock::test::string_tester::is_alphanumeric(str)

#define FB_STR_IS_LOWER(str)                                                       \
    ::fastblock::test::string_tester::is_lower(str)

#define FB_STR_IS_UPPER(str)                                                       \
    ::fastblock::test::string_tester::is_upper(str)

#define FB_STR_TRIM(str)                                                           \
    ::fastblock::test::string_tester::trim(str)

#define FB_STR_TO_LOWER(str)                                                       \
    ::fastblock::test::string_tester::to_lower(str)

#define FB_STR_TO_UPPER(str)                                                       \
    ::fastblock::test::string_tester::to_upper(str)

#define FB_STR_SPLIT(str, delim)                                                   \
    ::fastblock::test::string_tester::split(str, delim)

#define FB_STR_JOIN(parts, delim)                                                  \
    ::fastblock::test::string_tester::join(parts, delim)

#define FB_STR_REPLACE(str, from, to)                                              \
    ::fastblock::test::string_tester::replace(str, from, to)

#define FB_STR_COUNT(str, substr)                                                  \
    ::fastblock::test::string_tester::count_occurrences(str, substr)

// ============================================================================
// Test Discovery and Filtering
// ============================================================================

/**
 * @brief Test matcher for selective test execution
 *
 * Note: This is different from test_filter in test_config.h which stores
 * filter criteria. This class provides matching operations.
 */
class test_matcher {
public:
    enum class match_mode {
        EXACT,       // Exact name match
        PREFIX,      // Starts with pattern
        SUFFIX,      // Ends with pattern
        CONTAINS,    // Contains pattern
        REGEX        // Regular expression match
    };

    static bool matches(const std::string& name, const std::string& pattern, match_mode mode) {
        switch (mode) {
            case match_mode::EXACT:
                return name == pattern;
            case match_mode::PREFIX:
                return name.substr(0, pattern.length()) == pattern;
            case match_mode::SUFFIX:
                if (pattern.length() > name.length()) return false;
                return name.substr(name.length() - pattern.length()) == pattern;
            case match_mode::CONTAINS:
                return name.find(pattern) != std::string::npos;
            case match_mode::REGEX:
                try {
                    std::regex re(pattern);
                    return std::regex_search(name, re);
                } catch (...) {
                    return false;
                }
            default:
                return false;
        }
    }

    static std::vector<std::shared_ptr<test_case>> filter_tests(
        const std::vector<std::shared_ptr<test_case>>& tests,
        const std::string& pattern,
        match_mode mode) {
        std::vector<std::shared_ptr<test_case>> result;
        for (const auto& tc : tests) {
            if (matches(tc->name(), pattern, mode)) {
                result.push_back(tc);
            }
        }
        return result;
    }

    static std::vector<std::shared_ptr<test_case>> filter_by_suite(
        const std::vector<std::shared_ptr<test_case>>& tests,
        const std::string& suite_name) {
        std::vector<std::shared_ptr<test_case>> result;
        for (const auto& tc : tests) {
            if (tc->suite() == suite_name) {
                result.push_back(tc);
            }
        }
        return result;
    }

    static std::vector<std::shared_ptr<test_case>> filter_by_tag(
        const std::vector<std::shared_ptr<test_case>>& tests,
        test_tag tag) {
        std::vector<std::shared_ptr<test_case>> result;
        for (const auto& tc : tests) {
            auto tagged = std::dynamic_pointer_cast<tagged_test_case>(tc);
            if (tagged && tagged->tag() == tag) {
                result.push_back(tc);
            }
        }
        return result;
    }
};

#define FB_FILTER_EXACT(name, pattern)                                              \
    ::fastblock::test::test_matcher::matches(name, pattern,                          \
        ::fastblock::test::test_matcher::match_mode::EXACT)

#define FB_FILTER_PREFIX(name, pattern)                                             \
    ::fastblock::test::test_matcher::matches(name, pattern,                          \
        ::fastblock::test::test_matcher::match_mode::PREFIX)

#define FB_FILTER_SUFFIX(name, pattern)                                             \
    ::fastblock::test::test_matcher::matches(name, pattern,                          \
        ::fastblock::test::test_matcher::match_mode::SUFFIX)

#define FB_FILTER_CONTAINS(name, pattern)                                           \
    ::fastblock::test::test_matcher::matches(name, pattern,                          \
        ::fastblock::test::test_matcher::match_mode::CONTAINS)

#define FB_FILTER_REGEX(name, pattern)                                              \
    ::fastblock::test::test_matcher::matches(name, pattern,                          \
        ::fastblock::test::test_matcher::match_mode::REGEX)

/**
 * @brief Test selector for building test queries
 */
class test_selector {
public:
    test_selector& select_suite(const std::string& suite_name) {
        _suite_filter = suite_name;
        return *this;
    }

    test_selector& select_name(const std::string& pattern, test_matcher::match_mode mode) {
        _name_pattern = pattern;
        _name_mode = mode;
        return *this;
    }

    test_selector& select_tag(test_tag tag) {
        _tag_filter = tag;
        _use_tag = true;
        return *this;
    }

    test_selector& exclude(const std::string& pattern) {
        _excludes.push_back(pattern);
        return *this;
    }

    std::vector<std::shared_ptr<test_case>> apply() const;

private:
    std::string _suite_filter;
    std::string _name_pattern;
    test_matcher::match_mode _name_mode = test_matcher::match_mode::EXACT;
    test_tag _tag_filter = test_tag::NONE;
    bool _use_tag = false;
    std::vector<std::string> _excludes;
};

#define FB_SELECT()                                                                 \
    ::fastblock::test::test_selector()

// ============================================================================
// Test Report Generation
// ============================================================================

/**
 * @brief Report format enumeration
 */
enum class report_format {
    TEXT,
    JSON,
    JUNIT_XML,
    TAP
};

/**
 * @brief Test report generator
 */
class report_generator {
public:
    static std::string generate_junit_xml(
        const std::vector<test_result>& results,
        const std::string& suite_name = "test_suite") {

        std::stringstream ss;
        ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        ss << "<testsuite name=\"" << escape_xml(suite_name) << "\" ";

        int tests = 0, failures = 0, errors = 0, skipped = 0;
        std::stringstream testcases;

        for (const auto& r : results) {
            tests++;
            testcases << "  <testcase name=\"" << escape_xml(r.test_name)
                      << "\" classname=\"" << escape_xml(r.suite_name) << "\"";

            if (r.duration.count() > 0) {
                testcases << " time=\"" << (r.duration.count() / 1000000.0) << "\"";
            }

            if (r.status == test_status::FAILED) {
                failures++;
                testcases << ">\n";
                testcases << "    <failure message=\"" << escape_xml(r.message)
                          << "\">\n";
                testcases << "      " << escape_xml(r.file) << ":" << r.line << "\n";
                testcases << "    </failure>\n";
                testcases << "  </testcase>\n";
            } else if (r.status == test_status::SKIPPED) {
                skipped++;
                testcases << ">\n";
                testcases << "    <skipped message=\"" << escape_xml(r.skip_reason)
                          << "\"/>\n";
                testcases << "  </testcase>\n";
            } else {
                testcases << "/>\n";
            }
        }

        ss << "tests=\"" << tests << "\" "
           << "failures=\"" << failures << "\" "
           << "errors=\"" << errors << "\" "
           << "skipped=\"" << skipped << "\">\n";
        ss << testcases.str();
        ss << "</testsuite>\n";

        return ss.str();
    }

    static std::string generate_json(
        const std::vector<test_result>& results,
        const std::string& suite_name = "test_suite") {

        std::stringstream ss;
        ss << "{\n";
        ss << "  \"suite\": \"" << escape_json(suite_name) << "\",\n";
        ss << "  \"results\": [\n";

        bool first = true;
        for (const auto& r : results) {
            if (!first) ss << ",\n";
            first = false;

            ss << "    {\n";
            ss << "      \"name\": \"" << escape_json(r.test_name) << "\",\n";
            ss << "      \"suite\": \"" << escape_json(r.suite_name) << "\",\n";
            ss << "      \"status\": \"" << test_status_str(r.status) << "\",\n";
            ss << "      \"duration_ms\": " << (r.duration.count() / 1000.0) << ",\n";

            if (!r.message.empty()) {
                ss << "      \"message\": \"" << escape_json(r.message) << "\",\n";
            }
            if (!r.file.empty()) {
                ss << "      \"file\": \"" << escape_json(r.file) << "\",\n";
                ss << "      \"line\": " << r.line << "\n";
            }
            ss << "    }";
        }

        ss << "\n  ]\n";
        ss << "}\n";

        return ss.str();
    }

    static std::string generate_tap(
        const std::vector<test_result>& results) {

        std::stringstream ss;
        ss << "TAP version 13\n";
        ss << "1.." << results.size() << "\n";

        int i = 1;
        for (const auto& r : results) {
            if (r.status == test_status::PASSED) {
                ss << "ok " << i << " - " << r.test_name << "\n";
            } else if (r.status == test_status::SKIPPED) {
                ss << "ok " << i << " - " << r.test_name
                   << " # SKIP " << r.skip_reason << "\n";
            } else {
                ss << "not ok " << i << " - " << r.test_name << "\n";
                if (!r.message.empty()) {
                    ss << "  ---\n";
                    ss << "  message: " << r.message << "\n";
                    ss << "  ---\n";
                }
            }
            i++;
        }

        return ss.str();
    }

    static std::string generate_text(
        const std::vector<test_result>& results) {

        int total = results.size();
        int passed = 0, failed = 0, skipped = 0;

        for (const auto& r : results) {
            if (r.status == test_status::PASSED) passed++;
            else if (r.status == test_status::FAILED) failed++;
            else if (r.status == test_status::SKIPPED) skipped++;
        }

        std::stringstream ss;
        ss << "Test Results:\n";
        ss << "  Total:   " << total << "\n";
        ss << "  Passed:  " << passed << "\n";
        ss << "  Failed:  " << failed << "\n";
        ss << "  Skipped: " << skipped << "\n\n";

        if (failed > 0) {
            ss << "Failures:\n";
            for (const auto& r : results) {
                if (r.status == test_status::FAILED) {
                    ss << "  - " << r.suite_name << "." << r.test_name;
                    if (!r.file.empty()) {
                        ss << " (" << r.file << ":" << r.line << ")";
                    }
                    ss << "\n";
                    ss << "    " << r.message << "\n";
                }
            }
        }

        return ss.str();
    }

    static bool write_to_file(const std::string& content, const std::string& path) {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << content;
        return true;
    }

private:
    static std::string escape_xml(const std::string& s) {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '&': result += "&amp;"; break;
                case '<': result += "&lt;"; break;
                case '>': result += "&gt;"; break;
                case '"': result += "&quot;"; break;
                case '\'': result += "&apos;"; break;
                default: result += c;
            }
        }
        return result;
    }

    static std::string escape_json(const std::string& s) {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c;
            }
        }
        return result;
    }
};

#define FB_REPORT_JUNIT_XML(results, suite)                                       \
    ::fastblock::test::report_generator::generate_junit_xml(results, suite)

#define FB_REPORT_JSON(results, suite)                                            \
    ::fastblock::test::report_generator::generate_json(results, suite)

#define FB_REPORT_TAP(results)                                                     \
    ::fastblock::test::report_generator::generate_tap(results)

#define FB_REPORT_TEXT(results)                                                    \
    ::fastblock::test::report_generator::generate_text(results)

#define FB_REPORT_WRITE(content, path)                                             \
    ::fastblock::test::report_generator::write_to_file(content, path)
