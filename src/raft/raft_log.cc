
#include <assert.h>
#include <string>

#include "raft.h"
#include "raft_private.h"
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

int raft_log::log_append(std::vector<std::pair<std::shared_ptr<msg_entry_t>, context*>>& entries)
{
    // raft_index_t first_idx = _next_idx;

    for (auto& entry_pair : entries)
    {
        auto entry = entry_pair.first;
        auto complete = entry_pair.second;
        entry->set_idx(_next_idx);
        _next_idx++;  
        _entries.add(entry->obj_name(), entry, complete);
    }   
    
    // ((raft_server_t*)_raft)->raft_offer_log(entries, first_idx);
    return 0;
}

int raft_log::log_delete(raft_index_t idx)
{
    int n = _entries.for_upper(idx, 0, raft_pop_log, _raft);
    // if (_cb && _cb->log_pop)
        // e = _cb->log_pop(_raft, ((raft_server_t*)_raft)->raft_get_udata(), ptr, start_idx, &k);  
    _entries.delete_upper(idx, n);
    return 0;
}

int raft_log::log_poll(raft_index_t idx)
{
    int num = 0;
    raft_term_t term = 0;  //删除的最后一个日志的下一条日志的term
    //从日志系统中删除，返回term
    // if (_cb && _cb->log_poll)
        // e = _cb->log_poll(_raft, ((raft_server_t*)_raft)->raft_get_udata(),
                                // &_entries[me->front], me->base + 1, &num, &term);    
    
    /*
      apply一条log时，需要把这条log从缓存中删除，因此compat时idx在缓存中是不存在的
    */
    if(num > 0){
        _base_term = term;
        _base = idx + 1;
    }

    return 0;
}