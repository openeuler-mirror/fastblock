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
#include <string>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "raft/state_machine.h"
#include "fastblock/rpc/osd_msg.pb.h"
#include "localstore/aos/types.h"
#include "op_type_excl_lock.h"

namespace fastblock::aos {
class osd_store;
class snapshot_ingest;
}


template <typename lock_type>
class lock_manager
{
public:
    lock_manager(const bool disable_flag = false)
        : _disable_flag(disable_flag)
    {
        SPDK_INFOLOG(osd, "lock_manager initialized to %d\n", _disable_flag);
    }

    void lock(const std::string &key, const utils::operation_type type, utils::context *complete)
    {
        if (_disable_flag)
        {
            complete->complete(0);
            return;
        }

        SPDK_DEBUGLOG(osd, "lock object: %s, type: %u, all locks cnt before %lu\n",
                            key.c_str(), static_cast<uint32_t>(type), _lock_map.size());

        // Add mutex for the object if not exist.
        if (!_lock_map.contains(key)) {
            _lock_map.emplace(key, std::make_unique<lock_type>());
        }
        auto iter = _lock_map.find(key);
        auto lock_ptr = iter->second.get();
        lock_ptr->lock(type, complete);
    }

    auto unlock(const std::string& key, const utils::operation_type type) {
        if (_disable_flag) {
            return;
        }

        SPDK_DEBUGLOG(osd, "unlock object: %s, type: %u, all locks cnt before %lu\n",
                            key.c_str(), static_cast<uint32_t>(type), _lock_map.size());

        // Add mutex for the object if not exist.
        if (!_lock_map.contains(key))
        {
            return;
        }

        auto it = _lock_map.find(key);
        auto lock_ptr = it->second.get();
        lock_ptr->unlock(type);

        // Remove mutex if no other waiter.
        auto iter = _lock_map.find(key);
        if (iter == _lock_map.end())
        {
            SPDK_DEBUGLOG(osd, "no lock found! done with mutex, key: %s, all mutex cnt: %lu\n",
                                key.c_str(), _lock_map.size());
            return;
        }

        SPDK_DEBUGLOG(osd, "done with lock, object: %s, all mutex cnt: %lu, file waiters: %lu\n",
                            key.c_str(), _lock_map.size(), iter->second->holders());

        // Remove it from the list if no other tasks on it, otherwise the list will be too big.
        if (0 == iter->second->holders()) {
            _lock_map.erase(key);

            SPDK_DEBUGLOG(osd, "no waiter on %s, erase. mutex cnt now %lu\n", key.c_str(), _lock_map.size());
        }
    }

private:
    // std::unordered_map: Abseil flat_hash_map under ASan+system-libabsl
    // crashes on empty-map contains (swisstable generations ABI mismatch).
    std::unordered_map<std::string, std::unique_ptr<lock_type>> _lock_map;
    const bool _disable_flag;
};

using object_complete = std::function<void (void *arg, int objerrno)>;
class osd_stm : public state_machine {
public:
    osd_stm();
    ~osd_stm() override;

    void apply(std::shared_ptr<raft_entry_t> entry, utils::context *complete) override;

    void write_obj(const std::string& obj_name, uint64_t offset, const std::string& data,
                   uint64_t raft_write_index, uint64_t image_snap_seq,
                   utils::context *complete);
    void delete_obj(const std::string& obj_name, uint64_t raft_write_index,
                    uint64_t image_snap_seq, utils::context *complete);

    void write_and_wait(const osd::write_request* request, osd::write_reply* response, google::protobuf::Closure* done);
    void read_and_wait(const osd::read_request* request, osd::read_reply* response, google::protobuf::Closure* done);
    void delete_and_wait(const osd::delete_request* request, osd::delete_reply* response, google::protobuf::Closure* done);

    void image_snap_and_wait(const osd::image_snap_request* request,
                             osd::image_snap_reply* response,
                             google::protobuf::Closure* done);

    /**
     * Attach AppendOnlyStore for this PG. Required for all object I/O (knife-1:
     * AOS is the only object path; no blob object fallback).
     */
    void attach_aos(fastblock::aos::osd_store* store, uint64_t pool_id,
                    uint64_t pg_id);

    bool aos_attached() const { return _aos != nullptr; }
    fastblock::aos::osd_store* aos_store() { return _aos; }
    uint64_t aos_pool_id() const { return _pool_id; }
    uint64_t aos_pg_id() const { return _pg_id; }

    /**
     * Image timeline for object apply (design §5.2 / P5). 0 = no image seq.
     * Control plane (mon/CSI) sets this before image-aware writes.
     */
    void set_image_snap_seq(uint64_t seq) { _image_snap_seq = seq; }
    uint64_t image_snap_seq() const { return _image_snap_seq; }

    /** Pin / re-pin (clone) / release image snapshot on attached AOS PG. */
    void create_image_snapshot(uint64_t image_snap_seq, object_rw_complete cb,
                               void* arg);
    void release_image_snapshot(uint64_t image_snap_seq, object_rw_complete cb,
                                void* arg);
    void clone_image(uint64_t image_snap_seq, object_rw_complete cb,
                     void* arg) {
      create_image_snapshot(image_snap_seq, std::move(cb), arg);
    }
    void flatten_image(uint64_t parent_image_seq, uint64_t new_image_seq,
                       uint64_t raft_write_index, object_rw_complete cb,
                       void* arg);

    bool uses_aos_objects() const override;
    std::vector<std::string> list_live_objects() override;
    std::vector<std::string> list_live_objects_at(uint64_t raft_idx) override;
    bool object_exists(const std::string& name) override;
    void pin_recovery_view(uint64_t raft_apply_index, uint64_t raft_term,
                           object_rw_complete cb_fn, void* arg) override;
    void unpin_recovery_view(object_rw_complete cb_fn, void* arg) override;
    void read_recovery_object(const std::string& object_name, char* buf,
                              uint64_t len, object_rw_complete cb_fn,
                              void* arg) override;

    void write_install_object(const std::string& obj_name, char* buf,
                              std::size_t size, uint64_t snapshot_index,
                              object_rw_complete cb_fn, void* arg) override;
    void begin_install_snapshot(uint64_t snapshot_index, uint64_t snapshot_term,
                                object_rw_complete cb_fn, void* arg) override;
    void finish_install_snapshot(object_rw_complete cb_fn, void* arg) override;

    auto unlock(const std::string& key, const utils::operation_type type){
        return _object_rw_lock.unlock(key, type);
    }

    void stop(object_rw_complete cb_fn, void* arg);

    void destroy_objects(object_complete cb_fn, void *arg);

private:
    /** After snapshot activate: point raft disk_log at serving-epoch log. */
    void bind_raft_log_to_serving();

    lock_manager<op_type_excl_lock<utils::operation_type>>   _object_rw_lock;
    uint32_t  _sockid;
    fastblock::aos::osd_store* _aos{nullptr};
    uint64_t _pool_id{0};
    uint64_t _pg_id{0};
    uint64_t _image_snap_seq{0};
    std::unique_ptr<fastblock::aos::snapshot_ingest> _ingest;
    // AOS recovery pin bookkeeping (was on state_machine for blob path).
    bool _recovery_pinned{false};
    uint64_t _recovery_snap_index{0};
};
