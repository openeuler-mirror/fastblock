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

#include "base/core_sharded.h"
#include "base/shard_service.h"
#include "object_store.h"

#include <spdk/blob_bdev.h>
#include <spdk/blob.h>

#include <vector>
#include <functional>
#include <map>

/**
 * 每个shard都有一份 blob_tree，只保存这个核上的blob。
 *
 * kv: 每个核上只有一个blob
 * log: 每个pg一个blob，根据 pg 查找到这个blob
 * object: 每个pg一个map，保存了这个pg里面的对象
 */
struct blob_tree {
  spdk_blob_id kv_blob; 
  spdk_blob_id kv_checkpoint_blob; 
  spdk_blob_id kv_new_checkpoint_blob;                          
  std::map<std::string, struct spdk_blob*> log_blobs;
  std::map<std::string, object_store::container> object_blobs;
};

/// TODO(sunyifang): 现在都是单核的
struct spdk_blob_store* global_blobstore();

struct spdk_io_channel* global_io_channel();

sharded<blob_tree>& global_blob_tree();

using bm_complete = std::function<void (void *, int)>;

void
blobstore_init(std::string &bdev_name, const std::string& uuid, 
        bm_complete cb_fn, void* args);

void
blobstore_fini(bm_complete cb_fn, void* args);

void
blobstore_load(std::string &bdev_name, bm_complete cb_fn,
        void* args, std::string *osd_uuid = nullptr);