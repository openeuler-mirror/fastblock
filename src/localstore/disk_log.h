/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#pragma once

/**
 * disk_log — AOS-only production facade over log_store (design v2 P1/P4).
 * Blob/rolling_blob path removed from the production library.
 */

#include "buffer_pool.h"
#include "log_entry.h"
#include "types.h"
#include "fastblock/base/core_sharded.h"
#include "utils/units.h"
#include "fastblock/utils/utils.h"
#include "localstore/aos/log_store.h"

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/string.h>

#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

class disk_log;

using log_op_complete =
    std::function<void(void* arg, int rberrno)>;
using log_op_with_entry_complete =
    std::function<void(void* arg, std::vector<log_entry_t>&&, int rberrno)>;

class disk_log {
public:
  /** Does not own log_store (osd_store owns it). */
  explicit disk_log(fastblock::aos::log_store* aos_log) : _aos_log(aos_log) {}

  ~disk_log() = default;

  bool is_aos() const { return _aos_log != nullptr; }

  void start() {
    // AOS log has no rolling-blob trim poller.
  }

  void append(std::vector<log_entry_t>& entries, log_op_complete cb_fn,
              void* arg) {
    append_aos(entries, std::move(cb_fn), arg);
  }

  void append(log_entry_t& entry, log_op_complete cb_fn, void* arg) {
    std::vector<log_entry_t> v;
    v.push_back(std::move(entry));
    append_aos(v, std::move(cb_fn), arg);
  }

  void read(uint64_t index, log_op_with_entry_complete cb_fn, void* arg) {
    read(index, index, std::move(cb_fn), arg);
  }

  void read(uint64_t start_index, uint64_t end_index,
            log_op_with_entry_complete cb_fn, void* arg) {
    read_aos(start_index, end_index, std::move(cb_fn), arg);
  }

  void stop(log_op_complete cb_fn, void* arg) {
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

  /** AOS: log lives in shared segments; nothing to delete per-blob. */
  void remove_blob(struct spdk_blob_store* /*bs*/, log_op_complete cb_fn,
                   void* arg) {
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

  uint64_t get_term_id(uint64_t index) {
    if (_aos_log) {
      return _aos_log->get_term(index);
    }
    return 0;
  }

  bool maybe_trim() { return false; }

  void trim_back(uint64_t /*start_index*/, uint64_t end_index,
                 log_op_complete cb_fn, void* arg) {
    if (_aos_log) {
      _aos_log->trim_to(end_index);
      _lowest_index = _aos_log->base_index() == 0 ? end_index + 1
                                                  : _aos_log->base_index();
    }
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

  void truncate(uint64_t start_index, log_op_complete cb_fn, void* arg) {
    if (_aos_log) {
      _aos_log->truncate_from(start_index, std::move(cb_fn), arg);
      return;
    }
    if (cb_fn) {
      cb_fn(arg, -EINVAL);
    }
  }

  void advance_trim_index(uint64_t index) {
    _trim_index = std::max(_trim_index, index);
    if (_aos_log && index > 1000) {
      _aos_log->trim_to(index - 1000);
    }
  }

  std::string dump_state() {
    std::stringstream sstream;
    if (_aos_log) {
      sstream << "\naos_log base:" << _aos_log->base_index()
              << " last:" << _aos_log->last_index()
              << " count:" << _aos_log->entry_count()
              << " trim_index:" << _trim_index << std::endl;
    }
    return sstream.str();
  }

  uint64_t get_lowest_index() {
    if (_aos_log) {
      return _aos_log->base_index() == 0 ? 1 : _aos_log->base_index();
    }
    return _lowest_index;
  }

  uint64_t get_highest_index() {
    if (_aos_log) {
      return _aos_log->last_index();
    }
    return _highest_index;
  }

  void set_index(uint64_t index, log_op_complete cb_fn, void* arg) {
    if (_aos_log) {
      if (_aos_log->entry_count() > 0) {
        _aos_log->truncate_from(_aos_log->base_index(), [](void*, int) {},
                                nullptr);
      }
      _trim_index = index;
      _lowest_index = index + 1;
      _highest_index = index;
    }
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

  void set_blob_xattr(std::map<std::string, xattr_val_type>& /*xattr*/,
                      log_op_complete&& cb_fn, void* arg) {
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

  void load(log_op_complete cb_fn, void* arg) {
    // Already recovered with osd_store::recover (raft_log_entry replay).
    _lowest_index = get_lowest_index();
    _highest_index = get_highest_index();
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

private:
  fastblock::aos::log_store* _aos_log{nullptr}; // not owned
  uint64_t _lowest_index = 1;
  uint64_t _trim_index = 1;
  uint64_t _highest_index = 0;

  void append_aos(std::vector<log_entry_t>& entries, log_op_complete cb_fn,
                  void* arg);
  void read_aos(uint64_t start_index, uint64_t end_index,
                log_op_with_entry_complete cb_fn, void* arg);
};

using make_disklog_complete =
    std::function<void(void* arg, struct disk_log* dlog, int rberrno)>;

/** Production: disk_log backed by AOS log_store. log_store not owned. */
void make_aos_disk_log(fastblock::aos::log_store* log,
                       make_disklog_complete cb_fn, void* arg);
