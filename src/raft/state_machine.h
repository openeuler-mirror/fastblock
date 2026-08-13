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
#include "fastblock/utils/utils.h"
#include "spdk/thread.h"
#include "localstore/object_store.h"
#include "localstore/blob_manager.h"

#include <map>
#include <string>
#include <vector>

class raft_server_t;

using stm_complete = std::function<void (void *, int)>;

/**
 * Raft state machine base.
 *
 * Object data plane: production osd_stm uses AppendOnlyStore only.
 * Default methods still touch object_store for **unit demos / residual tests
 * only** (P4: not on the OSD production path). Prefer overriding virtuals
 * as osd_stm does.
 */
class state_machine{
public:
    state_machine()
    : _raft(nullptr)
    , _last_applied_idx(0)
    , _apply_in_progress(false)
    // P3 AOS-only: blobstore may be null; production osd_stm never uses _store.
    , _store(global_blobstore(core_sharded::get_core_sharded().this_shard_id()),
             global_io_channel(core_sharded::get_core_sharded().this_shard_id())) {}

    virtual ~state_machine() {
        stop();
        _raft = nullptr;
    }

    void set_raft(raft_server_t* raft){
        _raft = raft;
    }

    void clear_raft() {
        _raft = nullptr;
    }

    void set_last_applied_idx(raft_index_t idx)
    {
        _last_applied_idx = idx;
    }

    void start();
    void stop() {
        if (_timer) {
            spdk_poller_unregister(&_timer);
            _timer = nullptr;
        }
    }

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

    /**
     * Follower install-snapshot object write.
     * Production (osd_stm): AOS apply. Default: blob (demos only).
     */
    virtual void write_install_object(const std::string& obj_name, char* buf,
                                      std::size_t size, uint64_t snapshot_index,
                                      object_rw_complete cb_fn, void* arg) {
        (void)snapshot_index;
        std::map<std::string, xattr_val_type> xattr;
        xattr["type"] = blob_type::object;
        xattr["pg"] = get_pg_name();
        _store.write(xattr, obj_name, 0, buf, size, std::move(cb_fn), arg);
    }

    /** True when object data is AppendOnlyStore (not blob object_store). */
    virtual bool uses_aos_objects() const { return false; }

    /** Live object names for install-snapshot leader. */
    virtual std::vector<std::string> list_live_objects() {
        std::vector<std::string> names;
        names.reserve(_store.table.size());
        for (const auto& pr : _store.table) {
            names.push_back(pr.first);
        }
        return names;
    }

    virtual bool object_exists(const std::string& name) {
        return _store.is_exist(name);
    }

    /**
     * Pin a recovery read view at raft_apply_index (AOS: O(1) pin;
     * default blob: per-object recovery snapshots).
     */
    virtual void pin_recovery_view(uint64_t raft_apply_index, uint64_t raft_term,
                                   object_rw_complete cb_fn, void* arg);

    virtual void unpin_recovery_view(object_rw_complete cb_fn, void* arg);

    /**
     * Read full object for recovery/snapshot_check (up to len bytes, zero-fill rest).
     * When recovery view is pinned, implementations should read that view.
     */
    virtual void read_recovery_object(const std::string& object_name, char* buf,
                                      uint64_t len, object_rw_complete cb_fn,
                                      void* arg);

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

    std::string &get_pg_name();

    void set_pg(std::string pg_name) {
        _store.set_pg(pg_name);
    }

    /** Legacy blob object load — no-op for AOS-only osd_stm. */
    virtual void load_object(object_store::container objects){
        _store.load(std::move(objects));
    }

    bool raft_is_leader();
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
    // Recovery pin bookkeeping (blob default path; AOS uses pin records).
    std::vector<std::string> _recovery_names;
    bool _recovery_pinned{false};
    uint64_t _recovery_snap_index{0};
};
