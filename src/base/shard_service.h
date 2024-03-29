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
        uint32_t count = core_sharded::get_core_sharded().count();
        uint32_t this_shard_id = core_sharded::get_core_sharded().this_shard_id();
        _instances.resize(count);

        for (uint32_t shard = 0; shard < count; shard++) {
            if (shard == this_shard_id) {
                _instances[shard] = new Service(std::forward<Args>(args)...);
                continue;
            }
            core_sharded::get_core_sharded().invoke_on(shard, 
              [this, shard](Args... args){
                  _instances[shard] = new Service(std::forward<Args>(args)...);
              },
              std::forward<Args>(args)...);
        }
    }

    void stop(){
        uint32_t count = _instances.size();
        for (uint32_t shard = 0; shard < count; shard++){
            delete _instances[shard];
            _instances[shard] = nullptr;
        }
        _instances.clear();
    }

    Service& local() noexcept {
        uint32_t shard = core_sharded::get_core_sharded().this_shard_id();
        return *_instances[shard];
    }

    // 一个线程不安全的方法，试图访问其他核使用的对象。
    // 初始化时会有这种需求：0核修改数据，其他核使用，但用户要保证0核修改的时候，其他核不会同时访问
    Service& on_shard(uint32_t shard) noexcept {
        return *_instances[shard];
    }

    size_t size() { return _instances.size(); }

private:
    std::vector<Service*> _instances;
};