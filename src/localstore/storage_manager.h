#pragma once

#include "kv_store.h"

#include <spdk/string.h>
#include <assert.h>

class storage_manager;

storage_manager& global_storage();


using storage_op_complete = std::function<void (void *, int)>;

void storage_init(storage_op_complete cb_fn, void* arg);

void storage_fini(storage_op_complete cb_fn, void* arg);

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
                SPDK_ERRLOG("storage start failed. error:%s\n", spdk_strerror(error));
                cb_fn(arg, error);
                return;
            }

            this->_kvstore = kvs;
            this->_started = true;
            cb_fn(arg, 0);
            return;
        }, arg
      );
  }

  void stop(storage_op_complete cb_fn, void* arg) {
    _kvstore->stop(
        [this, cb_fn = std::move(cb_fn)](void *arg, int error){
            if (error) {
                SPDK_ERRLOG("storage stop failed. error:%s\n", spdk_strerror(error));
            }

            cb_fn(arg, error);
            delete _kvstore;
            return;
        }, arg
    );
  }

  kvstore* kvs() {
      assert(_started);
      return _kvstore; 
  }

private:
  kvstore* _kvstore;
  bool _started = false;
};