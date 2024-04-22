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
#pragma once
#include <limits>

#include "raft_cache.h"
#include "utils/log.h"
#include "localstore/disk_log.h"
#include "localstore/spdk_buffer.h"

class raft_server_t;

class raft_log
{
public:
    raft_log(disk_log* log)
    : _log(log)
    , _next_idx(1)
    , _max_applied_entry_num_in_cache(500) {}

    void log_set_raft(raft_server_t* raft){
        _raft = raft;
    }

    /**
     * Add 'n' entries to the log cache
     */
    int log_append(std::vector<std::pair<std::shared_ptr<raft_entry_t>, utils::context*>>& entries);

    int log_append(std::shared_ptr<raft_entry_t>, utils::context*, bool push_front = false);


    log_entry_t raft_entry_to_log_entry(raft_entry_t& raft_entry) {
        log_entry_t entry;

        entry.index = raft_entry.idx();
        entry.term_id = raft_entry.term();;
        entry.size = raft_entry.data().size();
        entry.type = raft_entry.type();
        entry.meta = raft_entry.meta();

        SPDK_INFOLOG_EX(pg_group, "entry.size:%lu \n", entry.size);
        if (entry.size % 4096 != 0) {
            SPDK_ERRLOG_EX("data size:%lu not align.\n", entry.size);
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

    void disk_append(raft_index_t start_idx, raft_index_t end_idx, utils::context* complete){
        std::vector<std::shared_ptr<raft_entry_t>> raft_entries;
        _entries.get_between(start_idx, end_idx, raft_entries);
        SPDK_INFOLOG_EX(pg_group, "start_idx:%lu end_idx:%lu.\n", start_idx, end_idx);

        if(!_log){
            complete->complete(0);
            return;
        }

        std::vector<log_entry_t> log_entries;
        for (auto& raft_entry : raft_entries) {
            log_entries.emplace_back(raft_entry_to_log_entry(*raft_entry));
        }

        SPDK_INFOLOG_EX(pg_group, "disk_append size:%lu.\n", log_entries.size());
        _log->append(
            log_entries,
            [log_entries](void *arg, int rberrno) mutable
            {
                SPDK_INFOLOG_EX(pg_group, "after disk_append.\n");
                utils::context *ctx = (utils::context *)arg;
                for (auto &entry : log_entries)
                {
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
        SPDK_INFOLOG_EX(pg_group, "start_idx:%lu end_idx:%lu.\n", start_idx, end_idx);
        _log->read(
            start_idx,
            end_idx,
            [this, cb_fn = std::move(cb_fn)](void *, std::vector<log_entry_t> &&entries, int rberrno)
            {
                SPDK_INFOLOG_EX(pg_group, "after disk_read, rberrno: %d, entry size: %lu\n", rberrno, entries.size());
                std::vector<raft_entry_t> raft_entries;
                if (rberrno != 0)
                {
                    cb_fn({}, rberrno);
                    return;
                }
                for (auto &entry : entries)
                {
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
        _entries.clear();
    }
  
    //截断idx（包含）之后的log entry
    int log_truncate(raft_index_t idx, log_op_complete cb_fn, void* arg);

    raft_index_t log_get_base_index()
    {
        return _log->get_lowest_index();
    }

    raft_term_t log_get_base_term()
    {
        return _log->get_term_id(_log->get_lowest_index());
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

    void set_applied_index(raft_index_t last_applied_idx, raft_index_t idx) {
        auto first_entry_idx = first_log_in_cache();
        if(first_entry_idx > last_applied_idx + 1){
            //说明raft启动时没有加载未apply的日志到cache
            SPDK_INFOLOG_EX(pg_group, "first entry: %ld in cache > last_applied_idx + 1: %ld\n",
                               first_log_in_cache(), last_applied_idx + 1);
            return;
        }
        uint32_t applied_num = last_applied_idx - first_entry_idx + 1;
        SPDK_DEBUGLOG_EX(pg_group, "apply [%ld, %ld], applied_num: %u, _max_applied_entry_num_in_cache: %u first_entry_idx: %ld\n",
                            last_applied_idx + 1, idx, applied_num, _max_applied_entry_num_in_cache, first_entry_idx);
        if(applied_num + idx - last_applied_idx > _max_applied_entry_num_in_cache){
            auto remove_size = applied_num + idx - last_applied_idx - _max_applied_entry_num_in_cache;
            auto end_remove_idx = first_entry_idx + remove_size - 1;
            SPDK_DEBUGLOG_EX(pg_group, "remove entry [%ld, %ld] from cache\n", first_entry_idx, end_remove_idx);
            get_entry_cache().remove_entry_between(first_entry_idx, end_remove_idx);
        }
        set_trim_index(idx);
    }

    void stop(){
        if(_log){
            _log->stop([](void*, int){}, nullptr);
            delete _log;
            _log = nullptr;
        }
    }

    void clear_entry_queue(int state){
        while(!_entry_queue.empty()){
            auto &ec = _entry_queue.front();
            auto complete = ec.second;
            if(complete)
                complete->complete(state);
            _entry_queue.pop_front();
        }
    }

    // int config_cache_flush();
    int entry_queue_flush();

    std::shared_ptr<raft_entry_t> get_entry(raft_index_t idx){
        return _entries.get(idx);
    }

    void set_next_idx(raft_index_t next_idx){
        _next_idx = next_idx;
    }

    raft_index_t get_next_idx(){
        return _next_idx;
    }

    void set_disk_log_index(raft_index_t index, log_op_complete cb_fn, void* arg){
        if(_log){
            _log->set_index(index, std::move(cb_fn), arg);
        }
    }

    void load(log_op_complete cb_fn, void* arg){
        _log->load(
          [this, cb_fn = std::move(cb_fn)](void* arg, int lerrno){
            if(lerrno != 0){
                cb_fn(arg, lerrno);
                return;
            }
            _next_idx = _log->get_highest_index() + 1;
            cb_fn(arg, lerrno);
          },
          arg);
    }

private:
    disk_log* _log;

    /* position of the queue */
    // raft_index_t front;

    raft_index_t _next_idx;

    // raft_entry_t* entries;
    entry_cache  _entries;

    raft_server_t* _raft;
    
    /* The maximum number of entries that have been applied in the cache */
    uint32_t _max_applied_entry_num_in_cache;
    
    std::deque<std::pair<std::shared_ptr<raft_entry_t>, utils::context*>> _entry_queue;
};

std::shared_ptr<raft_log> log_new(disk_log *log);