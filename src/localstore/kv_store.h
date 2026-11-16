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
 * kvstore — AOS-only production facade over kv_engine (design v2 P2/P4).
 * Blob rolling_blob + kv_checkpoint path removed from the production library.
 */

#include "localstore/aos/kv_engine.h"
#include "fastblock/base/core_sharded.h"
#include "fastblock/utils/utils.h"

#include <spdk/log.h>

#include <cerrno>
#include <functional>
#include <optional>
#include <string>

class kvstore;

using kvstore_rw_complete = std::function<void(void*, int)>;

class kvstore {
public:
  /** eng not owned; lifetime owned by aos_runtime. */
  explicit kvstore(fastblock::aos::kv_engine* eng) : _aos(eng) {}

  ~kvstore() = default;

  bool is_aos() const { return _aos != nullptr; }

  void start() {
    if (_aos) {
      _aos->start();
      SPDK_INFOLOG(kvlog, "start aos kvstore in shard %u\n",
                   core_sharded::get_core_sharded().this_shard_id());
    }
  }

  void clear() {
    // AOS engine keeps its own table; no local table to clear.
  }

  void stop(kvstore_rw_complete cb_fn, void* arg) {
    // Engine lifetime is owned by aos_runtime::stop (flush then reset).
    // Detach only — a second flush here UAFs after the engine is destroyed.
    _aos = nullptr;
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

  void put(std::string key, std::optional<std::string> value) {
    if (_aos) {
      _aos->put(std::move(key), std::move(value));
    }
  }

  void put_durable(std::string key, std::optional<std::string> value,
                   kvstore_rw_complete cb_fn, void* arg) {
    if (_aos) {
      _aos->put_durable(std::move(key), std::move(value), std::move(cb_fn),
                        arg);
      return;
    }
    if (cb_fn) {
      cb_fn(arg, -ENODEV);
    }
  }

  void remove(std::string key) {
    if (_aos) {
      _aos->remove(std::move(key));
    }
  }

  std::optional<std::string> get(std::string key) {
    if (_aos) {
      return _aos->get(key);
    }
    return std::nullopt;
  }

  bool need_commit() {
    return _aos != nullptr && _aos->pending_ops() > 0;
  }

  void commit(kvstore_rw_complete cb_fn, void* arg) {
    if (_aos) {
      _aos->commit(std::move(cb_fn), arg);
      return;
    }
    if (cb_fn) {
      cb_fn(arg, 0);
    }
  }

  size_t size() {
    return _aos ? _aos->table_size() : 0;
  }

private:
  fastblock::aos::kv_engine* _aos{nullptr}; // not owned
};

using make_kvs_complete =
    std::function<void(void* arg, struct kvstore* kvs, int kverrno)>;

/** Production: AOS-backed kvstore. eng not owned. */
void make_aos_kvstore(fastblock::aos::kv_engine* eng, make_kvs_complete cb_fn,
                      void* arg);
