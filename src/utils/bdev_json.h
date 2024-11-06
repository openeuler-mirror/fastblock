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
#pragma once

#include <string.h>
#include <cstdint>
#include <sys/types.h>
#include <unistd.h>

namespace utils {

inline std::string get_bdev_json_file_name(){
    std::string file_name = "/var/tmp/osd_bdev_" + std::to_string(getpid()) + ".json";
    return file_name;
}

void remove_bdev_json_file();

void save_bdev_json(std::string& , const std::string &, const std::string &, const std::string &);

}