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

storage_manager& global_storage();


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

  void start(storage_op_complete cb_fn, void* arg) {
      make_kvstore(global_blobstore(), global_io_channel(),
        [this, cb_fn = std::move(cb_fn)](void *arg, kvstore* kvs, int error){
            if (error) {
                SPDK_ERRLOG_EX("storage start failed. error:%s\n", spdk_strerror(error));
                cb_fn(arg, error);
                return;
            }

            this->_kvstore = kvs;
            this->_started = true;
            cb_fn(arg, 0);
            SPDK_NOTICELOG_EX("The storage manager has been started\n");
            return;
        }, arg
      );
  }

  void stop(storage_op_complete cb_fn, void* arg) {
    if(!_kvstore){
        cb_fn(arg, 0);
        return;
    }
    _kvstore->stop(
        [this, cb_fn = std::move(cb_fn)](void *arg, int error){
            if (error) {
                SPDK_ERRLOG_EX("storage stop failed. error:%s\n", spdk_strerror(error));
            }

            cb_fn(arg, error);
            delete _kvstore;
            SPDK_NOTICELOG_EX("The storage manager has been stopped\n");
            return;
        }, arg
    );
  }

  void load(spdk_blob_id blob_id, spdk_blob_id checkpoint_blob_id, spdk_blob_id new_checkpoint_blob_id, 
          storage_op_complete cb_fn, void* arg){
      load_kvstore(blob_id, checkpoint_blob_id, new_checkpoint_blob_id, global_blobstore(), global_io_channel(),
        [this, cb_fn = std::move(cb_fn)](void *arg, kvstore* kvs, int error){
            if (error) {
                SPDK_ERRLOG_EX("load storage failed. error:%s\n", spdk_strerror(error));
                cb_fn(arg, error);
                return;
            }

            this->_kvstore = kvs;
            this->_started = true;
            cb_fn(arg, 0);
            return;
        }, 
      arg);
  }

  kvstore* kvs() {
      assert(_started);
      return _kvstore;
  }

private:
  kvstore* _kvstore;
  bool _started = false;
};