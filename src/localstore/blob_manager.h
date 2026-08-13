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
 * Blobstore lifecycle stubs (design v2 P4).
 *
 * Production OSD is AOS-only and never opens a blobstore. These symbols remain
 * so shutdown paths and residual includes compile; init/load fail closed.
 * Full blob implementation archived under localstore/legacy_blob/.
 */

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct spdk_blob_store;
struct spdk_io_channel;

/** Always null on AOS-only production path. */
struct spdk_blob_store* global_blobstore(uint32_t shard_id);

/** Always null on AOS-only production path. */
struct spdk_io_channel* global_io_channel(uint32_t shard_id);

/** Always false — blobstore never initialized in production. */
bool blobstore_is_initialized();

extern std::vector<std::pair<uint64_t, uint64_t>> g_bs_space;

using bm_complete = std::function<void(void*, int)>;

/** Demo-only; production never calls. Returns -ENOTSUP. */
void blobstore_init(std::string& bdev_name, const std::string& uuid, bool force,
                    bm_complete cb_fn, void* args);

/** No-op when never opened (production always). */
void blobstore_fini(bm_complete cb_fn, void* args);

/** Demo-only; production never calls. Returns -ENOTSUP. */
void blobstore_load(std::string& bdev_name, bm_complete cb_fn, void* args,
                    std::string* osd_uuid = nullptr);
