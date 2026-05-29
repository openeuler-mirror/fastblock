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
 * @file test_config.h
 * @brief Test configuration management
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

namespace fastblock {
namespace test {

/**
 * @brief Test execution mode
 */
enum class execution_mode {
    SEQUENTIAL,     // Run tests one after another
    PARALLEL,       // Run tests in parallel (where possible)
    INTERACTIVE     // Ask user before each test
};

/**
 * @brief Output format for test results
 */
enum class output_format {
    TEXT,           // Plain text output
    JSON,           // JSON format
    JUNIT_XML,      // JUnit XML format for CI integration
    TAP             // Test Anything Protocol
};

/**
 * @brief Test filter criteria
 */
struct test_filter {
    std::string suite_pattern;      // Suite name pattern (regex)
    std::string test_pattern;       // Test name pattern (regex)
    std::string tag;                // Tag to filter by
    test_severity min_severity;     // Minimum severity level
    test_severity max_severity;     // Maximum severity level

    test_filter() : min_severity(test_severity::OPTIONAL),
                    max_severity(test_severity::CRITICAL) {}
};

/**
 * @brief Test configuration
 */
class test_config {
public:
    static test_config& instance() {
        static test_config config;
        return config;
    }

    /**
     * @brief Load configuration from JSON file
     */
    bool load_from_file(const std::string& path) {
        try {
            boost::property_tree::read_json(path, _pt);
            _config_path = path;
            parse_config();
            return true;
        } catch (const std::exception& e) {
            SPDK_ERRLOG("Failed to load config from %s: %s\n", path.c_str(), e.what());
            return false;
        }
    }

    /**
     * @brief Load configuration from command line arguments
     */
    bool load_from_args(int argc, char* argv[]) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "-C" || arg == "--config") {
                if (i + 1 < argc) {
                    return load_from_file(argv[++i]);
                }
            } else if (arg == "-s" || arg == "--suite") {
                if (i + 1 < argc) {
                    _filter.suite_pattern = argv[++i];
                }
            } else if (arg == "-t" || arg == "--test") {
                if (i + 1 < argc) {
                    _filter.test_pattern = argv[++i];
                }
            } else if (arg == "--parallel") {
                _mode = execution_mode::PARALLEL;
            } else if (arg == "--sequential") {
                _mode = execution_mode::SEQUENTIAL;
            } else if (arg == "--format") {
                if (i + 1 < argc) {
                    std::string fmt = argv[++i];
                    if (fmt == "json") _format = output_format::JSON;
                    else if (fmt == "junit") _format = output_format::JUNIT_XML;
                    else if (fmt == "tap") _format = output_format::TAP;
                    else _format = output_format::TEXT;
                }
            } else if (arg == "-v" || arg == "--verbose") {
                _verbose = true;
            } else if (arg == "-q" || arg == "--quiet") {
                _quiet = true;
            } else if (arg == "--list") {
                _list_only = true;
            }
        }
        return true;
    }

    // Getters
    const std::string& config_path() const { return _config_path; }
    execution_mode mode() const { return _mode; }
    output_format format() const { return _format; }
    const test_filter& filter() const { return _filter; }
    bool verbose() const { return _verbose; }
    bool quiet() const { return _quiet; }
    bool list_only() const { return _list_only; }
    uint32_t timeout_seconds() const { return _timeout_seconds; }
    uint32_t parallel_jobs() const { return _parallel_jobs; }

    /**
     * @brief Get test-specific configuration value
     */
    template<typename T>
    T get(const std::string& key, const T& default_value = T{}) const {
        try {
            return _pt.get<T>(key, default_value);
        } catch (...) {
            return default_value;
        }
    }

    /**
     * @brief Get environment-specific test parameters
     */
    const std::map<std::string, std::string>& env_params() const { return _env_params; }

    /**
     * @brief Print usage information
     */
    static void print_usage(const char* program_name) {
        printf("Usage: %s [options]\n", program_name);
        printf("\nOptions:\n");
        printf("  -C, --config <file>    Load configuration from JSON file\n");
        printf("  -s, --suite <pattern>  Run tests from matching suites only\n");
        printf("  -t, --test <pattern>   Run tests matching pattern only\n");
        printf("  --parallel             Run tests in parallel\n");
        printf("  --sequential           Run tests sequentially (default)\n");
        printf("  --format <fmt>         Output format: text, json, junit, tap\n");
        printf("  -v, --verbose          Verbose output\n");
        printf("  -q, --quiet            Quiet mode (errors only)\n");
        printf("  --list                 List tests without running\n");
        printf("  -h, --help             Show this help message\n");
    }

private:
    test_config()
        : _mode(execution_mode::SEQUENTIAL)
        , _format(output_format::TEXT)
        , _verbose(false)
        , _quiet(false)
        , _list_only(false)
        , _timeout_seconds(300)
        , _parallel_jobs(1) {}

    void parse_config() {
        _mode = parse_mode(_pt.get("mode", "sequential"));
        _format = parse_format(_pt.get("format", "text"));
        _verbose = _pt.get("verbose", false);
        _quiet = _pt.get("quiet", false);
        _timeout_seconds = _pt.get("timeout_seconds", 300);
        _parallel_jobs = _pt.get("parallel_jobs", 1);

        // Parse environment parameters
        if (_pt.count("environment") > 0) {
            for (const auto& [key, value] : _pt.get_child("environment")) {
                _env_params[key] = value.get_value<std::string>();
            }
        }

        // Parse filter
        if (_pt.count("filter") > 0) {
            auto& filter_pt = _pt.get_child("filter");
            _filter.suite_pattern = filter_pt.get("suite", "");
            _filter.test_pattern = filter_pt.get("test", "");
            _filter.tag = filter_pt.get("tag", "");
        }
    }

    execution_mode parse_mode(const std::string& mode) {
        if (mode == "parallel") return execution_mode::PARALLEL;
        if (mode == "interactive") return execution_mode::INTERACTIVE;
        return execution_mode::SEQUENTIAL;
    }

    output_format parse_format(const std::string& fmt) {
        if (fmt == "json") return output_format::JSON;
        if (fmt == "junit") return output_format::JUNIT_XML;
        if (fmt == "tap") return output_format::TAP;
        return output_format::TEXT;
    }

    std::string _config_path;
    boost::property_tree::ptree _pt;
    execution_mode _mode;
    output_format _format;
    test_filter _filter;
    bool _verbose;
    bool _quiet;
    bool _list_only;
    uint32_t _timeout_seconds;
    uint32_t _parallel_jobs;
    std::map<std::string, std::string> _env_params;
};

} // namespace test
} // namespace fastblock
