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
 * @brief Main test harness for running all tests
 *
 * This provides the main entry point for the unified test framework,
 * integrating SPDK initialization, test configuration, and result reporting.
 */

#pragma once

#include "test_framework.h"
#include "test_config.h"
#include "test_reporter.h"

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

namespace fastblock {
namespace test {

/**
 * @brief Test harness context
 */
struct test_harness_context {
    test_config* config;
    test_runner* runner;
    test_reporter* reporter;
    int result;
    std::string test_run_name;

    test_harness_context()
        : config(nullptr)
        , runner(nullptr)
        , reporter(nullptr)
        , result(0) {}
};

/**
 * @brief Test harness class - manages the entire test execution
 */
class test_harness {
public:
    test_harness() : _ctx(new test_harness_context()) {}

    /**
     * @brief Initialize the test harness
     */
    int init(int argc, char* argv[]) {
        // Parse configuration
        _ctx->config = &test_config::instance();
        if (!_ctx->config->load_from_args(argc, argv)) {
            SPDK_ERRLOG("Failed to parse arguments\n");
            return -EINVAL;
        }

        // Initialize SPDK
        struct spdk_app_opts opts = {};
        spdk_app_opts_init(&opts, sizeof(opts));
        opts.name = "fastblock_tests";
        opts.num_entries = 0;  // Disable tracing

        // Set log level based on config
        if (_ctx->config->verbose()) {
            opts.print_level = SPDK_LOG_DEBUG;
        } else if (_ctx->config->quiet()) {
            opts.print_level = SPDK_LOG_ERROR;
        } else {
            opts.print_level = SPDK_LOG_NOTICE;
        }

        // Load SPDK config if provided
        if (_ctx->config->config_path().empty()) {
            // Use default config
            boost::property_tree::ptree pt;
            try {
                boost::property_tree::read_json("conf.json", pt);
                // Apply SPDK config from test config
            } catch (...) {
                SPDK_NOTICELOG("No SPDK config file found, using defaults\n");
            }
        }

        return 0;
    }

    /**
     * @brief List all registered tests
     */
    void list_tests() {
        printf("Available test suites:\n\n");

        for (auto& suite : test_registry::instance().suites()) {
            printf("  Suite: %s\n", suite->name().c_str());
            printf("    Tests:\n");
            for (auto& tc : suite->tests()) {
                printf("      - %s [%s]\n", tc->name().c_str(),
                       severity_str(tc->severity()));
            }
            printf("\n");
        }
    }

    /**
     * @brief Run tests based on configuration
     */
    int run() {
        // Create appropriate reporter
        auto reporter = create_reporter(_ctx->config->format());
        _ctx->reporter = reporter.get();

        // Create runner
        test_runner runner;
        _ctx->runner = &runner;

        // Set test run name
        _ctx->test_run_name = "fastblock_test_run_" +
                              std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

        // Report header
        reporter->report_header(_ctx->test_run_name);

        // Run tests based on filter
        test_runner::summary s;
        const auto& filter = _ctx->config->filter();

        if (!filter.suite_pattern.empty() || !filter.test_pattern.empty()) {
            // Build pattern from suite and test
            std::string pattern;
            if (!filter.suite_pattern.empty() && !filter.test_pattern.empty()) {
                pattern = filter.suite_pattern + "\\." + filter.test_pattern;
            } else if (!filter.suite_pattern.empty()) {
                pattern = filter.suite_pattern + "\\..*";
            } else {
                pattern = ".*/" + filter.test_pattern;
            }
            s = runner.run_matching(pattern);
        } else {
            s = runner.run_all();
        }

        // Report results for each test
        for (const auto& result : runner.results()) {
            reporter->report_test(result);
        }

        // Report footer
        reporter->report_footer(s);

        // Print detailed results if verbose
        if (_ctx->config->verbose()) {
            runner.print_results();
        }

        // Set result code
        _ctx->result = s.failed > 0 ? 1 : 0;

        return _ctx->result;
    }

    /**
     * @brief Get test harness context
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
};

/**
 * @brief Global test harness instance
 */
static test_harness g_test_harness;

/**
 * @brief SPDK application start callback
 */
static void test_app_start(void* arg) {
    auto* harness = static_cast<test_harness*>(arg);

    if (test_config::instance().list_only()) {
        harness->list_tests();
        spdk_app_stop(0);
        return;
    }

    int result = harness->run();
    spdk_app_stop(result);
}

/**
 * @brief Main entry point for test application
 */
static int test_main(int argc, char* argv[]) {
    struct spdk_app_opts opts = {};
    int rc;

    // Check for help
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            test_config::print_usage(argv[0]);
            return 0;
        }
    }

    // Initialize harness
    rc = g_test_harness.init(argc, argv);
    if (rc != 0) {
        return rc;
    }

    // Initialize SPDK
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "fastblock_tests";
    opts.num_entries = 0;

    if (test_config::instance().verbose()) {
        opts.print_level = SPDK_LOG_DEBUG;
    } else if (test_config::instance().quiet()) {
        opts.print_level = SPDK_LOG_ERROR;
    } else {
        opts.print_level = SPDK_LOG_NOTICE;
    }

    // Run tests
    rc = spdk_app_start(&opts, test_app_start, &g_test_harness);
    spdk_app_fini();

    return rc;
}

} // namespace test
} // namespace fastblock

/**
 * @brief Convenience macro for main function
 */
#define FB_TEST_MAIN()                                                         \
    int main(int argc, char* argv[]) {                                          \
        return ::fastblock::test::test_main(argc, argv);                       \
    }