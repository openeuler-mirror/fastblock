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
#include "localstore/types.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

class raft_server_t;

using stm_complete = std::function<void(void*, int)>;

/**
 * Raft state machine base.
 *
 * Object data plane: production osd_stm uses AppendOnlyStore only (design v2 P4).
 * Base defaults are no-ops / empty; override as osd_stm does.
 */
class state_machine {
public:
  state_machine()
      : _raft(nullptr), _last_applied_idx(0), _apply_in_progress(false) {}

  virtual ~state_machine() {
    stop();
    _raft = nullptr;
  }

  void set_raft(raft_server_t* raft) { _raft = raft; }

  void clear_raft() { _raft = nullptr; }

  void set_last_applied_idx(raft_index_t idx) { _last_applied_idx = idx; }

  void start();
  void stop() {
    if (_timer) {
      spdk_poller_unregister(&_timer);
      _timer = nullptr;
    }
  }

  raft_index_t get_last_applied_idx() { return _last_applied_idx; }

  int raft_apply_entry();
  int raft_apply_entries();

  virtual void apply(std::shared_ptr<raft_entry_t> entry,
                     utils::context* complete) = 0;

  /**
   * Follower install-snapshot object write.
   * Production (osd_stm): AOS apply. Default: not supported.
   */
  virtual void write_install_object(const std::string& obj_name, char* buf,
                                    std::size_t size, uint64_t snapshot_index,
                                    object_rw_complete cb_fn, void* arg) {
    (void)obj_name;
    (void)buf;
    (void)size;
    (void)snapshot_index;
    if (cb_fn) {
      cb_fn(arg, -ENOTSUP);
    }
  }

  /** idle -> ingesting (AOS staging epoch). Default: no-op success. */
  virtual void begin_install_snapshot(uint64_t snapshot_index,
                                      uint64_t snapshot_term,
                                      object_rw_complete cb_fn, void* arg) {
    (void)snapshot_index;
    (void)snapshot_term;
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

  /**
   * ingesting -> sealed -> catchup -> idle (activate serving epoch).
   * Default: no-op success.
   */
  virtual void finish_install_snapshot(object_rw_complete cb_fn, void* arg) {
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

  /** True when object data is AppendOnlyStore. */
  virtual bool uses_aos_objects() const { return false; }

  /** Live object names for install-snapshot leader (current head view). */
  virtual std::vector<std::string> list_live_objects() { return {}; }

  /** Live names at a raft snapshot index (0 = head). Default: head list. */
  virtual std::vector<std::string> list_live_objects_at(uint64_t raft_idx) {
    (void)raft_idx;
    return list_live_objects();
  }

  virtual bool object_exists(const std::string& name) {
    (void)name;
    return false;
  }

  virtual void pin_recovery_view(uint64_t raft_apply_index, uint64_t raft_term,
                                 object_rw_complete cb_fn, void* arg);

  virtual void unpin_recovery_view(object_rw_complete cb_fn, void* arg);

  virtual void read_recovery_object(const std::string& object_name, char* buf,
                                    uint64_t len, object_rw_complete cb_fn,
                                    void* arg);

  raft_server_t* get_raft() { return _raft; }

  bool get_apply_in_progress() { return _apply_in_progress; }

  void set_apply_in_progress(bool apply_in_progress) {
    _apply_in_progress = apply_in_progress;
  }

  bool linearization();

  std::string& get_pg_name();

  void set_pg(std::string pg_name) { _pg_name = std::move(pg_name); }

  bool raft_is_leader();

private:
  raft_server_t* _raft;

  raft_index_t _last_applied_idx;
  bool _apply_in_progress;
  struct spdk_poller* _timer{nullptr};

  uint64_t _last_save_time = 0;
  raft_index_t _last_save_index = 0;
  std::string _pg_name;
};
