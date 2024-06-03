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
#pragma

#include "utils/fmt.h"

#include <charconv>
#include <memory>
#include <optional>
#include <string_view>

namespace localstore {

struct object_name {

    // for example:
    //  10_test_image_1_1_13237
    //    10: len("test_image")
    //    1: len("1"), "1" for pool_id
    //    13237: seq no, last part of obj name, unnecessary for encoding len

    static constexpr std::string_view connector{"_"};

    static std::string calc_image_object_prefix(const uint64_t pool_id, const std::string& image_name) {
        auto pool_id_str = std::to_string(pool_id);
        return FMT_7("%1%%2%%3%%4%%5%%6%%7%", image_name.size(), connector, image_name, connector, pool_id_str.size(), connector, pool_id_str);
    }

    static std::string get_image_object_name(const std::string& prefix, uint64_t seq_no) {
        return FMT_3("%1%%2%%3%", prefix, connector, seq_no);
    }

    static std::optional<std::tuple<std::string, int64_t>> get_image_pool_name(const std::string_view obj_name) {
        int32_t image_name_len{0};
        auto [ptr, ec] = std::from_chars(obj_name.data(), obj_name.data() + obj_name.size(), image_name_len);

        if (ec != std::errc{} or image_name_len <= 0) {
            return std::nullopt;
        }
        auto image_name = obj_name.substr(ptr - obj_name.data() + 1, image_name_len);

        int32_t pool_id_len{0};
        auto* pool_id_head{ptr + image_name_len + 1};
        auto [pool_ptr, ec] = std::from_chars(pool_id_head, pool_id_head + (obj_name.size() - image_name_len - 1), pool_id_len);

        if (ec != std::errc{} or pool_id_len <= 0) {
            return std::nullopt;
        }
        auto pool_id_string = obj_name.substr(pool_ptr - obj_name.data() + 1, pool_id_len);
        auto pool_id = std::stoll(std::string(pool_id_string));

        return std::make_tuple(std::string(image_name), pool_id);
    }
};

} // namespace localstore
