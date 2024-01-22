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
 * Copyright (c) 2013, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the raft/LICENSE file.
 */
#include <assert.h>
#include <string>

#include "raft.h"
#include "raft_log.h"

void raft_log::log_load_from_snapshot(raft_index_t idx, raft_term_t term)
{
    log_clear();
}

std::shared_ptr<raft_log> log_new(disk_log* log)
{
    return std::make_shared<raft_log>(log);
}

//used by follower
int raft_log::log_append(std::vector<std::pair<std::shared_ptr<raft_entry_t>, utils::context*>>& entries)
{
    for (auto& entry_pair : entries)
    {
        auto entry = entry_pair.first;
        auto complete = entry_pair.second;
        entry->set_idx(_next_idx);
        _next_idx++;  
        _entries.add(entry, complete);
        _raft->check_and_set_configuration(entry);
    }   
    
    return 0;
}

//used by leader
int raft_log::log_append(std::shared_ptr<raft_entry_t> entry, utils::context* complete){
    entry->set_idx(_next_idx);

    if(raft_entry_is_cfg_change(entry.get()) || !_config_cache.empty()){
        if(RAFT_LOGTYPE_ADD_NONVOTING_NODE != entry->type()){
            /* 收到RAFT_LOGTYPE_ADD_NONVOTING_NODE这种configuration entry,
               会进入CFG_CATCHING_UP状态，这种entry并不会写入log，为了log index的连续性，
               这种entry并不实际占用index，因此_next_idx不需要加1
            */
            _next_idx++;    
        }
        _config_cache.emplace_back(std::make_pair(entry, complete));
    }else{
        _next_idx++;
        _entries.add(entry, complete); 
    } 

    return 0;  
}

int raft_log::config_cache_flush(){
    int count = 0;
    if(_config_cache.empty())
        return count;

    auto &ec = _config_cache.front();
    auto entry = ec.first;
    if(raft_entry_is_cfg_change(entry.get())){
        auto complete = ec.second;
        if(RAFT_LOGTYPE_ADD_NONVOTING_NODE == entry->type()){
            _raft->set_cfg_entry_complete(entry, complete);
        }else{
            if(complete)
                _raft->set_cfg_complete(complete);
            _raft->reset_cfg_entry();
            _entries.add(entry, complete);
        }
        _config_cache.pop_front();
        count++;
    }

    while(!_config_cache.empty()){
        auto &ec = _config_cache.front();
        auto entry = ec.first;
        if(raft_entry_is_cfg_change(entry.get()))
            break;

        auto complete = ec.second;
        _entries.add(entry, complete);
        _config_cache.pop_front();
        count++;
    }
    return count;
}

int raft_log::log_truncate(raft_index_t idx)
{
    int n = _entries.for_upper(idx, 0, raft_pop_log, _raft);
    _entries.remove_upper(idx, n);
    //截断log盘中idx（包含idx）后的entry      todo

    return 0;
}
