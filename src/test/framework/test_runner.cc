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

#include "test_framework.h"
#include <iostream>
#include <iomanip>
#include <regex>
#include <algorithm>

namespace fastblock {
namespace test {

test_result test_case::execute(test_context& ctx) {
    auto start = std::chrono::high_resolution_clock::now();
    test_result result(_name, _suite, test_status::PENDING);
    result.severity = _severity;

    _status = test_status::RUNNING;
    SPDK_NOTICELOG("=== Running test: %s.%s ===\n", _suite.c_str(), _name.c_str());

    try {
        _func(ctx);

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        if (ctx.skipped()) {
            _status = test_status::SKIPPED;
            result.status = test_status::SKIPPED;
            result.message = ctx.skip_reason();
        } else if (ctx.failed()) {
            _status = test_status::FAILED;
            result.status = test_status::FAILED;
            result.message = ctx.fail_message();
            result.file = ctx.fail_file();
            result.line = ctx.fail_line();
        } else {
            _status = test_status::PASSED;
            result.status = test_status::PASSED;
        }
    } catch (const std::exception& e) {
        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        _status = test_status::FAILED;
        result.status = test_status::FAILED;
        result.message = std::string("Exception: ") + e.what();
        result.file = __FILE__;
        result.line = __LINE__;
    } catch (...) {
        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        _status = test_status::FAILED;
        result.status = test_status::FAILED;
        result.message = "Unknown exception";
        result.file = __FILE__;
        result.line = __LINE__;
    }

    SPDK_NOTICELOG("=== Test %s.%s: %s (duration: %lu us) ===\n",
                   _suite.c_str(), _name.c_str(),
                   test_status_str(_status), result.duration.count());

    return result;
}

test_runner::summary test_runner::run_all() {
    summary s;
    _results.clear();

    SPDK_NOTICELOG("\n========================================\n");
    SPDK_NOTICELOG("Running all tests\n");
    SPDK_NOTICELOG("========================================\n");

    for (auto& suite : test_registry::instance().suites()) {
        suite->run_setup();

        for (auto& tc : suite->tests()) {
            test_context ctx(*tc);
            auto result = tc->execute(ctx);
            _results.push_back(result);

            s.total++;
            s.total_duration += result.duration;

            switch (result.status) {
                case test_status::PASSED:
                    s.passed++;
                    break;
                case test_status::FAILED:
                    s.failed++;
                    // Stop if critical test fails
                    if (tc->severity() == test_severity::CRITICAL) {
                        suite->run_teardown();
                        SPDK_ERRLOG("Critical test failed, stopping execution\n");
                        return s;
                    }
                    break;
                case test_status::SKIPPED:
                    s.skipped++;
                    break;
                default:
                    break;
            }
        }

        suite->run_teardown();
    }

    return s;
}

test_runner::summary test_runner::run_suite(const std::string& suite_name) {
    summary s;
    _results.clear();

    bool found = false;
    for (auto& suite : test_registry::instance().suites()) {
        if (suite->name() == suite_name) {
            found = true;
            suite->run_setup();

            for (auto& tc : suite->tests()) {
                test_context ctx(*tc);
                auto result = tc->execute(ctx);
                _results.push_back(result);

                s.total++;
                s.total_duration += result.duration;

                switch (result.status) {
                    case test_status::PASSED:
                        s.passed++;
                        break;
                    case test_status::FAILED:
                        s.failed++;
                        if (tc->severity() == test_severity::CRITICAL) {
                            suite->run_teardown();
                            return s;
                        }
                        break;
                    case test_status::SKIPPED:
                        s.skipped++;
                        break;
                    default:
                        break;
                }
            }

            suite->run_teardown();
            break;
        }
    }

    if (!found) {
        SPDK_ERRLOG("Suite '%s' not found\n", suite_name.c_str());
    }

    return s;
}

test_runner::summary test_runner::run_matching(const std::string& pattern) {
    summary s;
    _results.clear();

    std::regex re(pattern);
    SPDK_NOTICELOG("Running tests matching pattern: %s\n", pattern.c_str());

    for (auto& suite : test_registry::instance().suites()) {
        bool suite_matched = false;

        for (auto& tc : suite->tests()) {
            std::string full_name = suite->name() + "." + tc->name();
            if (std::regex_search(full_name, re)) {
                suite_matched = true;

                if (!suite_matched) {
                    suite->run_setup();
                }

                test_context ctx(*tc);
                auto result = tc->execute(ctx);
                _results.push_back(result);

                s.total++;
                s.total_duration += result.duration;

                switch (result.status) {
                    case test_status::PASSED:
                        s.passed++;
                        break;
                    case test_status::FAILED:
                        s.failed++;
                        if (tc->severity() == test_severity::CRITICAL) {
                            suite->run_teardown();
                            return s;
                        }
                        break;
                    case test_status::SKIPPED:
                        s.skipped++;
                        break;
                    default:
                        break;
                }
            }
        }

        if (suite_matched) {
            suite->run_teardown();
        }
    }

    return s;
}

void test_runner::print_results() const {
    std::cout << "\n========================================\n";
    std::cout << "Test Results Summary\n";
    std::cout << "========================================\n\n";

    // Print detailed results
    for (const auto& result : _results) {
        std::cout << std::left << std::setw(20) << result.suite_name
                  << "." << std::setw(30) << result.test_name
                  << " [" << test_status_str(result.status) << "]";

        if (result.status == test_status::FAILED) {
            std::cout << " at " << result.file << ":" << result.line;
        }

        std::cout << " (" << result.duration.count() / 1000.0 << " ms)\n";

        if (!result.message.empty()) {
            std::cout << "    Message: " << result.message << "\n";
        }
    }

    std::cout << "\n========================================\n";

    // Calculate summary
    int total = _results.size();
    int passed = 0, failed = 0, skipped = 0;
    for (const auto& r : _results) {
        switch (r.status) {
            case test_status::PASSED: passed++; break;
            case test_status::FAILED: failed++; break;
            case test_status::SKIPPED: skipped++; break;
            default: break;
        }
    }

    std::cout << "Total:   " << total << "\n";
    std::cout << "Passed:  " << passed << " (" << (100.0 * passed / total) << "%)\n";
    std::cout << "Failed:  " << failed << " (" << (100.0 * failed / total) << "%)\n";
    std::cout << "Skipped: " << skipped << " (" << (100.0 * skipped / total) << "%)\n";
    std::cout << "========================================\n";

    if (failed > 0) {
        std::cout << "\n*** FAILED TESTS ***\n";
        for (const auto& r : _results) {
            if (r.status == test_status::FAILED) {
                std::cout << "  - " << r.suite_name << "." << r.test_name
                          << " (" << r.file << ":" << r.line << ")\n";
                std::cout << "    " << r.message << "\n";
            }
        }
    }
}

} // namespace test
} // namespace fastblock