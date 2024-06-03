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
#include "utils/simple_poller.h"

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
using snap_create_ctx_id_type = uint64_t;

class object_store {
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

    using container = absl::flat_hash_map<std::string, object>;

    struct snap_create_ctx {
        std::string* object_name;
        std::string* snap_name;
        int64_t epoch;
        object_rw_complete cb_fn;
        void* arg;
        snap_create_ctx_id_type id;
        object_store::snap snap;
        object_store::container* hashtable;
        object_snap_xattr xattr;
        ::spdk_blob_id bolb_id;
        object_store* this_ptr;
    };

    struct erase_snap_rd_ctx {
        object_store* this_ptr{nullptr};
        snap_create_ctx_id_type id{};
    };

private:

    static constexpr uint32_t blob_cluster = 4;
    static constexpr uint32_t cluster_size = 1024 * 1024;
    static constexpr uint32_t blob_size = blob_cluster * cluster_size;
    static constexpr uint32_t unit_size = 512;

public:
    // 此处有坑，channel 必须从外部传进 object_store 才可以。
    // 不可以在 object_store 内部调用 spdk_bs_alloc_io_channel，否则bs无法正常unload
    object_store(struct spdk_blob_store *bs, struct spdk_io_channel *channel)
      : bs(bs)
      , channel(channel) {
        if (not _thread) {
            SPDK_ERRLOG_EX("get spdk thread failed\n");
            throw std::runtime_error{"get spdk thread failed"};
        }

        _core_poller.register_poller(core_poller, this, 0);
    }

    ~object_store() noexcept = default;

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

    void snap_create(
      std::string* object_name,
      std::string* snap_name,
      int64_t epoch,
      object_rw_complete cb_fn,
      void* arg);

    void snap_create_bulk();

    void snap_delete(std::string object_name, std::string snap_name,
                    object_rw_complete cb_fn, void* arg);

    void recovery_create(std::map<std::string, xattr_val_type>& xattr, std::string object_name,
                    object_rw_complete cb_fn, void* arg);

    void recovery_read(std::string object_name, char* buf,
                    object_rw_complete cb_fn, void* arg);

    void recovery_delete(std::string object_name,
                    object_rw_complete cb_fn, void* arg);

    std::optional<int64_t> snapshot_latest_epoch(const std::string&);

    void destroy(object_rw_complete cb_fn, void* arg);

    int handle_core_poll();

public:

    void do_erase_creating_snap_rd(const snap_create_ctx_id_type id) {
        _creating_snaps.erase(id);
    }

public:

    static int core_poller(void* arg) {
        auto* ctx = reinterpret_cast<object_store*>(arg);
        return ctx->handle_core_poll();
    }

    static void on_snap_create(void* arg) {
        auto* ctx = reinterpret_cast<snap_create_ctx*>(arg);
        ctx->this_ptr->handle_snap_create(std::unique_ptr<snap_create_ctx>{ctx});
    }

    static void handle_erase_creating_snap_rd(void* arg) {
        auto* ctx = reinterpret_cast<erase_snap_rd_ctx*>(arg);
        ctx->this_ptr->do_erase_creating_snap_rd(ctx->id);
        delete ctx;
    }

public:

    auto erase_creating_snap_rd(const snap_create_ctx_id_type id) {
        auto* ctx = new erase_snap_rd_ctx{this, id};
        ::spdk_thread_send_msg(_thread, handle_erase_creating_snap_rd, ctx);
    }

    void handle_snap_create(std::unique_ptr<snap_create_ctx> ctx) {
        if (_is_termianted) { return; }
        _snap_create_list.push_back(std::move(ctx));
    }

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

  void do_create_snapshot(snap_create_ctx*);
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

    static void get_page_parameters(
      uint64_t offset,
      uint64_t length,
      uint64_t *start_lba,
      uint32_t *lba_size,
      uint64_t *num_lba) {
        uint64_t end_lba;

        *lba_size = object_store::unit_size;
        *start_lba = offset / *lba_size;
        end_lba = (offset + length - 1) / *lba_size;
        *num_lba = (end_lba - *start_lba + 1);
    }

public:
  //快照版本链表的结点。

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

private:

    bool _is_termianted{false};
    ::spdk_thread* _thread{::spdk_get_thread()};
    utils::simple_poller _core_poller{};
    snap_create_ctx_id_type _snap_create_id_gen{0};
    std::list<std::unique_ptr<snap_create_ctx>> _snap_create_list{};
    std::unordered_map<snap_create_ctx_id_type, std::unique_ptr<snap_create_ctx>> _creating_snaps{};
};
