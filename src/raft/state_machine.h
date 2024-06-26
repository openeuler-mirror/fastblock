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
#include "raft_types.h"
#include "rpc/raft_msg.pb.h"
#include "utils/utils.h"
#include "spdk/thread.h"
#include "localstore/object_store.h"
#include "localstore/blob_manager.h"

class raft_server_t;

using stm_complete = std::function<void (void *, int)>;

class state_machine{
public:
    state_machine()
    : _raft(nullptr)
    , _last_applied_idx(0)
    , _apply_in_progress(false)
    , _store(global_blobstore(), global_io_channel()) {}

    void set_raft(raft_server_t* raft){
        _raft = raft;
    }

    void set_last_applied_idx(raft_index_t idx)
    {
        _last_applied_idx = idx;
    }

    void start();
    void stop() { spdk_poller_unregister(&_timer); }

    /**
     * @return index of last applied entry */
    raft_index_t get_last_applied_idx()
    {
        return _last_applied_idx;
    }

    /**
     * Apply entry at lastApplied + 1. Entry becomes 'committed'.
     * @return 1 if entry committed, 0 otherwise */
    int raft_apply_entry();

    int raft_apply_entries();

    virtual void apply(std::shared_ptr<raft_entry_t> entry, utils::context *complete) = 0;
    raft_server_t* get_raft(){
        return _raft;
    }

    bool get_apply_in_progress(){
        return _apply_in_progress;
    }

    void set_apply_in_progress(bool apply_in_progress){
        _apply_in_progress = apply_in_progress;
    }

    bool linearization();

    object_store* get_object_store(){
        return &_store;
    }

    std::string get_pg_name();

    void set_pg(std::string pg_name) {
        _store.set_pg(pg_name);
    }

    void load_object(object_store::container objects){
        _store.load(std::move(objects));
    }
private:
    raft_server_t* _raft;

    /* idx of highest log entry applied to state machine */
    raft_index_t _last_applied_idx;
    bool _apply_in_progress;
    struct spdk_poller * _timer;

    uint64_t _last_save_time = 0;
    raft_index_t _last_save_index = 0;

protected:
    object_store _store;
};