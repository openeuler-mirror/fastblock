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
 * @file test_reporter.h
 * @brief Test result reporting in various formats
 */

#pragma once

#include "test_framework.h"
#include <fstream>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace fastblock {
namespace test {

/**
 * @brief Base class for test reporters
 */
class test_reporter {
public:
    virtual ~test_reporter() = default;

    virtual void report_header(const std::string& test_run_name) = 0;
    virtual void report_test(const test_result& result) = 0;
    virtual void report_footer(const test_runner::summary& s) = 0;

    virtual void output_to_file(const std::string& path) {
        _output_file.open(path);
    }

    std::ostream& out() {
        return _output_file.is_open() ? _output_file : std::cout;
    }

protected:
    std::ofstream _output_file;
};

/**
 * @brief Text format reporter
 */
class text_reporter : public test_reporter {
public:
    void report_header(const std::string& test_run_name) override {
        out() << "\n========================================\n";
        out() << "Test Run: " << test_run_name << "\n";
        out() << "Started: " << current_timestamp() << "\n";
        out() << "========================================\n\n";
    }

    void report_test(const test_result& result) override {
        out() << std::left << std::setw(20) << result.suite_name
              << "." << std::setw(30) << result.test_name
              << " [" << color_status(result.status) << "]";

        if (result.status == test_status::FAILED) {
            out() << " at " << result.file << ":" << result.line;
        }

        out() << " (" << duration_str(result.duration) << ")\n";

        if (!result.message.empty()) {
            out() << "    " << result.message << "\n";
        }
    }

    void report_footer(const test_runner::summary& s) override {
        out() << "\n========================================\n";
        out() << "Summary:\n";
        out() << "  Total:   " << s.total << "\n";
        out() << "  Passed:  " << s.passed << " (" << percentage(s.passed, s.total) << "%)\n";
        out() << "  Failed:  " << s.failed << " (" << percentage(s.failed, s.total) << "%)\n";
        out() << "  Skipped: " << s.skipped << " (" << percentage(s.skipped, s.total) << "%)\n";
        out() << "  Duration: " << duration_str(s.total_duration) << "\n";
        out() << "========================================\n";

        if (s.failed > 0) {
            out() << "\n*** FAILED TESTS ***\n";
        }
    }

private:
    std::string current_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    std::string color_status(test_status status) {
        switch (status) {
            case test_status::PASSED:  return "\033[32mPASSED\033[0m";   // Green
            case test_status::FAILED:  return "\033[31mFAILED\033[0m";   // Red
            case test_status::SKIPPED: return "\033[33mSKIPPED\033[0m";  // Yellow
            default:                   return test_status_str(status);
        }
    }

    std::string duration_str(std::chrono::microseconds us) {
        double ms = us.count() / 1000.0;
        if (ms < 1000) {
            std::stringstream ss;
            ss << std::fixed << std::setprecision(2) << ms << " ms";
            return ss.str();
        }
        double sec = ms / 1000.0;
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << sec << " s";
        return ss.str();
    }

    double percentage(int count, int total) {
        return total > 0 ? (100.0 * count / total) : 0.0;
    }
};

/**
 * @brief JSON format reporter
 */
class json_reporter : public test_reporter {
public:
    void report_header(const std::string& test_run_name) override {
        _json << "{\n";
        _json << "  \"test_run\": \"" << test_run_name << "\",\n";
        _json << "  \"timestamp\": \"" << current_timestamp() << "\",\n";
        _json << "  \"results\": [\n";
        _first_test = true;
    }

    void report_test(const test_result& result) override {
        if (!_first_test) {
            _json << ",\n";
        }
        _first_test = false;

        _json << "    {\n";
        _json << "      \"suite\": \"" << result.suite_name << "\",\n";
        _json << "      \"test\": \"" << result.test_name << "\",\n";
        _json << "      \"status\": \"" << test_status_str(result.status) << "\",\n";
        _json << "      \"duration_us\": " << result.duration.count() << ",\n";
        if (!result.message.empty()) {
            _json << "      \"message\": \"" << escape_json(result.message) << "\",\n";
        }
        if (result.status == test_status::FAILED) {
            _json << "      \"file\": \"" << result.file << "\",\n";
            _json << "      \"line\": " << result.line << ",\n";
        }
        _json << "      \"severity\": \"" << severity_str(result.severity) << "\"\n";
        _json << "    }";
    }

    void report_footer(const test_runner::summary& s) override {
        _json << "\n  ],\n";
        _json << "  \"summary\": {\n";
        _json << "    \"total\": " << s.total << ",\n";
        _json << "    \"passed\": " << s.passed << ",\n";
        _json << "    \"failed\": " << s.failed << ",\n";
        _json << "    \"skipped\": " << s.skipped << ",\n";
        _json << "    \"duration_us\": " << s.total_duration.count() << "\n";
        _json << "  }\n";
        _json << "}\n";

        out() << _json.str();
    }

private:
    std::stringstream _json;
    bool _first_test = true;

    std::string current_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%dT%H:%M:%S");
        return ss.str();
    }

    std::string escape_json(const std::string& s) {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    }

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
 * @brief JUnit XML format reporter (for CI integration)
 */
class junit_reporter : public test_reporter {
public:
    void report_header(const std::string& test_run_name) override {
        _xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        _xml << "<testsuites name=\"" << test_run_name << "\" ";
        _xml << "timestamp=\"" << current_timestamp_iso() << "\">";

        _suite_counts.clear();
        _suite_failures.clear();
        _suite_times.clear();
    }

    void report_test(const test_result& result) override {
        // Track per-suite statistics
        _suite_counts[result.suite_name]++;
        _suite_times[result.suite_name] += result.duration;
        if (result.status == test_status::FAILED) {
            _suite_failures[result.suite_name]++;
        }

        // Store test details
        _tests_by_suite[result.suite_name].push_back(result);
    }

    void report_footer(const test_runner::summary& s) override {
        // Generate XML for each suite
        for (const auto& [suite_name, tests] : _tests_by_suite) {
            double suite_time = _suite_times[suite_name].count() / 1000000.0;
            int suite_count = _suite_counts[suite_name];
            int suite_failures = _suite_failures[suite_name];

            _xml << "\n  <testsuite name=\"" << suite_name << "\" ";
            _xml << "tests=\"" << suite_count << "\" ";
            _xml << "failures=\"" << suite_failures << "\" ";
            _xml << "errors=\"0\" ";
            _xml << "skipped=\"" << (suite_count - tests.size() + suite_failures) << "\" ";
            _xml << "time=\"" << std::fixed << std::setprecision(3) << suite_time << "\">";

            for (const auto& test : tests) {
                double test_time = test.duration.count() / 1000000.0;
                _xml << "\n    <testcase name=\"" << test.test_name << "\" ";
                _xml << "classname=\"" << suite_name << "\" ";
                _xml << "time=\"" << std::fixed << std::setprecision(3) << test_time << "\"";

                if (test.status == test_status::SKIPPED) {
                    _xml << ">\n      <skipped message=\"" << escape_xml(test.message) << "\"/>";
                    _xml << "\n    </testcase>";
                } else if (test.status == test_status::FAILED) {
                    _xml << ">\n      <failure message=\"" << escape_xml(test.message) << "\" ";
                    _xml << "type=\"AssertionFailure\">";
                    _xml << "\n        " << test.file << ":" << test.line;
                    _xml << "\n        " << escape_xml(test.message);
                    _xml << "\n      </failure>";
                    _xml << "\n    </testcase>";
                } else {
                    _xml << "/>";
                }
            }

            _xml << "\n  </testsuite>";
        }

        _xml << "\n</testsuites>\n";

        out() << _xml.str();
    }

private:
    std::stringstream _xml;
    std::map<std::string, int> _suite_counts;
    std::map<std::string, int> _suite_failures;
    std::map<std::string, std::chrono::microseconds> _suite_times;
    std::map<std::string, std::vector<test_result>> _tests_by_suite;

    std::string current_timestamp_iso() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%dT%H:%M:%S");
        return ss.str();
    }

    std::string escape_xml(const std::string& s) {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '<': result += "&lt;"; break;
                case '>': result += "&gt;"; break;
                case '&': result += "&amp;"; break;
                case '"': result += "&quot;"; break;
                case '\'': result += "&apos;"; break;
                default: result += c; break;
            }
        }
        return result;
    }
};

/**
 * @brief TAP (Test Anything Protocol) reporter
 */
class tap_reporter : public test_reporter {
public:
    void report_header(const std::string& /*test_run_name*/) override {
        // TAP format: 1..N at the end, we'll estimate based on registry
    }

    void report_test(const test_result& result) override {
        _tap_lines.push_back(format_tap_line(result));
    }

    void report_footer(const test_runner::summary& s) override {
        // TAP header
        out() << "TAP version 14\n";
        out() << "1.." << s.total << "\n";

        // Test lines
        for (const auto& line : _tap_lines) {
            out() << line << "\n";
        }

        // Summary
        out() << "# tests " << s.total << "\n";
        out() << "# pass  " << s.passed << "\n";
        out() << "# fail  " << s.failed << "\n";
        out() << "# skip  " << s.skipped << "\n";

        if (s.failed > 0) {
            out() << "Bail out! " << s.failed << " tests failed\n";
        }
    }

private:
    std::vector<std::string> _tap_lines;
    int _test_num = 1;

    std::string format_tap_line(const test_result& result) {
        std::stringstream ss;

        switch (result.status) {
            case test_status::PASSED:
                ss << "ok " << _test_num << " - " << result.suite_name << "." << result.test_name;
                break;
            case test_status::FAILED:
                ss << "not ok " << _test_num << " - " << result.suite_name << "." << result.test_name;
                ss << "\n  ---";
                ss << "\n  message: " << result.message;
                ss << "\n  file: " << result.file;
                ss << "\n  line: " << result.line;
                ss << "\n  severity: " << severity_str(result.severity);
                ss << "\n  ...";
                break;
            case test_status::SKIPPED:
                ss << "ok " << _test_num << " - " << result.suite_name << "." << result.test_name;
                ss << " # SKIP " << result.message;
                break;
            default:
                ss << "not ok " << _test_num << " - " << result.suite_name << "." << result.test_name;
                ss << " # unknown status";
                break;
        }

        _test_num++;
        return ss.str();
    }

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
 * @brief Factory function to create appropriate reporter
 */
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

} // namespace test
} // namespace fastblock