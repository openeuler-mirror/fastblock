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

#define KFASTBLOCK_OSD_FLAG_IN (1U << 0)
#define KFASTBLOCK_OSD_FLAG_UP (1U << 1)

struct kfastblock_image_info {
	char pool_name[KFASTBLOCK_MAX_NAME_LEN];
	char image_name[KFASTBLOCK_MAX_NAME_LEN];
	u64 size_bytes;
	u32 block_size;
	u32 object_size;
	u32 pool_id;
	u32 pg_count;
	bool read_only;
};

struct kfastblock_osd_shard {
	u32 shard_id;
	u16 port;
	u16 core_id;
};

struct kfastblock_osd_endpoint {
	u32 osd_id;
	u32 flags;
	u16 shard_count;
	char address[KFASTBLOCK_MAX_ADDR_LEN];
	struct kfastblock_osd_shard *shards;
};

struct kfastblock_leader_info {
	u32 osd_id;
	u16 port;
	char address[KFASTBLOCK_MAX_ADDR_LEN];
};

struct kfastblock_pg_route {
	u32 pool_id;
	u32 pg_id;
	u64 version;
	u32 state;
	u32 primary_shard;
	u32 replica_count;
	u32 *osd_ids;
	bool leader_valid;
	struct kfastblock_leader_info leader;
};

struct kfastblock_cluster_view {
	struct kfastblock_image_info image;
	u64 image_epoch;
	u64 osdmap_epoch;
	u64 pgmap_epoch;
	u64 leader_epoch;
	u32 osd_count;
	u32 route_count;
	unsigned long last_refresh_jiffies;
	enum kfastblock_meta_sync_state sync_state;
	struct kfastblock_osd_endpoint *osds;
	struct kfastblock_pg_route *routes;
};

int kfastblock_meta_init(void);
void kfastblock_meta_exit(void);
void kfastblock_meta_cleanup_view(struct kfastblock_cluster_view *view);
int kfastblock_meta_bootstrap(struct kfastblock_cluster_view *view,
			      const struct kfastblock_attach_spec *spec);
int kfastblock_meta_refresh(struct kfastblock_cluster_view *view,
			    const struct kfastblock_attach_spec *spec);
int kfastblock_meta_replace_cluster_map(struct kfastblock_cluster_view *view,
					u64 osdmap_epoch,
					u64 pgmap_epoch,
					u32 pool_pg_count,
					u32 osd_count,
					struct kfastblock_osd_endpoint *osds,
					u32 route_count,
					struct kfastblock_pg_route *routes);
bool kfastblock_meta_ready(const struct kfastblock_cluster_view *view);
bool kfastblock_meta_io_ready(const struct kfastblock_cluster_view *view);
const struct kfastblock_osd_endpoint *
kfastblock_meta_find_osd(const struct kfastblock_cluster_view *view, u32 osd_id);
const struct kfastblock_osd_shard *
kfastblock_meta_find_osd_shard(const struct kfastblock_osd_endpoint *osd,
			       u32 shard_id);
const struct kfastblock_pg_route *
kfastblock_meta_find_pg_route(const struct kfastblock_cluster_view *view,
			      u32 pool_id, u32 pg_id);
int kfastblock_meta_resolve_pg_target(const struct kfastblock_cluster_view *view,
				      u32 pool_id, u32 pg_id, u32 osd_id,
				      const struct kfastblock_pg_route **route,
				      const struct kfastblock_osd_endpoint **osd,
				      const struct kfastblock_osd_shard **shard);
int kfastblock_meta_get_pg_leader(const struct kfastblock_cluster_view *view,
				  u32 pool_id, u32 pg_id,
				  struct kfastblock_leader_info *leader);
int kfastblock_meta_set_pg_leader(struct kfastblock_cluster_view *view,
				  u32 pool_id, u32 pg_id,
				  const struct kfastblock_leader_info *leader);
void kfastblock_meta_invalidate_pg_leader(struct kfastblock_cluster_view *view,
					  u32 pool_id, u32 pg_id);

#endif
