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
        // SPDK_WARNLOG("entry index: %ld, _next_idx: %ld\n", entry->idx(), _next_idx);
        entry->set_idx(_next_idx);
        _next_idx++;  
        _entries.add(entry, complete);
        _raft->check_and_set_configuration(entry);
    }   
    
    return 0;
}

//used by leader
int raft_log::log_append(std::shared_ptr<raft_entry_t> entry, utils::context* complete, bool push_front){
    if(push_front)
        _entry_queue.emplace_front(std::make_pair(entry, complete));
    else
        _entry_queue.emplace_back(std::make_pair(entry, complete));
    return 0;
}

int raft_log::entry_queue_flush(){
    int count = 0;
    if(_entry_queue.empty())
        return count;

    auto &ec = _entry_queue.front();
    auto entry = ec.first;
    if(raft_entry_is_cfg_change(entry)){
        entry->set_idx(_next_idx);

        auto complete = ec.second;
        if(RAFT_LOGTYPE_ADD_NONVOTING_NODE == entry->type()){
            _raft->set_cfg_entry_complete(entry, complete);
        }else{
            /* 收到RAFT_LOGTYPE_ADD_NONVOTING_NODE这种configuration entry,
               会进入CFG_CATCHING_UP状态，这种entry并不会写入log，为了log index的连续性，
               这种entry并不实际占用index，因此_next_idx不需要加1
            */
            _next_idx++;    

            if(complete)
                _raft->set_cfg_complete(complete);
            _raft->reset_cfg_entry();
            _entries.add(entry, nullptr);
        }
        _entry_queue.pop_front();
        count++;
    }

    while(!_entry_queue.empty()){
        auto &ec = _entry_queue.front();
        auto entry = ec.first;
        if(raft_entry_is_cfg_change(entry))
            break;

        entry->set_idx(_next_idx);
        _next_idx++; 
        
        auto complete = ec.second;
        _entries.add(entry, complete);
        _entry_queue.pop_front();
        count++;
    }
    return count;    
}

void raft_log::disk_append(raft_index_t start_idx, raft_index_t end_idx, utils::context* complete){
    std::vector<std::shared_ptr<raft_entry_t>> raft_entries;
    _entries.get_between(start_idx, end_idx, raft_entries);
    if(!_log){
        complete->complete(0);
        return;
    }
    std::vector<log_entry_t> log_entries;
    for (auto& raft_entry : raft_entries) {
        log_entries.emplace_back(raft_entry_to_log_entry(*raft_entry));
    }
    SPDK_INFOLOG(pg_group, "in pg %lu.%lu, disk_append [%ld, %ld] size:%lu.\n", 
            _raft->raft_get_pool_id(), _raft->raft_get_pg_id(), start_idx, end_idx, log_entries.size());
    _log->append(
        log_entries,
        [log_entries, start_idx, end_idx, pool_id = _raft->raft_get_pool_id(), pg_id = _raft->raft_get_pg_id()](void *arg, int rberrno) mutable
        {
            SPDK_INFOLOG(pg_group, "in pg %lu.%lu, after disk_append [%ld, %ld]\n", pool_id, pg_id, start_idx, end_idx);
            utils::context *ctx = (utils::context *)arg;
            for (auto &entry : log_entries)
            {
                free_buffer_list(entry.data);
            }
            ctx->complete(rberrno);
        },
        complete);
}

int raft_log::log_truncate(raft_index_t idx, log_op_complete cb_fn, void* arg)
{
    auto last_cache_idx = get_last_cache_entry();
    raft_write_entry_finish(idx, last_cache_idx, 0);
    remove_entry_between(idx, last_cache_idx);
    
    _raft->get_node_configuration_manager().truncate_by_idx(idx);
    
    _log->truncate(idx, std::move(cb_fn), arg);

    return 0;
}