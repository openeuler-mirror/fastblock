#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kfastblock/common.h"
#include "kfastblock/meta.h"
#include "kfastblock/transport.h"
#include "kfastblock/volume.h"

static void kfastblock_meta_free_osds(struct kfastblock_osd_endpoint *osds,
				      u32 osd_count)
{
	u32 i;

	if (!osds)
		return;

	for (i = 0; i < osd_count; ++i)
		kfree(osds[i].shards);
	kvfree(osds);
}

static void kfastblock_meta_free_routes(struct kfastblock_pg_route *routes,
					u32 route_count)
{
	u32 i;

	if (!routes)
		return;

	for (i = 0; i < route_count; ++i)
		kfree(routes[i].osd_ids);
	kvfree(routes);
}

static bool kfastblock_pg_route_has_osd(const struct kfastblock_pg_route *route,
					u32 osd_id)
{
	u32 i;

	if (!route)
		return false;

	for (i = 0; i < route->replica_count; ++i) {
		if (route->osd_ids[i] == osd_id)
			return true;
	}

	return false;
}

static void kfastblock_meta_copy_leader_cache(struct kfastblock_pg_route *dst,
					      u32 dst_count,
					      const struct kfastblock_pg_route *src,
					      u32 src_count)
{
	u32 dst_idx = 0;
	u32 src_idx = 0;

	while (dst_idx < dst_count && src_idx < src_count) {
		const struct kfastblock_pg_route *src_route = &src[src_idx];
		struct kfastblock_pg_route *dst_route = &dst[dst_idx];

		if (src_route->pool_id == dst_route->pool_id &&
		    src_route->pg_id == dst_route->pg_id) {
			dst_route->leader_valid = src_route->leader_valid;
			if (src_route->leader_valid)
				dst_route->leader = src_route->leader;
			++src_idx;
			++dst_idx;
			continue;
		}
		if (src_route->pool_id < dst_route->pool_id ||
		    (src_route->pool_id == dst_route->pool_id &&
		     src_route->pg_id < dst_route->pg_id)) {
			++src_idx;
			continue;
		}
		++dst_idx;
	}
}

static int kfastblock_meta_prepare_bootstrap_view(
	struct kfastblock_cluster_view *view,
	const struct kfastblock_attach_spec *spec)
{
	if (!view || !spec)
		return -EINVAL;

	kfastblock_meta_cleanup_view(view);
	strscpy(view->image.pool_name, spec->pool_name,
		sizeof(view->image.pool_name));
	strscpy(view->image.image_name, spec->image_name,
		sizeof(view->image.image_name));
	view->image.block_size = KFASTBLOCK_DEFAULT_BLOCK_SIZE;
	view->image.object_size = spec->debug_object_size ?
		spec->debug_object_size : KFASTBLOCK_DEFAULT_OBJECT_SIZE;
	view->image.size_bytes = spec->debug_size_bytes;
	view->image.pool_id = spec->debug_pool_id;
	view->image.pg_count = spec->debug_pg_count ? spec->debug_pg_count : 1;
	view->image.read_only = spec->read_only;
	view->last_refresh_jiffies = jiffies;
	view->last_image_refresh_jiffies = jiffies;
	view->sync_state = KFASTBLOCK_META_SYNC_NEW;
	return 0;
}

int kfastblock_meta_init(void)
{
	return 0;
}

void kfastblock_meta_exit(void)
{
}

void kfastblock_meta_cleanup_view(struct kfastblock_cluster_view *view)
{
	if (!view)
		return;

	kfastblock_meta_free_osds(view->osds, view->osd_count);
	kfastblock_meta_free_routes(view->routes, view->route_count);
	memset(view, 0, sizeof(*view));
}

int kfastblock_meta_bootstrap(struct kfastblock_cluster_view *view,
			      const struct kfastblock_attach_spec *spec)
{
	int ret;

	if (!view || !spec)
		return -EINVAL;

	ret = kfastblock_meta_prepare_bootstrap_view(view, spec);
	if (ret)
		return ret;

	ret = kfastblock_transport_fetch_cluster_view(view, spec);
	if (!ret) {
		view->sync_state = KFASTBLOCK_META_SYNC_READY;
		view->last_refresh_jiffies = jiffies;
		view->last_image_refresh_jiffies = jiffies;
		return 0;
	}

	if (ret == -EOPNOTSUPP && view->image.size_bytes) {
		view->sync_state = KFASTBLOCK_META_SYNC_STALE;
		return 0;
	}

	return ret;
}

int kfastblock_meta_bootstrap_volume(struct kfastblock_volume *vol)
{
	int ret;

	if (!vol)
		return -EINVAL;

	ret = kfastblock_meta_prepare_bootstrap_view(&vol->view, &vol->spec);
	if (ret)
		return ret;

	ret = kfastblock_transport_refresh_image_volume(vol);
	if (!ret)
		ret = kfastblock_transport_refresh_cluster_map_volume(vol);
	if (!ret) {
		vol->view.sync_state = KFASTBLOCK_META_SYNC_READY;
		vol->view.last_refresh_jiffies = jiffies;
		vol->view.last_image_refresh_jiffies = jiffies;
		return 0;
	}

	if (ret == -EOPNOTSUPP && vol->view.image.size_bytes) {
		vol->view.sync_state = KFASTBLOCK_META_SYNC_STALE;
		return 0;
	}

	return ret;
}

int kfastblock_meta_refresh(struct kfastblock_cluster_view *view,
			    const struct kfastblock_attach_spec *spec)
{
	int ret;

	if (!view || !spec)
		return -EINVAL;

	ret = kfastblock_transport_fetch_cluster_view(view, spec);
	if (!ret) {
		view->sync_state = KFASTBLOCK_META_SYNC_READY;
		view->last_refresh_jiffies = jiffies;
		view->last_image_refresh_jiffies = jiffies;
	}

	return ret;
}

int kfastblock_meta_refresh_cluster_map(struct kfastblock_cluster_view *view,
					const struct kfastblock_attach_spec *spec)
{
	int ret;

	if (!view || !spec)
		return -EINVAL;

	ret = kfastblock_transport_fetch_cluster_map(view, spec);
	if (!ret) {
		view->sync_state = KFASTBLOCK_META_SYNC_READY;
		view->last_refresh_jiffies = jiffies;
	}

	return ret;
}

int kfastblock_meta_refresh_image(struct kfastblock_cluster_view *view,
				  const struct kfastblock_attach_spec *spec)
{
	int ret;

	if (!view || !spec)
		return -EINVAL;

	ret = kfastblock_transport_fetch_image(view, spec);
	if (!ret)
		view->last_image_refresh_jiffies = jiffies;

	return ret;
}

int kfastblock_meta_replace_cluster_map(struct kfastblock_cluster_view *view,
					u64 osdmap_epoch,
					u64 pgmap_epoch,
					u32 pool_pg_count,
					u32 osd_count,
					struct kfastblock_osd_endpoint *osds,
					u32 route_count,
					struct kfastblock_pg_route *routes)
{
	struct kfastblock_cluster_view old_view = {};
	bool maps_changed;

	if (!view)
		return -EINVAL;

	old_view.osd_count = view->osd_count;
	old_view.osds = view->osds;
	old_view.route_count = view->route_count;
	old_view.routes = view->routes;
	maps_changed = view->osdmap_epoch != osdmap_epoch ||
		       view->pgmap_epoch != pgmap_epoch;

	view->osdmap_epoch = osdmap_epoch;
	view->pgmap_epoch = pgmap_epoch;
	view->osd_count = osd_count;
	view->osds = osds;
	view->route_count = route_count;
	view->routes = routes;
	view->image.pg_count = pool_pg_count;
	if (!maps_changed)
		kfastblock_meta_copy_leader_cache(view->routes, view->route_count,
						 old_view.routes,
						 old_view.route_count);
	if (maps_changed || !view->leader_epoch)
		++view->leader_epoch;

	kfastblock_meta_cleanup_view(&old_view);
	return 0;
}

bool kfastblock_meta_ready(const struct kfastblock_cluster_view *view)
{
	return view && view->image.size_bytes && view->image.object_size &&
		view->image.block_size && view->image.pg_count;
}

bool kfastblock_meta_io_ready(const struct kfastblock_cluster_view *view)
{
	return kfastblock_meta_ready(view) &&
		view->sync_state == KFASTBLOCK_META_SYNC_READY;
}

const struct kfastblock_osd_endpoint *
kfastblock_meta_find_osd(const struct kfastblock_cluster_view *view, u32 osd_id)
{
	u32 low = 0;
	u32 high;

	if (!view || !view->osds || !view->osd_count)
		return NULL;

	high = view->osd_count;
	while (low < high) {
		u32 mid = low + ((high - low) >> 1);
		const struct kfastblock_osd_endpoint *osd = &view->osds[mid];

		if (osd->osd_id < osd_id) {
			low = mid + 1;
			continue;
		}
		high = mid;
	}

	if (low < view->osd_count && view->osds[low].osd_id == osd_id)
		return &view->osds[low];

	return NULL;
}

const struct kfastblock_osd_shard *
kfastblock_meta_find_osd_shard(const struct kfastblock_osd_endpoint *osd,
			       u32 shard_id)
{
	u32 i;

	if (!osd || !osd->shards || !osd->shard_count)
		return NULL;

	for (i = 0; i < osd->shard_count; ++i) {
		if (osd->shards[i].shard_id == shard_id)
			return &osd->shards[i];
	}

	return NULL;
}

const struct kfastblock_pg_route *
kfastblock_meta_find_pg_route(const struct kfastblock_cluster_view *view,
			      u32 pool_id, u32 pg_id)
{
	u32 low = 0;
	u32 high;

	if (!view || !view->routes || !view->route_count)
		return NULL;

	high = view->route_count;
	while (low < high) {
		u32 mid = low + ((high - low) >> 1);
		const struct kfastblock_pg_route *route = &view->routes[mid];

		if (route->pool_id < pool_id ||
		    (route->pool_id == pool_id && route->pg_id < pg_id)) {
			low = mid + 1;
			continue;
		}
		high = mid;
	}

	if (low < view->route_count &&
	    view->routes[low].pool_id == pool_id &&
	    view->routes[low].pg_id == pg_id)
		return &view->routes[low];

	return NULL;
}

int kfastblock_meta_resolve_pg_target(const struct kfastblock_cluster_view *view,
				      u32 pool_id, u32 pg_id, u32 osd_id,
				      const struct kfastblock_pg_route **route,
				      const struct kfastblock_osd_endpoint **osd,
				      const struct kfastblock_osd_shard **shard)
{
	const struct kfastblock_pg_route *resolved_route;
	const struct kfastblock_osd_endpoint *resolved_osd;
	const struct kfastblock_osd_shard *resolved_shard;

	if (route)
		*route = NULL;
	if (osd)
		*osd = NULL;
	if (shard)
		*shard = NULL;
	if (!view)
		return -EINVAL;

	resolved_route = kfastblock_meta_find_pg_route(view, pool_id, pg_id);
	if (!resolved_route || !kfastblock_pg_route_has_osd(resolved_route, osd_id))
		return -ENOENT;

	resolved_osd = kfastblock_meta_find_osd(view, osd_id);
	if (!resolved_osd)
		return -ENOENT;

	resolved_shard = kfastblock_meta_find_osd_shard(
		resolved_osd, resolved_route->primary_shard);
	if (!resolved_shard)
		return -ENOENT;

	if (route)
		*route = resolved_route;
	if (osd)
		*osd = resolved_osd;
	if (shard)
		*shard = resolved_shard;

	return 0;
}

int kfastblock_meta_get_pg_leader(const struct kfastblock_cluster_view *view,
				  u32 pool_id, u32 pg_id,
				  struct kfastblock_leader_info *leader)
{
	const struct kfastblock_pg_route *route;

	if (!view || !leader)
		return -EINVAL;

	leader->osd_id = 0;
	leader->port = 0;
	leader->address[0] = '\0';

	route = kfastblock_meta_find_pg_route(view, pool_id, pg_id);
	if (!route || !route->leader_valid)
		return -ENOENT;

	*leader = route->leader;
	return 0;
}

int kfastblock_meta_set_pg_leader(struct kfastblock_cluster_view *view,
				  u32 pool_id, u32 pg_id,
				  const struct kfastblock_leader_info *leader)
{
	struct kfastblock_pg_route *route;

	if (!view || !leader)
		return -EINVAL;

	route = (struct kfastblock_pg_route *)
		kfastblock_meta_find_pg_route(view, pool_id, pg_id);
	if (!route)
		return -ENOENT;

	route->leader = *leader;
	route->leader_valid = true;
	return 0;
}

void kfastblock_meta_invalidate_pg_leader(struct kfastblock_cluster_view *view,
					  u32 pool_id, u32 pg_id)
{
	struct kfastblock_pg_route *route;

	if (!view)
		return;

	route = (struct kfastblock_pg_route *)
		kfastblock_meta_find_pg_route(view, pool_id, pg_id);
	if (!route)
		return;

	route->leader_valid = false;
	memset(&route->leader, 0, sizeof(route->leader));
}

void kfastblock_meta_invalidate_all_pg_leaders(struct kfastblock_cluster_view *view)
{
	u32 i;
	bool invalidated = false;

	if (!view || !view->routes)
		return;

	for (i = 0; i < view->route_count; ++i) {
		struct kfastblock_pg_route *route = &view->routes[i];

		if (!route->leader_valid)
			continue;
		route->leader_valid = false;
		memset(&route->leader, 0, sizeof(route->leader));
		invalidated = true;
	}
	if (invalidated || !view->leader_epoch)
		++view->leader_epoch;
}
