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

#include "fastblock/utils/overload.h"

#include <cstdio>
#include <string>
#include <variant>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

int main() {
    using Var = std::variant<int, double, std::string>;

    Var v_int = 42;
    Var v_double = 3.14;
    Var v_string = std::string("hello");

    auto visitor = utils::overload{
        [](int i) -> std::string { return "int:" + std::to_string(i); },
        [](double d) -> std::string { return "double:" + std::to_string(d); },
        [](const std::string &s) -> std::string { return "string:" + s; }
    };

    TEST_ASSERT(std::visit(visitor, v_int) == "int:42", "overload int visit mismatch");
    TEST_ASSERT(std::visit(visitor, v_double).substr(0, 10) == "double:3.1", "overload double visit mismatch");
    TEST_ASSERT(std::visit(visitor, v_string) == "string:hello", "overload string visit mismatch");

    // Test with const char* variant
    using Var2 = std::variant<int, const char *>;
    Var2 v2 = "world";
    auto v2_result = std::visit(utils::overload{
        [](int i) { return std::string("int"); },
        [](const char *s) { return std::string(s); }
    }, v2);
    TEST_ASSERT(v2_result == "world", "overload const char* visit mismatch");

    std::printf("\noverload test results: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
