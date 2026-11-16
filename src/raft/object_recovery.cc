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

#include "object_recovery.h"
#include "state_machine.h"

#include <spdk/log.h>

#include <cerrno>

void object_recovery::recovery_create(uint64_t snap_index, uint64_t snap_term,
                                      recovery_op_complete cb_fn, void* arg) {
  if (_stm == nullptr) {
    if (cb_fn) {
      cb_fn(arg, -EINVAL);
    }
    return;
  }
  _obj_names = _stm->list_live_objects();
  idx = 0;
  _stm->pin_recovery_view(snap_index, snap_term, std::move(cb_fn), arg);
}

void object_recovery::recovery_read_iter_first(recovery_op_complete cb_fn,
                                               char* buf, void* arg) {
  iter_start();
  if (iter_is_end()) {
    cb_fn(arg, -ENOENT);
    return;
  }
  auto next_name = iter_next_name();
  SPDK_DEBUGLOG(pg_group, "object name:%s recovery read.\n",
                next_name.c_str());
  // 4MiB object unit (matches raft install path).
  constexpr uint64_t kUnit = 4ull * 1024 * 1024;
  _stm->read_recovery_object(next_name, buf, kUnit, std::move(cb_fn), arg);
}

void object_recovery::recovery_read_iter_next(recovery_op_complete cb_fn,
                                              char* buf, void* arg) {
  if (iter_is_end()) {
    cb_fn(arg, -ENOENT);
    return;
  }
  auto next_name = iter_next_name();
  SPDK_DEBUGLOG(pg_group, "object name:%s recovery read.\n",
                next_name.c_str());
  constexpr uint64_t kUnit = 4ull * 1024 * 1024;
  _stm->read_recovery_object(next_name, buf, kUnit, std::move(cb_fn), arg);
}

void object_recovery::recovery_delete(recovery_op_complete cb_fn, void* arg) {
  if (_stm == nullptr) {
    if (cb_fn) {
      cb_fn(arg, -EINVAL);
    }
    return;
  }
  _stm->unpin_recovery_view(std::move(cb_fn), arg);
}
