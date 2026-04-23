#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/string.h>

#include "kfastblock/common.h"
#include "kfastblock/meta.h"
#include "kfastblock/transport.h"

int kfastblock_meta_init(void)
{
	return 0;
}

void kfastblock_meta_exit(void)
{
}

int kfastblock_meta_bootstrap(struct kfastblock_cluster_view *view,
			      const struct kfastblock_attach_spec *spec)
{
	int ret;

	if (!view || !spec)
		return -EINVAL;

	memset(view, 0, sizeof(*view));
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
	view->sync_state = KFASTBLOCK_META_SYNC_NEW;

	ret = kfastblock_transport_fetch_cluster_view(view, spec);
	if (!ret) {
		view->sync_state = KFASTBLOCK_META_SYNC_READY;
		view->last_refresh_jiffies = jiffies;
		return 0;
	}

	if (ret == -EOPNOTSUPP && view->image.size_bytes) {
		view->sync_state = KFASTBLOCK_META_SYNC_STALE;
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
	}

	return ret;
}

bool kfastblock_meta_ready(const struct kfastblock_cluster_view *view)
{
	return view && view->image.size_bytes && view->image.object_size &&
		view->image.block_size && view->image.pg_count;
}
