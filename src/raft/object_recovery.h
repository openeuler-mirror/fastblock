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

#include <functional>
#include <string>
#include <vector>

class state_machine;

using recovery_op_complete = std::function<void(void* arg, int rerrno)>;

/**
 * Leader install-snapshot object iteration.
 *
 * Pins a recovery view on the state machine (AOS: O(1) raft pin;
 * legacy blob: per-object recovery snapshots), then streams full objects.
 */
class object_recovery {
public:
  explicit object_recovery(state_machine* stm) : _stm(stm) {}

  /**
   * List live objects and pin recovery view at (snap_index, snap_term).
   */
  void recovery_create(uint64_t snap_index, uint64_t snap_term,
                       recovery_op_complete cb_fn, void* arg);

  std::vector<std::string> recovery_get_obj_names(size_t start, size_t num) {
    std::vector<std::string> object_names;
    size_t index = start;
    size_t end = start + num;
    while (!iter_is_end(index) && index < end) {
      object_names.emplace_back(get_iter_name(index));
      index++;
    }
    return object_names;
  }

  /** Read next object into buf (4MiB unit). -ENOENT when iteration ends. */
  void recovery_read_iter_first(recovery_op_complete cb_fn, char* buf,
                                void* arg);
  void recovery_read_iter_next(recovery_op_complete cb_fn, char* buf, void* arg);

  void recovery_delete(recovery_op_complete cb_fn, void* arg);

  void iter_start() { idx = 0; }

  std::string& iter_next_name() { return _obj_names[idx++]; }

  bool iter_is_end() { return idx == _obj_names.size(); }

  bool iter_is_end(size_t index) { return index == _obj_names.size(); }
  std::string& get_iter_name(size_t index) { return _obj_names[index]; }
  size_t get_iter_idx() { return idx; }

private:
  state_machine* _stm{nullptr};
  std::vector<std::string> _obj_names;
  size_t idx = 0;
};
