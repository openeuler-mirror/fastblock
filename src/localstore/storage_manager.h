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

#include "kv_store.h"

#include <spdk/string.h>
#include <assert.h>

class storage_manager;

storage_manager& global_storage(uint32_t shard_id);


using storage_op_complete = std::function<void (void *, int)>;

void storage_init(storage_op_complete cb_fn, void* arg);

void storage_fini(storage_op_complete cb_fn, void* arg);

void storage_load(storage_op_complete cb_fn, void* arg);

/**
 * storage_manager保存一个核上的所有存储对象：
 * - kvstore        所有pg共用一个
 * - log_manager    每个pg都有一个disk_log
 * - object_manager 每个pg都有一个object_store
 */
class storage_manager {
public:
  storage_manager() { }

  void start(uint32_t shard_id, storage_op_complete cb_fn, void* arg) {
      // design v2 P4: production is AOS-only. Never create blob kv here;
      // attach_aos_kv() wires the real engine after aos_runtime open/recover.
      // Blob make_kvstore remains for demos that still call blobstore_init.
      (void)shard_id;
      _kvstore = nullptr;
      _started = false;
      SPDK_NOTICELOG(
          "storage start (AOS-only): defer kv until attach_aos_kv shard %u\n",
          shard_id);
      if (cb_fn) {
          cb_fn(arg, 0);
      }
  }

  void stop(storage_op_complete cb_fn, void* arg) {
    if(!_kvstore){
        cb_fn(arg, 0);
        return;
    }
    _kvstore->stop(
        [this, cb_fn = std::move(cb_fn)](void *arg, int error){
            if (error) {
                SPDK_ERRLOG("storage stop failed. error:%s\n", spdk_strerror(error));
            }
            
            SPDK_INFOLOG(storage_log, "The storage manager has been stopped\n");
            /* 注意：kvstore的析构函数里会注销poller,需要在这个kv所属的core里注销
             *（注册、注销poller要在一个thread里，一个cpu core有一个thread，因此注册、注销poller要在一个cpu core里）、
             */
            delete _kvstore;
            cb_fn(arg, error);
            return;
        }, arg
    );
  }

  void load(uint32_t shard_id, spdk_blob_id blob_id, spdk_blob_id checkpoint_blob_id, spdk_blob_id new_checkpoint_blob_id, 
          storage_op_complete cb_fn, void* arg){
      // design v2 P4: ignore blob ids; AOS recover + attach_aos_kv supply kv.
      (void)blob_id;
      (void)checkpoint_blob_id;
      (void)new_checkpoint_blob_id;
      _kvstore = nullptr;
      _started = false;
      SPDK_NOTICELOG(
          "storage load (AOS-only): defer kv until attach_aos_kv shard %u\n",
          shard_id);
      if (cb_fn) {
          cb_fn(arg, 0);
      }
  }

  kvstore* kvs() {
      assert(_started);
      return _kvstore;
  }

  /**
   * Switch to AOS-backed kv (design v2 P2). Replaces existing kvstore.
   * eng not owned; must outlive this manager.
   */
  void attach_aos_kv(fastblock::aos::kv_engine* eng, storage_op_complete cb_fn,
                     void* arg) {
    auto finish = [this, eng, cb_fn = std::move(cb_fn), arg](void*, int) {
      auto* kvs = new kvstore(eng);
      kvs->start();
      _kvstore = kvs;
      _started = true;
      SPDK_NOTICELOG("storage_manager attached AOS kv\n");
      if (cb_fn) {
        cb_fn(arg, 0);
      }
    };
    if (_kvstore) {
      auto* old = _kvstore;
      _kvstore = nullptr;
      old->stop(
          [old, finish = std::move(finish)](void* a, int err) {
            delete old;
            finish(a, err);
          },
          arg);
      return;
    }
    finish(arg, 0);
  }

private:
  kvstore* _kvstore{nullptr};
  bool _started = false;
};