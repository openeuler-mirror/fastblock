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
 * @file test_harness.h
 * @brief Test harness for running tests with configuration
 */

#pragma once

#include "test_framework.h"
#include "test_config.h"
#include "test_reporter.h"
#include <iostream>
#include <chrono>
#include <memory>

namespace fastblock {
namespace test {

/**
 * @brief Test harness context
 */
struct test_harness_context {
    test_config* config = nullptr;
    test_runner* runner = nullptr;
    test_reporter* reporter = nullptr;
    std::string test_run_name;
};

/**
 * @brief Test harness for running tests
 */
class test_harness {
public:
    test_harness() : _ctx(std::make_unique<test_harness_context>()) {}

    /**
     * @brief Initialize the test harness
     */
    int init(int argc, char* argv[]) {
        // Parse configuration
        if (!test_config::instance().load_from_args(argc, argv)) {
            return 1;
        }
        return 0;
    }

    /**
     * @brief List available tests
     */
    void list_tests() {
        printf("Available test suites:\n\n");

        for (auto& suite : test_registry::instance().suites()) {
            printf("  Suite: %s\n", suite->name().c_str());
            printf("    Tests:\n");
            for (auto& tc : suite->tests()) {
                printf("      - %s [%s]\n", tc->name().c_str(),
                       severity_str(tc->severity()).c_str());
            }
            printf("\n");
        }
    }

    /**
     * @brief Run tests based on configuration
     */
    int run() {
        // Create appropriate reporter
        auto reporter = create_reporter(test_config::instance().format());
        _ctx->reporter = reporter.get();

        // Create runner
        test_runner runner;
        _ctx->runner = &runner;

        // Set test run name
        _ctx->test_run_name = "fastblock_test_run_" +
                              std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

        // Run tests
        auto summary = runner.run_all();

        // Report results
        reporter->report_footer(summary);

        return summary.failed > 0 ? 1 : 0;
    }

    /**
     * @brief Get context
     */
    test_harness_context* context() { return _ctx.get(); }

private:
    std::unique_ptr<test_harness_context> _ctx;

    std::string severity_str(test_severity sev) {
        switch (sev) {
            case test_severity::CRITICAL: return "critical";
            case test_severity::NORMAL: return "normal";
            case test_severity::OPTIONAL: return "optional";
            default: return "unknown";
        }
    }

    std::unique_ptr<test_reporter> create_reporter(output_format format) {
        switch (format) {
            case output_format::JSON:
                return std::make_unique<json_reporter>();
            case output_format::JUNIT_XML:
                return std::make_unique<junit_reporter>();
            case output_format::TAP:
                return std::make_unique<tap_reporter>();
            default:
                return std::make_unique<text_reporter>();
        }
    }
};

/**
 * @brief Global test harness instance
 */
static test_harness g_test_harness;

/**
 * @brief Main entry point for test programs
 */
inline int test_main(int argc, char* argv[]) {
    if (g_test_harness.init(argc, argv) != 0) {
        return 1;
    }
    return g_test_harness.run();
}

} // namespace test
} // namespace fastblock

/**
 * @brief Convenience macro for main function
 */
#define FB_TEST_MAIN()                                                         \
    int main(int argc, char* argv[]) {                                         \
        return ::fastblock::test::test_main(argc, argv);                       \
    }
