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

} // namespace test
} // namespace fastblock

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
