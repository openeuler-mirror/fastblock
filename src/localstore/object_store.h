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

#include "types.h"

#include <absl/container/flat_hash_map.h>
#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/string.h>

#include <functional>
#include <string>
#include <map>
#include <errno.h>
// typedef void (*object_rw_complete)(void *arg, int errno);
using object_rw_complete = std::function<void (void *arg, int objerrno)>;

class object_store {
  static constexpr uint32_t blob_cluster = 4;
  static constexpr uint32_t cluster_size = 1024 * 1024;
  static constexpr uint32_t blob_size = blob_cluster * cluster_size;
  static constexpr uint32_t unit_size = 512;

public:
  // 此处有坑，channel 必须从外部传进 object_store 才可以。
  // 不可以在 object_store 内部调用 spdk_bs_alloc_io_channel，否则bs无法正常unload
  object_store(struct spdk_blob_store *bs, struct spdk_io_channel *channel)
  : bs(bs)
  , channel(channel) { }

  ~object_store(){
  //   // for (auto& pr : table) {
  //   //   delete pr.second;
  //   // }
  }

  /**
   * 由于要直接读写blob，这里的buf务必用spdk_malloc申请。len的单位是字节，而不是io unit。
   */
  void read(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
            uint64_t offset, char* buf, uint64_t len,
            object_rw_complete cb_fn, void* arg);

  void write(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
             uint64_t offset, char* buf, uint64_t len,
             object_rw_complete cb_fn, void* arg);

  void stop(object_rw_complete cb_fn, void* arg);

  void snap_create(std::map<std::string, xattr_val_type>& xattr, std::string object_name, std::string snap_name,
                   object_rw_complete cb_fn, void* arg);

  void snap_delete(std::string object_name, std::string snap_name,
                   object_rw_complete cb_fn, void* arg);
    
  void recovery_create(std::map<std::string, xattr_val_type>& xattr, std::string object_name, 
                 object_rw_complete cb_fn, void* arg);

  void recovery_read(std::string object_name, char* buf,
                 object_rw_complete cb_fn, void* arg);

  void recovery_delete(std::string object_name,
                 object_rw_complete cb_fn, void* arg);

  bool is_exist(std::string object_name){
      auto it = table.find(object_name);
      return it != table.end();
  }

  void destroy(object_rw_complete cb_fn, void* arg);
private:
  void readwrite(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
                     uint64_t offset, char* buf, uint64_t len,
                     object_rw_complete cb_fn, void* arg, bool is_read);
  void create_blob(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
                     uint64_t offset, char* buf, uint64_t len,
                     object_rw_complete cb_fn, void* arg, bool is_read);

  static void blob_readwrite(struct spdk_blob *blob, struct spdk_io_channel * channel,
                       uint64_t offset, char* buf, uint64_t len,
                       object_rw_complete cb_fn, void* arg, bool is_read);
  // 下面都是一些回调函数
  static void rw_done(void *arg, int objerrno);
  static void read_done(void *arg, int objerrno);
  static void create_done(void *arg, spdk_blob_id blobid, int objerrno);
  static void open_done(void *arg, struct spdk_blob *blob, int objerrno);
  static void close_done(void *arg, int objerrno);
  static void sync_md_done(void *arg, int bserrno);

  static void snap_delete_complete(void *arg, int objerrno);  // 用户主动删除snapshot
  static void snap_create_complete(void *arg, spdk_blob_id snap_id, int objerrno);

  static void recovery_create_complete(void *arg, spdk_blob_id blob_id, int objerrno);
  static void recovery_open_complete(void *arg, struct spdk_blob *blob, int objerrno);
  static void recovery_close_complete(void *arg, int objerrno);
  static void recovery_delete_complete(void *arg, int objerrno);
  static void recovery_read_complete(void *arg, int objerrno);

  static bool is_lba_aligned(uint64_t offset, uint64_t length) {
    uint32_t lba_size = object_store::unit_size;
    if ((offset % lba_size == 0) && (length % lba_size == 0)) {
      return true;
    }
    return false;
  }

  static void get_page_parameters(uint64_t offset, uint64_t length,
		        uint64_t *start_lba, uint32_t *lba_size, uint64_t *num_lba) {
    uint64_t end_lba;

    *lba_size = object_store::unit_size;
    *start_lba = offset / *lba_size;
    end_lba = (offset + length - 1) / *lba_size;
    *num_lba = (end_lba - *start_lba + 1);
  }

public:
  struct snap {
    fb_blob     snap_blob;
    std::string snap_name;
  };
  struct object {
    fb_blob         origin;
    fb_blob         recover;
    std::list<snap> snap_list;
  };
  //快照版本链表的结点。
  using container = absl::flat_hash_map<std::string, object>;

  // 加载的时候把pg也加载进去
  void set_pg(std::string pg) {
    this->pg = std::move(pg);
  }

  void load(container objects){
      table = std::move(objects);
  }
  
  using iterator = container::iterator;
  std::string pg;
  container table;
  struct spdk_blob_store *bs;       // 我们不掌握blob_store的生命周期
  struct spdk_io_channel *channel;  // 所以不用担心这两个指针的free
                            //obiect_store是管理所有块的类
  //std::map<std::string,std::list<snap_Node*>> snap_hashlist;//这个hash表时存储快照的链式hash。
};
