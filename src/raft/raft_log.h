#ifndef RAFT_LOG_H_
#define RAFT_LOG_H_

#include "raft_cache.h"
#include "localstore/disk_log.h"
#include "localstore/spdk_buffer.h"

// #define ENABLE_LOG

struct raft_cbs_t;

class raft_log
{
public:
    raft_log(disk_log* log)
    : _log(log)
    , _next_idx(0)
    , _base(0)
    , _base_term(0){}

    void log_set_raft(void* raft){
        _raft = raft;
    }

    /**
     * Add 'n' entries to the log with valid (positive, non-zero) IDs
     * that haven't already been added and save the number of successfully
     * appended entries in 'n' */
    int log_append(std::vector<std::pair<std::shared_ptr<raft_entry_t>, context*>>& entries);

    log_entry_t raft_entry_to_log_entry(raft_entry_t& raft_entry) {
        log_entry_t entry;
            
        entry.index = raft_entry.idx();
        entry.term_id = raft_entry.term();;
        entry.size = raft_entry.data().size();
        entry.meta = raft_entry.meta();
        
        if (entry.size % 4096 != 0) {
            SPDK_ERRLOG("data size:%lu not align.\n", entry.size);
            /// TODO: 怎么处理这个错误
            return log_entry_t{};
        }

        auto datastr = raft_entry.data();
        entry.data = make_buffer_list(entry.size / 4096);
        int i = 0;
        for (auto sbuf : entry.data) {
            sbuf.append(datastr.c_str() + i * 4096, 4096);
            i++;
        }
        return entry;
    }

    void disk_append(raft_index_t start_idx, raft_index_t end_idx, context* complete){
        std::vector<std::shared_ptr<raft_entry_t>> raft_entries;
        _entries.get_between(start_idx, end_idx, raft_entries);
        SPDK_NOTICELOG("start_idx:%lu end_idx:%lu.\n", start_idx, end_idx);

        if(!_log){
            complete->complete(0);
            return;
        }
#ifdef ENABLE_LOG
        std::vector<log_entry_t> log_entries;
        for (auto& raft_entry : raft_entries) {
            log_entries.emplace_back(raft_entry_to_log_entry(*raft_entry));
        }

        SPDK_NOTICELOG("disk_append size:%lu.\n", log_entries.size());
        _log->append(log_entries, 
          [](void *arg, int rberrno){
              SPDK_NOTICELOG("after disk_append.\n");
              context* ctx = (context*)arg;
              ctx->complete(rberrno);
          },
          complete);
#endif
    }

    /** Get an array of entries from this index onwards.
     * This is used for batching.
     */
    void log_get_from_idx(raft_index_t idx, std::vector<std::shared_ptr<raft_entry_t>> &entrys)
    { 
        _entries.get_upper(idx, entrys);
    }

    std::shared_ptr<raft_entry_t> log_get_at_idx(raft_index_t idx)
    {
        return _entries.get(idx);
    }

    void log_clear()
    {
        _base = 0;
        _base_term = 0;
        _entries.clear();
    }

    /**
     * Delete all logs from this log onwards */
    int log_delete(raft_index_t idx);

    /**
     * Remove all entries before and at idx. */
    int log_poll(raft_index_t idx);

    raft_index_t log_get_base()
    {
        return _base;
    }

    raft_term_t log_get_base_term()
    {
        return _base_term;
    }

    raft_index_t get_last_cache_entry(){
        return _entries.get_last_cache_entry();
    }

    void log_load_from_snapshot(raft_index_t idx, raft_term_t term);

    void raft_write_entry_finish(raft_index_t start_idx, raft_index_t end_idx, int result){
        _entries.complete_entry_between(start_idx, end_idx, result);
    }

private:
    disk_log* _log;

    /* position of the queue */
    // raft_index_t front;

    raft_index_t _next_idx;

    /* we compact the log, and thus need to increment the Base Log Index */
    raft_index_t _base;

    /* term of the base */
    raft_term_t _base_term;

    // raft_entry_t* entries;
    entry_cache  _entries;

    void* _raft;
};

std::shared_ptr<raft_log> log_new(disk_log* log);

#endif /* RAFT_LOG_H_ */