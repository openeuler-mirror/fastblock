#ifndef RAFT_LOG_H_
#define RAFT_LOG_H_
#include <limits>

#include "raft_cache.h"
#include "localstore/disk_log.h"
#include "localstore/spdk_buffer.h"

struct raft_cbs_t;

class raft_log
{
public:
    raft_log(disk_log* log)
    : _log(log)
    , _next_idx(1)
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
        entry.type = raft_entry.type();
        entry.meta = raft_entry.meta();

        SPDK_INFOLOG(pg_group, "entry.size:%lu \n", entry.size);
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
        SPDK_INFOLOG(pg_group, "start_idx:%lu end_idx:%lu.\n", start_idx, end_idx);

        if(!_log){
            complete->complete(0);
            return;
        }

        std::vector<log_entry_t> log_entries;
        for (auto& raft_entry : raft_entries) {
            log_entries.emplace_back(raft_entry_to_log_entry(*raft_entry));
        }

        SPDK_INFOLOG(pg_group, "disk_append size:%lu.\n", log_entries.size());
        _log->append(
            log_entries,
            [log_entries](void *arg, int rberrno) mutable
            {
                SPDK_INFOLOG(pg_group, "after disk_append.\n");
                context *ctx = (context *)arg;
                for(auto & entry : log_entries){
                    free_buffer_list(entry.data);
                }
                ctx->complete(rberrno);
            },
            complete);
    }

    raft_entry_t log_entry_to_raft_entry(log_entry_t& log_entry){
        raft_entry_t raft_entry;
        raft_entry.set_term(log_entry.term_id);
        raft_entry.set_idx(log_entry.index);
        raft_entry.set_type(log_entry.type);
        raft_entry.set_obj_name("");
        raft_entry.set_meta(log_entry.meta);
        std::string data;
        data.reserve(log_entry.data.bytes());
        for (auto sbuf : log_entry.data){
            data.append(sbuf.get_buf(), sbuf.size());
        }
        raft_entry.set_data(std::move(data));
        return raft_entry;
    }

    using log_read_complete = std::function<void (std::vector<raft_entry_t>&&, int rberrno)>;
    void disk_read(raft_index_t start_idx, raft_index_t end_idx, log_read_complete cb_fn){
        SPDK_INFOLOG(pg_group, "start_idx:%lu end_idx:%lu.\n", start_idx, end_idx);
        _log->read(
          start_idx, 
          end_idx, 
          [this, cb_fn = std::move(cb_fn)](void *, std::vector<log_entry_t>&& entries, int rberrno){
            SPDK_INFOLOG(pg_group, "after disk_read, rberrno: %d, entry size: %lu\n", rberrno, entries.size());
            std::vector<raft_entry_t> raft_entries;
            if(rberrno != 0){
                cb_fn({}, rberrno);
                return;
            }  
            for(auto& entry : entries){
                raft_entries.emplace_back(log_entry_to_raft_entry(entry));
                free_buffer_list(entry.data);
            }

            cb_fn(std::move(raft_entries), rberrno);
          }, 
          nullptr);
    }

    bool disk_get_term(raft_index_t idx, raft_term_t& term){
        auto _term = _log->get_term_id(idx);
        if(_term == 0){
            return false;
        }
        term = _term;
        return true;
    }

    /** Get an array of entries from this index onwards.
     * This is used for batching.
     */
    void log_get_from_idx(raft_index_t idx, std::vector<std::shared_ptr<raft_entry_t>> &entrys)
    { 
        _entries.get_upper(idx, entrys);
    }

    void log_get_from_idx(raft_index_t idx, int num, std::vector<std::shared_ptr<raft_entry_t>> &entrys)
    { 
        return _entries.get(idx, num, entrys); 
    }

    std::shared_ptr<raft_entry_t> log_get_at_idx(raft_index_t idx)
    {
        return _entries.get(idx);
    }

    raft_index_t first_log_in_cache(){
        auto entry = _entries.get_first_entry();
        if(!entry)
            return std::numeric_limits<raft_index_t>::max();
        return entry->idx();
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

    void remove_entry_between(raft_index_t start_idx, raft_index_t end_idx){
        _entries.remove_entry_between(start_idx, end_idx);
    }

    entry_cache& get_entry_cache(){
        return _entries;
    }

    /// TODO: 这里一定要改。等raft_index_t改成uint64_t，这里就不用强制转换了
    void set_trim_index(raft_index_t index) {
        _log->advance_trim_index((uint64_t)index);
    }

    void set_applied_index(raft_index_t idx) {
        get_entry_cache().remove(idx);
        set_trim_index(idx);
    }

    void destroy_log(){
        if(_log){
            _log->stop([](void*, int){}, nullptr);
            delete _log;
            _log = nullptr;
        }
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
