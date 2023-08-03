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



// 暂时被废弃
/*
class blob_manager {
    blob_manager(struct spdk_blob_store* bs) : blobstore(bs) { }
    void start() {
        channels.resize(core_sharded::get_core_sharded().count());

        for (uint32_t shard = 0; shard < core_sharded::get_core_sharded().count(); shard++) {
            core_sharded::get_core_sharded().invoke_on(shard, 
              [this, shard]{
                  channels[shard] = spdk_bs_alloc_io_channel(blobstore);
              });
        }
    }

    void stop() {
        for (uint32_t shard = 0; shard < core_sharded::get_core_sharded().count(); shard++) {
            core_sharded::get_core_sharded().invoke_on(shard, 
              [this, shard]{
                if (channels[shard]) {
                  spdk_bs_free_io_channel(channels[shard]);
                }
              });
        }
    }

private:
    struct spdk_blob_store* blobstore;
    std::vector<spdk_io_channel*> channels;
};
*/