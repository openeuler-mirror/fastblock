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

#include "object_store.h"
#include "spdk_buffer.h"

#include <spdk/log.h>
#include <vector>

using recovery_op_complete = std::function<void (void *arg, int rerrno)>;

class object_recovery;

struct recovery_op_ctx {
    object_recovery *recv;
    object_store *obs;

    recovery_op_complete cb_fn;
    void* arg;
};


class object_recovery {
public:
  object_recovery(object_store *obs) : _obs(obs) { }

  /**
   * 创建所有对象的recovery snapshot。
   */
  void recovery_create(recovery_op_complete cb_fn, void* arg) {
      _obj_names.reserve(_obs->table.size());
      for (auto& pr : _obs->table) {
          _obj_names.emplace_back(pr.first);
      }
      recovery_op_ctx* ctx = new recovery_op_ctx{.recv = this, .obs = _obs,
                                    .cb_fn = std::move(cb_fn), .arg = arg};
      iter_start();
      auto next_name = iter_next_name();
      // SPDK_NOTICELOG("object name:%s create recovery.\n", next_name.c_str());
      _obs->recovery_create(next_name, recovery_create_continue, ctx);
  }

  static void recovery_create_continue(void *arg, int rerrno) {
      struct recovery_op_ctx* ctx = (struct recovery_op_ctx*)arg;

      if (rerrno) {
          ctx->cb_fn(ctx->arg, rerrno);
          delete ctx;
          return;
      }

      if (!ctx->recv->iter_is_end()) {
          auto next_name = ctx->recv->iter_next_name();
          // SPDK_NOTICELOG("object name:%s create recovery.\n", next_name.c_str());
          ctx->obs->recovery_create(next_name, recovery_create_continue, ctx);
          return;
      }

      ctx->cb_fn(ctx->arg, 0);
      delete ctx;
  }

  std::vector<std::string> recovery_get_obj_names(size_t start, size_t num){
      std::vector<std::string> object_names;
      size_t index = start;
      size_t end = start + num;

      while(!iter_is_end(index) && index < end){
          object_names.emplace_back(get_iter_name(index));
          index++;
      }
      return std::move(object_names);
  }

  /**
   * 每次读取一个对象的全部 4_MB 数据。迭代结束时，errno返回-ENOENT。
   */
  void recovery_read_iter_first(recovery_op_complete cb_fn, char* buf, void* arg) {
      iter_start();
      if (iter_is_end()) {
          cb_fn(arg, -ENOENT);
          return ;
      }
      auto next_name = iter_next_name();
      SPDK_NOTICELOG("object name:%s read.\n", next_name.c_str());
      _obs->recovery_read(next_name, buf, std::move(cb_fn), arg);
  }

  void recovery_read_iter_next(recovery_op_complete cb_fn, char* buf, void* arg) {
      if (iter_is_end()) {
          cb_fn(arg, -ENOENT);
          return;
      }
      auto next_name = iter_next_name();
      SPDK_NOTICELOG("object name:%s read.\n", next_name.c_str());
      _obs->recovery_read(next_name, buf, std::move(cb_fn), arg);
  }

  /**
   * 删除所有对象的recovery snapshot。
   */
  void recovery_delete(recovery_op_complete cb_fn, void* arg) {
      recovery_op_ctx* ctx = new recovery_op_ctx{.recv = this, .obs = _obs,
                                    .cb_fn = std::move(cb_fn), .arg = arg};
      iter_start();
      auto next_name = iter_next_name();
      // SPDK_NOTICELOG("object name:%s delete.\n", next_name.c_str());
      _obs->recovery_delete(next_name, recovery_delete_continue, ctx);
  }

  static void recovery_delete_continue(void *arg, int rerrno) {
      struct recovery_op_ctx* ctx = (struct recovery_op_ctx*)arg;

      if (rerrno) {
          ctx->cb_fn(ctx->arg, rerrno);
          delete ctx;
          return;
      }

      if (!ctx->recv->iter_is_end()) {
          auto next_name = ctx->recv->iter_next_name();
          // SPDK_NOTICELOG("object name:%s delete.\n", next_name.c_str());
          ctx->obs->recovery_delete(next_name, recovery_delete_continue, ctx);
          return;
      }

      ctx->cb_fn(ctx->arg, 0);
      delete ctx;
  }

  void iter_start() { idx = 0; }

  std::string& iter_next_name() { return _obj_names[idx++]; }

  bool iter_is_end() { return idx == _obj_names.size(); }

  bool iter_is_end(size_t index) { return index == _obj_names.size(); }
  std::string& get_iter_name(size_t index)  { return _obj_names[index]; }
  size_t get_iter_idx() { return idx; }

private:
  object_store *_obs;
  std::vector<std::string> _obj_names;
  size_t idx = 0;
};
