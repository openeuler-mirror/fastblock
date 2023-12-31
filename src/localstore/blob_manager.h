/* Copyright (c) 2023 ChinaUnicom
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

#include "base/core_sharded.h"

#include <spdk/blob_bdev.h>
#include <spdk/blob.h>

#include <vector>
#include <functional>

/// TODO(sunyifang): 现在都是单核的
struct spdk_blob_store* global_blobstore();

struct spdk_io_channel* global_io_channel();

using bm_complete = void (*) (void *arg, int rberrno);

void
blobstore_init(const char *bdev_name, bm_complete cb_fn, void* args);

void
blobstore_fini(bm_complete cb_fn, void* args);
