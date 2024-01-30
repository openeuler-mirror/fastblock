/* Copyright (c) 2024 ChinaUnicom
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

#include "core_sharded.h"

#include <spdk/util.h>
#include <vector>
#include <tuple>
#include <memory>


template <typename Service>
class sharded {
public:
    template <typename... Args>
    void start(Args&&... args) {
        _instances.resize(core_sharded::get_core_sharded().count());

        for (uint32_t shard = 0; shard < core_sharded::get_core_sharded().count(); shard++) {
            core_sharded::get_core_sharded().invoke_on(shard,
              [this, shard](Args... args){
                  _instances[shard] = new Service(std::forward<Args>(args)...);
              },
              std::forward<Args>(args)...);
        }
    }

    Service& local() noexcept {
        uint32_t shard = core_sharded::get_core_sharded().this_shard_id();
        return *_instances[shard];
    }

private:
    std::vector<Service*> _instances;
};