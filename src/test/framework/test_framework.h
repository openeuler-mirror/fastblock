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
#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <fstream>
#include <atomic>
#include <random>
#include <initializer_list>

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
        auto& suite = get_or_create_suite(suite_name);
        suite.add_test(tc);
    }

    /**
     * @brief Get or create a test suite
     */
    test_suite& get_or_create_suite(const std::string& name) {
        for (auto& s : _suites) {
            if (s->name() == name) {
                return *s;
            }
        }
        auto suite = std::make_shared<test_suite>(name);
        _suites.push_back(suite);
        return *suite;
    }

    /**
     * @brief Get all test suites
     */
    const std::vector<std::shared_ptr<test_suite>>& suites() const { return _suites; }

    /**
     * @brief Clear all registered tests
     */
    void clear() { _suites.clear(); }

private:
    test_registry() = default;
    std::vector<std::shared_ptr<test_suite>> _suites;
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
            std::thread([this, func, &ctx, &async_ctx]() {
                func(ctx, async_ctx);
            }).detach();

            std::unique_lock<std::mutex> lock(async_ctx.mutex);
            if (!async_ctx.cv.wait_for(lock,
                    std::chrono::milliseconds(timeout_ms),
                    [&async_ctx] { return async_ctx.completed; })) {
                async_ctx.timed_out = true;
                ctx.fail("Async test timed out", __FILE__, __LINE__);
            }
            if (!async_ctx.error_message.empty()) {
                ctx.fail(async_ctx.error_message, __FILE__, __LINE__);
            }
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
 */
class random_generator {
public:
    static int random_int(int min_val, int max_val) {
        return min_val + rand() % (max_val - min_val + 1);
    }

    static uint64_t random_uint64(uint64_t min_val, uint64_t max_val) {
        return min_val + ((uint64_t)rand() << 32 | rand()) % (max_val - min_val + 1);
    }

    static std::string random_string(size_t length) {
        static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += chars[rand() % (sizeof(chars) - 1)];
        }
        return result;
    }

    static std::vector<uint8_t> random_bytes(size_t length) {
        std::vector<uint8_t> result(length);
        for (size_t i = 0; i < length; ++i) {
            result[i] = rand() % 256;
        }
        return result;
    }

    static double random_double(double min_val, double max_val) {
        return min_val + (double)rand() / RAND_MAX * (max_val - min_val);
    }

    static bool random_bool() {
        return rand() % 2 == 0;
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
        std::random_shuffle(values.begin(), values.end());
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
