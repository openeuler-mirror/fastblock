/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "disk_log.h"
#include "localstore/aos/log_store.h"

#include <cstring>

SPDK_LOG_REGISTER_COMPONENT(disk_log)

namespace {

fastblock::aos::log_entry to_aos_entry(log_entry_t& e) {
  fastblock::aos::log_entry out;
  out.term = e.term_id;
  out.index = e.index;
  out.type = e.type;
  out.meta = e.meta;
  out.data.reserve(e.data.bytes());
  for (auto sbuf : e.data) {
    out.data.append(sbuf.get_buf(), sbuf.size());
  }
  return out;
}

log_entry_t from_aos_entry(const fastblock::aos::log_entry& e) {
  log_entry_t out;
  out.term_id = e.term;
  out.index = e.index;
  out.type = e.type;
  out.meta = e.meta;
  out.size = e.data.size();
  const uint64_t nblocks = out.size == 0 ? 0 : (out.size + 4095) / 4096;
  if (nblocks == 0) {
    return out;
  }
  out.data = make_buffer_list(nblocks);
  size_t off = 0;
  for (auto sbuf : out.data) {
    const size_t n =
        std::min(static_cast<size_t>(4096), e.data.size() - off);
    if (n > 0) {
      sbuf.append(e.data.data() + off, n);
      off += n;
    }
  }
  out.size = nblocks * 4096;
  return out;
}

} // namespace

void disk_log::append_aos(std::vector<log_entry_t>& entries,
                          log_op_complete cb_fn, void* arg) {
  if (_aos_log == nullptr) {
    if (cb_fn) {
      cb_fn(arg, -EINVAL);
    }
    return;
  }
  std::vector<fastblock::aos::log_entry> batch;
  batch.reserve(entries.size());
  for (auto& e : entries) {
    batch.push_back(to_aos_entry(e));
  }
  _aos_log->append_batch(
      std::move(batch),
      [cb_fn = std::move(cb_fn)](void* a, int err) {
        if (cb_fn) {
          cb_fn(a, err);
        }
      },
      arg);
}

void disk_log::read_aos(uint64_t start_index, uint64_t end_index,
                        log_op_with_entry_complete cb_fn, void* arg) {
  if (_aos_log == nullptr) {
    if (cb_fn) {
      cb_fn(arg, {}, -EINVAL);
    }
    return;
  }
  _aos_log->read(
      start_index, end_index,
      [cb_fn = std::move(cb_fn)](void* a,
                                 std::vector<fastblock::aos::log_entry> entries,
                                 int err) {
        if (err != 0) {
          if (cb_fn) {
            cb_fn(a, {}, err);
          }
          return;
        }
        std::vector<log_entry_t> out;
        out.reserve(entries.size());
        for (auto& e : entries) {
          out.push_back(from_aos_entry(e));
        }
        if (cb_fn) {
          cb_fn(a, std::move(out), 0);
        }
      },
      arg);
}

void make_aos_disk_log(fastblock::aos::log_store* log,
                       make_disklog_complete cb_fn, void* arg) {
  if (log == nullptr) {
    if (cb_fn) {
      cb_fn(arg, nullptr, -EINVAL);
    }
    return;
  }
  auto* dlog = new disk_log(log);
  dlog->start();
  if (cb_fn) {
    cb_fn(arg, dlog, 0);
  }
}
