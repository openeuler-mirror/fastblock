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
#include <future>

namespace fastblock {
namespace test {

test_result test_case::execute(test_context& ctx) {
    auto start = std::chrono::high_resolution_clock::now();
    test_result result(_name, _suite, test_status::PENDING);
    result.severity = _severity;

    _status = test_status::RUNNING;
    std::cout << "=== Running test: " << _suite << "." << _name
              << " (timeout: " << _timeout_seconds << " s) ===" << std::endl;

    // Execute test with timeout checking
    auto future = std::async(std::launch::async, [this, &ctx]() {
        _func(ctx);
    });

    std::future_status status = future.wait_for(std::chrono::seconds(_timeout_seconds));

    auto end = std::chrono::high_resolution_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    if (status == std::future_status::timeout) {
        // Test timed out
        _status = test_status::FAILED;
        result.status = test_status::FAILED;
        result.message = "Test timed out after " + std::to_string(_timeout_seconds) + " seconds";
        result.file = __FILE__;
        result.line = __LINE__;
        std::cerr << "=== Test " << _suite << "." << _name << " TIMEOUT ===" << std::endl;
    } else {
        // Test completed within timeout
        try {
            future.get();  // Get result or throw exception

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
            _status = test_status::FAILED;
            result.status = test_status::FAILED;
            result.message = std::string("Exception: ") + e.what();
            result.file = __FILE__;
            result.line = __LINE__;
        } catch (...) {
            _status = test_status::FAILED;
            result.status = test_status::FAILED;
            result.message = "Unknown exception";
            result.file = __FILE__;
            result.line = __LINE__;
        }
    }

    std::cout << "=== Test " << _suite << "." << _name << ": "
              << test_status_str(_status) << " (duration: " << result.duration.count() << " us) ===" << std::endl;

    return result;
}

test_runner::summary test_runner::run_all() {
    summary s;
    _results.clear();

    std::cout << "\n========================================\n";
    std::cout << "Running all tests\n";
    std::cout << "========================================\n";

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
                        std::cerr << "Critical test failed, stopping execution" << std::endl;;
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
        std::cerr << "Suite '" << suite_name << "' not found" << std::endl;
    }

    return s;
}

test_runner::summary test_runner::run_matching(const std::string& pattern) {
    summary s;
    _results.clear();

    std::regex re(pattern);
    std::cout << "Running tests matching pattern: " << pattern << std::endl;

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

test_runner::summary test_runner::run_by_tag(test_tag tag) {
    summary s;
    _results.clear();

    std::cout << "Running tests with tag: " << static_cast<int>(tag) << std::endl;

    for (auto& suite : test_registry::instance().suites()) {
        bool suite_matched = false;

        for (auto& tc : suite->tests()) {
            // Check if test has the specified tag
            auto tagged = std::dynamic_pointer_cast<tagged_test_case>(tc);
            if (tagged && tagged->tag() == tag) {
                suite_matched = true;

                if (suite_matched) {
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

    if (s.total == 0) {
        std::cout << "No tests found with tag " << static_cast<int>(tag) << std::endl;
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

    ::std::cout << "Total:   " << total << "\n";
    ::std::cout << "Passed:  " << passed << " (" << (100.0 * passed / total) << "%)\n";
    ::std::cout << "Failed:  " << failed << " (" << (100.0 * failed / total) << "%)\n";
    ::std::cout << "Skipped: " << skipped << " (" << (100.0 * skipped / total) << "%)\n";
    ::std::cout << "========================================\n";

    if (failed > 0) {
        ::std::cout << "\n*** FAILED TESTS ***\n";
        for (const auto& r : _results) {
            if (r.status == test_status::FAILED) {
                ::std::cout << "  - " << r.suite_name << "." << r.test_name
                          << " (" << r.file << ":" << r.line << ")\n";
                ::std::cout << "    " << r.message << "\n";
            }
        }
    }
}

} // namespace test
} // namespace fastblock