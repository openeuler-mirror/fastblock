#ifndef KFASTBLOCK_META_H
#define KFASTBLOCK_META_H

#include <linux/types.h>

#include "kfastblock/common.h"
#include "kfastblock/control.h"

enum kfastblock_meta_sync_state {
	KFASTBLOCK_META_SYNC_NEW = 0,
	KFASTBLOCK_META_SYNC_STALE = 1,
	KFASTBLOCK_META_SYNC_READY = 2,
};

struct kfastblock_image_info {
	char pool_name[KFASTBLOCK_MAX_NAME_LEN];
	char image_name[KFASTBLOCK_MAX_NAME_LEN];
	u64 size_bytes;
	u32 block_size;
	u32 object_size;
	u32 pool_id;
};

struct kfastblock_cluster_view {
	struct kfastblock_image_info image;
	u64 image_epoch;
	u64 osdmap_epoch;
	u64 pgmap_epoch;
	u64 leader_epoch;
	unsigned long last_refresh_jiffies;
	enum kfastblock_meta_sync_state sync_state;
};

int kfastblock_meta_init(void);
void kfastblock_meta_exit(void);
int kfastblock_meta_bootstrap(struct kfastblock_cluster_view *view,
			      const struct kfastblock_attach_spec *spec);
int kfastblock_meta_refresh(struct kfastblock_cluster_view *view,
			    const struct kfastblock_attach_spec *spec);
bool kfastblock_meta_ready(const struct kfastblock_cluster_view *view);

#endif
