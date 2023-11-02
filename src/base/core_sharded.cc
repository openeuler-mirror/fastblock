/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "core_sharded.h"

std::vector<uint32_t> get_shard_cores(){
    std::vector<uint32_t> shard_cores;
    uint32_t lcore;

    SPDK_ENV_FOREACH_CORE(lcore){
        shard_cores.push_back(lcore);
    }
    return shard_cores;
}