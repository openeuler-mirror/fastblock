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

    _base = idx;
    _base_term = term;
}

std::shared_ptr<raft_log> log_new(disk_log* log)
{
    return std::make_shared<raft_log>(log);
}

int raft_log::log_append(std::vector<std::pair<std::shared_ptr<raft_entry_t>, utils::context*>>& entries)
{
    // raft_index_t first_idx = _next_idx;

    for (auto& entry_pair : entries)
    {
        auto entry = entry_pair.first;
        auto complete = entry_pair.second;
        entry->set_idx(_next_idx);
        _next_idx++;  
        _entries.add(entry, complete);
    }   
    
    // ((raft_server_t*)_raft)->raft_offer_log(entries, first_idx);
    return 0;
}

int raft_log::log_truncate(raft_index_t idx)
{
    int n = _entries.for_upper(idx, 0, raft_pop_log, _raft); 
    _entries.remove_upper(idx, n);
    //截断log盘中idx（包含idx）后的entry      todo
    
    return 0;
}