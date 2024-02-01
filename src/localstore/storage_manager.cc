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

#include "storage_manager.h"

#include <memory>

static storage_manager g_st_mgr;

storage_manager& global_storage() {
    return g_st_mgr;
}

void
storage_init(storage_op_complete cb_fn, void* arg) {
  struct spdk_bs_dev *bs_dev = NULL;

  SPDK_NOTICELOG("storage_init\n");
  std::construct_at(&g_st_mgr);
  g_st_mgr.start(std::move(cb_fn), arg);
}

void
storage_fini(storage_op_complete cb_fn, void* arg) {
  struct spdk_bs_dev *bs_dev = NULL;

  SPDK_NOTICELOG("storage_fini\n");
  g_st_mgr.stop(
    [cb_fn = std::move(cb_fn)](void *arg, int error){
        if (error) {
            SPDK_ERRLOG("storage_fini. error:%s\n", spdk_strerror(error));
        }

        cb_fn(arg, error);
        std::destroy_at(&g_st_mgr);
        return;
    }, arg
  );
}
