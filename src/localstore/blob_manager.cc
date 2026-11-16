/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "blob_manager.h"

#include <spdk/log.h>

#include <cerrno>

std::vector<std::pair<uint64_t, uint64_t>> g_bs_space;

struct spdk_blob_store* global_blobstore(uint32_t /*shard_id*/) {
  return nullptr;
}

struct spdk_io_channel* global_io_channel(uint32_t /*shard_id*/) {
  return nullptr;
}

bool blobstore_is_initialized() { return false; }

void blobstore_init(std::string& /*bdev_name*/, const std::string& /*uuid*/,
                    bool /*force*/, bm_complete cb_fn, void* args) {
  SPDK_ERRLOG("blobstore_init removed from production (AOS-only P4)\n");
  if (cb_fn) {
    cb_fn(args, -ENOTSUP);
  }
}

void blobstore_fini(bm_complete cb_fn, void* args) {
  // Production never opened a blobstore.
  if (cb_fn) {
    cb_fn(args, 0);
  }
}

void blobstore_load(std::string& /*bdev_name*/, bm_complete cb_fn, void* args,
                    std::string* /*osd_uuid*/) {
  SPDK_ERRLOG("blobstore_load removed from production (AOS-only P4)\n");
  if (cb_fn) {
    cb_fn(args, -ENOTSUP);
  }
}
