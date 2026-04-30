#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>

#include "kfastblock/common.h"
#include "kfastblock/meta.h"
#include "kfastblock/request.h"
#include "kfastblock/transport.h"
#include "kfastblock/volume.h"

static LIST_HEAD(g_kfastblock_volumes);
static DEFINE_MUTEX(g_kfastblock_volumes_lock);
static DEFINE_IDA(g_kfastblock_dev_ida);
static struct dentry *g_kfastblock_debugfs_root;

void kfastblock_volume_stats_init(struct kfastblock_volume *vol)
{
	if (!vol)
		return;

	atomic64_set(&vol->stats.io_submitted, 0);
	atomic64_set(&vol->stats.io_completed, 0);
	atomic64_set(&vol->stats.io_failed, 0);
	atomic64_set(&vol->stats.read_requests, 0);
	atomic64_set(&vol->stats.write_requests, 0);
	atomic64_set(&vol->stats.discard_requests, 0);
	atomic64_set(&vol->stats.flush_requests, 0);
	atomic64_set(&vol->stats.write_zeroes_requests, 0);
	atomic64_set(&vol->stats.read_bytes, 0);
	atomic64_set(&vol->stats.write_bytes, 0);
	atomic64_set(&vol->stats.discard_bytes, 0);
	atomic64_set(&vol->stats.queue_pause_events, 0);
	atomic64_set(&vol->stats.queue_resume_events, 0);
	atomic64_set(&vol->stats.metadata_stale_events, 0);
	atomic64_set(&vol->stats.cluster_refresh_ok, 0);
	atomic64_set(&vol->stats.cluster_refresh_fail, 0);
	atomic64_set(&vol->stats.image_refresh_ok, 0);
	atomic64_set(&vol->stats.image_refresh_fail, 0);
	atomic64_set(&vol->stats.leader_query_ok, 0);
	atomic64_set(&vol->stats.leader_query_fail, 0);
	atomic64_set(&vol->stats.object_io_retries, 0);
	atomic64_set(&vol->stats.object_io_errors, 0);
}

void kfastblock_volume_account_io_submit(struct kfastblock_volume *vol,
				       enum req_op op, u32 bytes)
{
	if (!vol)
		return;

	atomic64_inc(&vol->stats.io_submitted);
	switch (op) {
	case REQ_OP_READ:
		atomic64_inc(&vol->stats.read_requests);
		atomic64_add(bytes, &vol->stats.read_bytes);
		break;
	case REQ_OP_WRITE:
		atomic64_inc(&vol->stats.write_requests);
		atomic64_add(bytes, &vol->stats.write_bytes);
		break;
	case REQ_OP_DISCARD:
		atomic64_inc(&vol->stats.discard_requests);
		atomic64_add(bytes, &vol->stats.discard_bytes);
		break;
	case REQ_OP_WRITE_ZEROES:
		atomic64_inc(&vol->stats.write_zeroes_requests);
		atomic64_add(bytes, &vol->stats.discard_bytes);
		break;
	case REQ_OP_FLUSH:
		atomic64_inc(&vol->stats.flush_requests);
		break;
	default:
		break;
	}
}

void kfastblock_volume_account_io_complete(struct kfastblock_volume *vol, int ret)
{
	if (!vol)
		return;

	if (ret)
		atomic64_inc(&vol->stats.io_failed);
	else
		atomic64_inc(&vol->stats.io_completed);
}

void kfastblock_volume_account_object_retry(struct kfastblock_volume *vol)
{
	if (vol)
		atomic64_inc(&vol->stats.object_io_retries);
}

void kfastblock_volume_account_object_error(struct kfastblock_volume *vol)
{
	if (vol)
		atomic64_inc(&vol->stats.object_io_errors);
}

void kfastblock_volume_account_leader_query(struct kfastblock_volume *vol, int ret)
{
	if (!vol)
		return;

	if (ret)
		atomic64_inc(&vol->stats.leader_query_fail);
	else
		atomic64_inc(&vol->stats.leader_query_ok);
}

void kfastblock_volume_account_cluster_refresh(struct kfastblock_volume *vol,
				       int ret)
{
	if (!vol)
		return;

	if (ret)
		atomic64_inc(&vol->stats.cluster_refresh_fail);
	else
		atomic64_inc(&vol->stats.cluster_refresh_ok);
}

void kfastblock_volume_account_image_refresh(struct kfastblock_volume *vol,
				     int ret)
{
	if (!vol)
		return;

	if (ret)
		atomic64_inc(&vol->stats.image_refresh_fail);
	else
		atomic64_inc(&vol->stats.image_refresh_ok);
}

void kfastblock_volume_account_metadata_stale(struct kfastblock_volume *vol)
{
	if (vol)
		atomic64_inc(&vol->stats.metadata_stale_events);
}

static int kfastblock_volume_summary_show(struct seq_file *m, void *v)
{
	struct kfastblock_volume *vol = m->private;

	if (!vol)
		return -ENODEV;

	down_read(&vol->state_lock);
	seq_printf(m, "disk=%s\n", vol->disk ? vol->disk->disk_name : "");
	seq_printf(m, "ready=%d\n", atomic_read(&vol->ready));
	seq_printf(m, "queue_paused=%d\n", vol->queue_paused ? 1 : 0);
	seq_printf(m, "open_count=%d\n", atomic_read(&vol->open_count));
	seq_printf(m, "pool_name=%s\n", vol->view.image.pool_name);
	seq_printf(m, "image_name=%s\n", vol->view.image.image_name);
	seq_printf(m, "size_bytes=%llu\n", vol->view.image.size_bytes);
	seq_printf(m, "block_size=%u\n", vol->view.image.block_size);
	seq_printf(m, "object_size=%u\n", vol->view.image.object_size);
	seq_printf(m, "pool_id=%u\n", vol->view.image.pool_id);
	seq_printf(m, "pg_count=%u\n", vol->view.image.pg_count);
	seq_printf(m, "read_only=%u\n", vol->view.image.read_only ? 1 : 0);
	seq_printf(m, "sync_state=%u\n", vol->view.sync_state);
	seq_printf(m, "image_epoch=%llu\n", vol->view.image_epoch);
	seq_printf(m, "osdmap_epoch=%llu\n", vol->view.osdmap_epoch);
	seq_printf(m, "pgmap_epoch=%llu\n", vol->view.pgmap_epoch);
	seq_printf(m, "leader_epoch=%llu\n", vol->view.leader_epoch);
	seq_printf(m, "osd_count=%u\n", vol->view.osd_count);
	seq_printf(m, "route_count=%u\n", vol->view.route_count);
	seq_printf(m, "last_refresh_jiffies=%lu\n", vol->view.last_refresh_jiffies);
	seq_printf(m, "last_image_refresh_jiffies=%lu\n",
		   vol->view.last_image_refresh_jiffies);
	up_read(&vol->state_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kfastblock_volume_summary);

static int kfastblock_volume_osds_show(struct seq_file *m, void *v)
{
	struct kfastblock_volume *vol = m->private;
	u32 i, j;

	if (!vol)
		return -ENODEV;

	down_read(&vol->state_lock);
	for (i = 0; i < vol->view.osd_count; ++i) {
		const struct kfastblock_osd_endpoint *osd = &vol->view.osds[i];

		seq_printf(m,
			   "osd_id=%u flags=0x%x address=%s shard_count=%u\n",
			   osd->osd_id, osd->flags, osd->address, osd->shard_count);
		for (j = 0; j < osd->shard_count; ++j) {
			seq_printf(m,
				   "  shard_id=%u core_id=%u port=%u\n",
				   osd->shards[j].shard_id,
				   osd->shards[j].core_id,
				   osd->shards[j].port);
		}
	}
	up_read(&vol->state_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kfastblock_volume_osds);

static int kfastblock_volume_routes_show(struct seq_file *m, void *v)
{
	struct kfastblock_volume *vol = m->private;
	u32 i, j;

	if (!vol)
		return -ENODEV;

	down_read(&vol->state_lock);
	for (i = 0; i < vol->view.route_count; ++i) {
		const struct kfastblock_pg_route *route = &vol->view.routes[i];

		seq_printf(m,
			   "pool_id=%u pg_id=%u version=%llu state=0x%x primary_shard=%u replicas=%u leader_valid=%u",
			   route->pool_id, route->pg_id, route->version, route->state,
			   route->primary_shard, route->replica_count,
			   route->leader_valid ? 1 : 0);
		if (route->leader_valid) {
			seq_printf(m,
				   " leader_osd=%u leader_addr=%s leader_port=%u",
				   route->leader.osd_id,
				   route->leader.address,
				   route->leader.port);
		}
		seq_putc(m, '\n');
		for (j = 0; j < route->replica_count; ++j)
			seq_printf(m, "  osd[%u]=%u\n", j, route->osd_ids[j]);
	}
	up_read(&vol->state_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kfastblock_volume_routes);

static int kfastblock_volume_sockets_show(struct seq_file *m, void *v)
{
	struct kfastblock_volume *vol = m->private;
	int i;

	if (!vol)
		return -ENODEV;

	for (i = 0; i < KFASTBLOCK_MAX_SOCKET_CACHE; ++i) {
		const struct kfastblock_cached_socket *cached = &vol->socket_cache[i];

		seq_printf(m,
			   "osd_cache[%d] active=%u osd_id=%u address=%s port=%u next_seq=%llu\n",
			   i, cached->sock ? 1 : 0, cached->osd_id, cached->address,
			   cached->port, cached->next_seq);
	}
	for (i = 0; i < KFASTBLOCK_MAX_MONITORS; ++i) {
		const struct kfastblock_cached_monitor_socket *cached =
			&vol->monitor_cache[i];

		seq_printf(m,
			   "monitor_cache[%d] active=%u address=%s port=%u next_seq=%llu\n",
			   i, cached->sock ? 1 : 0, cached->address, cached->port,
			   cached->next_seq);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kfastblock_volume_sockets);

static int kfastblock_volume_stats_show(struct seq_file *m, void *v)
{
	struct kfastblock_volume *vol = m->private;

	if (!vol)
		return -ENODEV;

	seq_printf(m, "io_submitted=%lld\n",
		   atomic64_read(&vol->stats.io_submitted));
	seq_printf(m, "io_completed=%lld\n",
		   atomic64_read(&vol->stats.io_completed));
	seq_printf(m, "io_failed=%lld\n",
		   atomic64_read(&vol->stats.io_failed));
	seq_printf(m, "read_requests=%lld\n",
		   atomic64_read(&vol->stats.read_requests));
	seq_printf(m, "write_requests=%lld\n",
		   atomic64_read(&vol->stats.write_requests));
	seq_printf(m, "discard_requests=%lld\n",
		   atomic64_read(&vol->stats.discard_requests));
	seq_printf(m, "flush_requests=%lld\n",
		   atomic64_read(&vol->stats.flush_requests));
	seq_printf(m, "write_zeroes_requests=%lld\n",
		   atomic64_read(&vol->stats.write_zeroes_requests));
	seq_printf(m, "read_bytes=%lld\n",
		   atomic64_read(&vol->stats.read_bytes));
	seq_printf(m, "write_bytes=%lld\n",
		   atomic64_read(&vol->stats.write_bytes));
	seq_printf(m, "discard_bytes=%lld\n",
		   atomic64_read(&vol->stats.discard_bytes));
	seq_printf(m, "queue_pause_events=%lld\n",
		   atomic64_read(&vol->stats.queue_pause_events));
	seq_printf(m, "queue_resume_events=%lld\n",
		   atomic64_read(&vol->stats.queue_resume_events));
	seq_printf(m, "metadata_stale_events=%lld\n",
		   atomic64_read(&vol->stats.metadata_stale_events));
	seq_printf(m, "cluster_refresh_ok=%lld\n",
		   atomic64_read(&vol->stats.cluster_refresh_ok));
	seq_printf(m, "cluster_refresh_fail=%lld\n",
		   atomic64_read(&vol->stats.cluster_refresh_fail));
	seq_printf(m, "image_refresh_ok=%lld\n",
		   atomic64_read(&vol->stats.image_refresh_ok));
	seq_printf(m, "image_refresh_fail=%lld\n",
		   atomic64_read(&vol->stats.image_refresh_fail));
	seq_printf(m, "leader_query_ok=%lld\n",
		   atomic64_read(&vol->stats.leader_query_ok));
	seq_printf(m, "leader_query_fail=%lld\n",
		   atomic64_read(&vol->stats.leader_query_fail));
	seq_printf(m, "object_io_retries=%lld\n",
		   atomic64_read(&vol->stats.object_io_retries));
	seq_printf(m, "object_io_errors=%lld\n",
		   atomic64_read(&vol->stats.object_io_errors));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kfastblock_volume_stats);

static void kfastblock_volume_debugfs_init(struct kfastblock_volume *vol)
{
	if (IS_ERR_OR_NULL(g_kfastblock_debugfs_root) || !vol || !vol->disk)
		return;

	vol->debugfs_dir = debugfs_create_dir(vol->disk->disk_name,
				      g_kfastblock_debugfs_root);
	if (IS_ERR_OR_NULL(vol->debugfs_dir)) {
		vol->debugfs_dir = NULL;
		return;
	}

	debugfs_create_file("summary", 0444, vol->debugfs_dir, vol,
			    &kfastblock_volume_summary_fops);
	debugfs_create_file("osds", 0444, vol->debugfs_dir, vol,
			    &kfastblock_volume_osds_fops);
	debugfs_create_file("routes", 0444, vol->debugfs_dir, vol,
			    &kfastblock_volume_routes_fops);
	debugfs_create_file("sockets", 0444, vol->debugfs_dir, vol,
			    &kfastblock_volume_sockets_fops);
	debugfs_create_file("stats", 0444, vol->debugfs_dir, vol,
			    &kfastblock_volume_stats_fops);
}

static void kfastblock_volume_debugfs_exit(struct kfastblock_volume *vol)
{
	if (!vol || !vol->debugfs_dir)
		return;

	debugfs_remove_recursive(vol->debugfs_dir);
	vol->debugfs_dir = NULL;
}

static unsigned long kfastblock_volume_refresh_interval(void)
{
	return msecs_to_jiffies(3000);
}

static unsigned long kfastblock_volume_image_refresh_interval(void)
{
	return msecs_to_jiffies(30000);
}

static void kfastblock_volume_close_osd_cached_sockets(struct kfastblock_volume *vol)
{
	int i;

	if (!vol)
		return;

	for (i = 0; i < KFASTBLOCK_MAX_SOCKET_CACHE; ++i) {
		if (!vol->socket_cache[i].sock)
			continue;
		sock_release(vol->socket_cache[i].sock);
		vol->socket_cache[i].sock = NULL;
		memset(vol->socket_cache[i].address, 0,
		       sizeof(vol->socket_cache[i].address));
		vol->socket_cache[i].osd_id = 0;
		vol->socket_cache[i].port = 0;
	}
}

static void kfastblock_volume_close_monitor_cached_sockets(struct kfastblock_volume *vol)
{
	int i;

	if (!vol)
		return;

	for (i = 0; i < KFASTBLOCK_MAX_MONITORS; ++i) {
		if (!vol->monitor_cache[i].sock)
			continue;
		sock_release(vol->monitor_cache[i].sock);
		vol->monitor_cache[i].sock = NULL;
		memset(vol->monitor_cache[i].address, 0,
		       sizeof(vol->monitor_cache[i].address));
		vol->monitor_cache[i].port = 0;
		vol->monitor_cache[i].next_seq = 0;
	}
}

static void kfastblock_volume_close_cached_sockets(struct kfastblock_volume *vol)
{
	kfastblock_volume_close_osd_cached_sockets(vol);
	kfastblock_volume_close_monitor_cached_sockets(vol);
}

static bool kfastblock_volume_should_pause_queue(const struct kfastblock_volume *vol)
{
	if (!vol || !atomic_read(&vol->ready))
		return true;

	return !kfastblock_meta_io_ready(&vol->view);
}

static void kfastblock_volume_update_queue_gate(struct kfastblock_volume *vol)
{
	bool should_pause;

	if (!vol || !vol->disk || !vol->disk->queue)
		return;

	should_pause = kfastblock_volume_should_pause_queue(vol);
	if (should_pause == vol->queue_paused)
		return;

	if (should_pause) {
		blk_mq_quiesce_queue(vol->disk->queue);
		vol->queue_paused = true;
		atomic64_inc(&vol->stats.queue_pause_events);
	} else {
		blk_mq_unquiesce_queue(vol->disk->queue);
		vol->queue_paused = false;
		atomic64_inc(&vol->stats.queue_resume_events);
	}
}

static u32 kfastblock_volume_effective_max_io_bytes(const struct kfastblock_volume *vol)
{
	u64 limit = KFASTBLOCK_MAX_IO_BYTES;

	if (!vol || !vol->view.image.object_size)
		return KFASTBLOCK_MAX_IO_BYTES;

	limit = min_t(u64, limit,
		      (u64)vol->view.image.object_size * KFASTBLOCK_MAX_OBJECT_EXTENTS);
	if (vol->view.image.block_size)
		limit = round_down(limit, (u64)vol->view.image.block_size);
	if (limit == 0)
		limit = vol->view.image.block_size ? vol->view.image.block_size : 512;

	return (u32)limit;
}

static void kfastblock_volume_schedule_refresh(struct kfastblock_volume *vol)
{
	if (!vol || !atomic_read(&vol->ready))
		return;

	schedule_delayed_work(&vol->refresh_work,
			      kfastblock_volume_refresh_interval());
}

void kfastblock_volume_kick_refresh(struct kfastblock_volume *vol)
{
	if (!vol || !atomic_read(&vol->ready))
		return;

	mod_delayed_work(system_wq, &vol->refresh_work, 0);
}

static int kfastblock_volume_copy_attach_spec(struct kfastblock_attach_spec *dst,
					      const struct kfastblock_attach_spec *src)
{
	if (!dst || !src)
		return -EINVAL;

	memset(dst, 0, sizeof(*dst));
	if (src->monitor_addr) {
		dst->monitor_addr = kstrdup(src->monitor_addr, GFP_KERNEL);
		if (!dst->monitor_addr)
			goto nomem;
	}
	if (src->pool_name) {
		dst->pool_name = kstrdup(src->pool_name, GFP_KERNEL);
		if (!dst->pool_name)
			goto nomem;
	}
	if (src->image_name) {
		dst->image_name = kstrdup(src->image_name, GFP_KERNEL);
		if (!dst->image_name)
			goto nomem;
	}

	dst->read_only = src->read_only;
	dst->debug_size_bytes = src->debug_size_bytes;
	dst->debug_object_size = src->debug_object_size;
	dst->debug_pool_id = src->debug_pool_id;
	dst->debug_pg_count = src->debug_pg_count;
	dst->nr_monitors = src->nr_monitors;
	memcpy(dst->monitors, src->monitors, sizeof(dst->monitors));
	return 0;

nomem:
	kfastblock_control_cleanup_attach_spec(dst);
	return -ENOMEM;
}

static void kfastblock_volume_refresh_workfn(struct work_struct *work)
{
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct kfastblock_volume *vol = container_of(delayed_work,
					      struct kfastblock_volume,
					      refresh_work);
	u64 old_size;
	u64 old_osdmap_epoch;
	u64 old_pgmap_epoch;
	u32 old_block_size;
	u32 old_object_size;
	bool old_read_only;
	bool refresh_image;
	int ret;
	int image_ret;

	if (!atomic_read(&vol->ready))
		return;

	down_write(&vol->state_lock);
	if (!atomic_read(&vol->ready)) {
		up_write(&vol->state_lock);
		return;
	}

	old_size = vol->view.image.size_bytes;
	old_osdmap_epoch = vol->view.osdmap_epoch;
	old_pgmap_epoch = vol->view.pgmap_epoch;
	old_block_size = vol->view.image.block_size;
	old_object_size = vol->view.image.object_size;
	old_read_only = vol->view.image.read_only;
	refresh_image = time_after_eq(
		jiffies,
		vol->view.last_image_refresh_jiffies +
		kfastblock_volume_image_refresh_interval());
	ret = kfastblock_transport_refresh_cluster_map_volume(vol);
	if (!ret && vol->disk) {
		image_ret = 0;
		if (refresh_image)
			image_ret = kfastblock_transport_refresh_image_volume(vol);
		if (old_osdmap_epoch != vol->view.osdmap_epoch ||
		    old_pgmap_epoch != vol->view.pgmap_epoch)
			kfastblock_volume_close_osd_cached_sockets(vol);
		if (old_size != vol->view.image.size_bytes)
			set_capacity_and_notify(vol->disk,
						vol->view.image.size_bytes >>
						SECTOR_SHIFT);
		if (old_block_size != vol->view.image.block_size &&
		    vol->view.image.block_size)
			blk_queue_logical_block_size(vol->disk->queue,
					     vol->view.image.block_size);
		if ((old_block_size != vol->view.image.block_size &&
		     vol->view.image.block_size) ||
		    (old_object_size != vol->view.image.object_size &&
		     vol->view.image.object_size)) {
			u32 max_io_bytes = kfastblock_volume_effective_max_io_bytes(vol);

			blk_queue_io_opt(vol->disk->queue,
					 vol->view.image.object_size);
			blk_queue_chunk_sectors(vol->disk->queue,
					vol->view.image.object_size >>
					SECTOR_SHIFT);
			blk_queue_max_hw_sectors(vol->disk->queue,
					 max_io_bytes >> SECTOR_SHIFT);
			blk_queue_max_segment_size(vol->disk->queue, max_io_bytes);
		}
		if (old_read_only != vol->view.image.read_only)
			set_disk_ro(vol->disk, vol->view.image.read_only);
		if (image_ret)
			dev_warn(&vol->dev,
				 "image metadata refresh failed: %d\n",
				 image_ret);
	} else if (ret) {
		kfastblock_volume_close_osd_cached_sockets(vol);
		kfastblock_volume_close_monitor_cached_sockets(vol);
		vol->view.sync_state = KFASTBLOCK_META_SYNC_STALE;
		kfastblock_volume_account_metadata_stale(vol);
	}
	kfastblock_volume_update_queue_gate(vol);
	up_write(&vol->state_lock);

	kfastblock_volume_schedule_refresh(vol);
}

static ssize_t pool_name_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%s\n", vol->view.image.pool_name);
}

static ssize_t image_name_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%s\n", vol->view.image.image_name);
}

static ssize_t size_bytes_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%llu\n", vol->view.image.size_bytes);
}

static ssize_t object_size_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->view.image.object_size);
}

static ssize_t pool_id_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->view.image.pool_id);
}

static ssize_t pg_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->view.image.pg_count);
}

static ssize_t osd_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->view.osd_count);
}

static ssize_t route_count_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->view.route_count);
}

static ssize_t osdmap_epoch_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%llu\n", vol->view.osdmap_epoch);
}

static ssize_t pgmap_epoch_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%llu\n", vol->view.pgmap_epoch);
}

static ssize_t leader_epoch_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%llu\n", vol->view.leader_epoch);
}

static ssize_t last_refresh_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lu\n",
			 vol->view.last_refresh_jiffies);
}

static ssize_t last_image_refresh_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lu\n",
			 vol->view.last_image_refresh_jiffies);
}

static ssize_t read_only_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->view.image.read_only ? 1 : 0);
}

static ssize_t sync_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->view.sync_state);
}

static ssize_t queue_paused_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->queue_paused ? 1 : 0);
}

static ssize_t io_submitted_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.io_submitted));
}

static ssize_t io_completed_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.io_completed));
}

static ssize_t io_failed_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.io_failed));
}

static ssize_t object_io_retries_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.object_io_retries));
}

static ssize_t metadata_stale_events_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.metadata_stale_events));
}

static ssize_t cluster_refresh_fail_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.cluster_refresh_fail));
}

static ssize_t image_refresh_fail_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.image_refresh_fail));
}

static ssize_t leader_query_fail_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.leader_query_fail));
}

static DEVICE_ATTR_RO(pool_name);
static DEVICE_ATTR_RO(image_name);
static DEVICE_ATTR_RO(size_bytes);
static DEVICE_ATTR_RO(object_size);
static DEVICE_ATTR_RO(pool_id);
static DEVICE_ATTR_RO(pg_count);
static DEVICE_ATTR_RO(osd_count);
static DEVICE_ATTR_RO(route_count);
static DEVICE_ATTR_RO(osdmap_epoch);
static DEVICE_ATTR_RO(pgmap_epoch);
static DEVICE_ATTR_RO(leader_epoch);
static DEVICE_ATTR_RO(last_refresh);
static DEVICE_ATTR_RO(last_image_refresh);
static DEVICE_ATTR_RO(read_only);
static DEVICE_ATTR_RO(sync_state);
static DEVICE_ATTR_RO(queue_paused);
static DEVICE_ATTR_RO(io_submitted);
static DEVICE_ATTR_RO(io_completed);
static DEVICE_ATTR_RO(io_failed);
static DEVICE_ATTR_RO(object_io_retries);
static DEVICE_ATTR_RO(metadata_stale_events);
static DEVICE_ATTR_RO(cluster_refresh_fail);
static DEVICE_ATTR_RO(image_refresh_fail);
static DEVICE_ATTR_RO(leader_query_fail);

static struct attribute *kfastblock_volume_attrs[] = {
	&dev_attr_pool_name.attr,
	&dev_attr_image_name.attr,
	&dev_attr_size_bytes.attr,
	&dev_attr_object_size.attr,
	&dev_attr_pool_id.attr,
	&dev_attr_pg_count.attr,
	&dev_attr_osd_count.attr,
	&dev_attr_route_count.attr,
	&dev_attr_osdmap_epoch.attr,
	&dev_attr_pgmap_epoch.attr,
	&dev_attr_leader_epoch.attr,
	&dev_attr_last_refresh.attr,
	&dev_attr_last_image_refresh.attr,
	&dev_attr_read_only.attr,
	&dev_attr_sync_state.attr,
	&dev_attr_queue_paused.attr,
	&dev_attr_io_submitted.attr,
	&dev_attr_io_completed.attr,
	&dev_attr_io_failed.attr,
	&dev_attr_object_io_retries.attr,
	&dev_attr_metadata_stale_events.attr,
	&dev_attr_cluster_refresh_fail.attr,
	&dev_attr_image_refresh_fail.attr,
	&dev_attr_leader_query_fail.attr,
	NULL,
};

static const struct attribute_group kfastblock_volume_attr_group = {
	.attrs = kfastblock_volume_attrs,
};

static const struct attribute_group *kfastblock_volume_attr_groups[] = {
	&kfastblock_volume_attr_group,
	NULL,
};

static int kfastblock_blk_open(struct gendisk *disk, blk_mode_t mode)
{
	struct kfastblock_volume *vol = disk->private_data;

	if (!vol)
		return -ENXIO;

	if (!atomic_read(&vol->ready))
		return -ENOENT;

	atomic_inc(&vol->open_count);
	get_device(&vol->dev);
	return 0;
}

static void kfastblock_blk_release(struct gendisk *disk)
{
	struct kfastblock_volume *vol = disk->private_data;

	put_device(&vol->dev);
	atomic_dec(&vol->open_count);
}

static blk_status_t kfastblock_queue_rq(struct blk_mq_hw_ctx *hctx,
					const struct blk_mq_queue_data *bd)
{
	struct kfastblock_volume *vol = hctx->queue->queuedata;
	struct request *rq = bd->rq;
	struct kfastblock_request *kf_req = blk_mq_rq_to_pdu(rq);
	int ret;

	blk_mq_start_request(rq);

	if (!vol || !atomic_read(&vol->ready) || vol->queue_paused) {
		blk_mq_end_request(rq, BLK_STS_RESOURCE);
		return BLK_STS_RESOURCE;
	}

	if (blk_rq_is_passthrough(rq)) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}
	if (vol->view.image.read_only) {
		switch (req_op(rq)) {
		case REQ_OP_WRITE:
		case REQ_OP_WRITE_ZEROES:
		case REQ_OP_DISCARD:
			blk_mq_end_request(rq, BLK_STS_IOERR);
			return BLK_STS_IOERR;
		default:
			break;
		}
	}

	kf_req->status = 0;
	down_read(&vol->state_lock);
	if (!kfastblock_meta_io_ready(&vol->view)) {
		up_read(&vol->state_lock);
		kfastblock_volume_kick_refresh(vol);
		blk_mq_end_request(rq, BLK_STS_RESOURCE);
		return BLK_STS_RESOURCE;
	}
	kfastblock_request_init(kf_req, vol, rq);
	ret = kfastblock_request_split(kf_req);
	up_read(&vol->state_lock);
	if (ret) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}

	kfastblock_volume_account_io_submit(vol, req_op(rq), blk_rq_bytes(rq));
	ret = kfastblock_transport_submit(kf_req);
	if (ret) {
		kfastblock_volume_account_io_complete(vol, ret);
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}

	return BLK_STS_OK;
}

static const struct block_device_operations kfastblock_bd_ops = {
	.owner = THIS_MODULE,
	.open = kfastblock_blk_open,
	.release = kfastblock_blk_release,
};

static const struct blk_mq_ops kfastblock_mq_ops = {
	.queue_rq = kfastblock_queue_rq,
};

static void kfastblock_volume_free(struct kfastblock_volume *vol)
{
	if (!vol)
		return;

	atomic_set(&vol->ready, 0);
	cancel_delayed_work_sync(&vol->refresh_work);

	mutex_lock(&g_kfastblock_volumes_lock);
	if (!list_empty(&vol->node))
		list_del_init(&vol->node);
	mutex_unlock(&g_kfastblock_volumes_lock);

	kfastblock_volume_debugfs_exit(vol);
	if (vol->disk) {
		vol->queue_paused = true;
		blk_mq_quiesce_queue(vol->disk->queue);
		del_gendisk(vol->disk);
		put_disk(vol->disk);
		vol->disk = NULL;
	}

	blk_mq_free_tag_set(&vol->tag_set);

	if (vol->dev_id >= 0)
		ida_free(&g_kfastblock_dev_ida, vol->dev_id);

	kfastblock_volume_close_cached_sockets(vol);
	kfastblock_control_cleanup_attach_spec(&vol->spec);
	kfastblock_meta_cleanup_view(&vol->view);
	kfree(vol);
	module_put(THIS_MODULE);
}

static void kfastblock_volume_dev_release(struct device *dev)
{
	struct kfastblock_volume *vol;

	vol = container_of(dev, struct kfastblock_volume, dev);
	kfastblock_volume_free(vol);
}

static const struct device_type kfastblock_volume_type = {
	.name = "kfastblock_volume",
	.groups = kfastblock_volume_attr_groups,
	.release = kfastblock_volume_dev_release,
};

static int kfastblock_volume_add_disk(struct kfastblock_volume *vol,
				      struct bus_type *bus,
				      struct device *parent_dev)
{
	int ret;

	memset(&vol->tag_set, 0, sizeof(vol->tag_set));
	vol->tag_set.ops = &kfastblock_mq_ops;
	vol->tag_set.queue_depth = KFASTBLOCK_DEFAULT_QUEUE_DEPTH;
	vol->tag_set.numa_node = NUMA_NO_NODE;
	vol->tag_set.nr_hw_queues = 1;
	vol->tag_set.cmd_size = sizeof(struct kfastblock_request);
	vol->tag_set.driver_data = vol;
	vol->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;

	ret = blk_mq_alloc_tag_set(&vol->tag_set);
	if (ret)
		return ret;

	vol->dev_id = ida_alloc_range(&g_kfastblock_dev_ida, 0,
				      (1 << (MINORBITS -
				       KFASTBLOCK_DEVICE_PART_SHIFT)) - 1,
				      GFP_KERNEL);
	if (vol->dev_id < 0)
		return vol->dev_id;

	vol->minor = vol->dev_id << KFASTBLOCK_DEVICE_PART_SHIFT;
	vol->disk = blk_mq_alloc_disk(&vol->tag_set, vol);
	if (!vol->disk)
		return -ENOMEM;

	vol->disk->private_data = vol;
	vol->disk->major = vol->major;
	vol->disk->first_minor = vol->minor;
	vol->disk->fops = &kfastblock_bd_ops;
	vol->disk->minors = 1 << KFASTBLOCK_DEVICE_PART_SHIFT;
	snprintf(vol->disk->disk_name, sizeof(vol->disk->disk_name), "%s%d",
		 KFASTBLOCK_DRV_NAME_PREFIX, vol->dev_id);
	set_capacity(vol->disk, vol->view.image.size_bytes >> SECTOR_SHIFT);
	if (vol->view.image.read_only)
		set_disk_ro(vol->disk, true);
	if (vol->view.image.block_size)
		blk_queue_logical_block_size(vol->disk->queue,
					     vol->view.image.block_size);
	if (vol->view.image.object_size) {
		u32 max_io_bytes = kfastblock_volume_effective_max_io_bytes(vol);

		blk_queue_io_opt(vol->disk->queue, vol->view.image.object_size);
		blk_queue_chunk_sectors(vol->disk->queue,
					vol->view.image.object_size >>
					SECTOR_SHIFT);
		blk_queue_max_hw_sectors(vol->disk->queue,
				 max_io_bytes >> SECTOR_SHIFT);
		blk_queue_max_segment_size(vol->disk->queue, max_io_bytes);
	} else {
		blk_queue_max_hw_sectors(vol->disk->queue,
				 KFASTBLOCK_MAX_IO_BYTES >> SECTOR_SHIFT);
		blk_queue_max_segment_size(vol->disk->queue, KFASTBLOCK_MAX_IO_BYTES);
	}
	blk_queue_max_segments(vol->disk->queue, USHRT_MAX);
	vol->disk->queue->queuedata = vol;
	kfastblock_volume_update_queue_gate(vol);

	device_initialize(&vol->dev);
	vol->dev.parent = parent_dev;
	vol->dev.bus = bus;
	vol->dev.type = &kfastblock_volume_type;
	dev_set_name(&vol->dev, "%s%s-%s",
		     KFASTBLOCK_SYSFS_DEV_PREFIX,
		     vol->view.image.pool_name,
		     vol->view.image.image_name);
	dev_set_drvdata(&vol->dev, vol);
	kfastblock_volume_debugfs_init(vol);

	ret = device_add(&vol->dev);
	if (ret) {
		kfastblock_volume_debugfs_exit(vol);
		return ret;
	}

	ret = device_add_disk(&vol->dev, vol->disk, NULL);
	if (ret) {
		kfastblock_volume_debugfs_exit(vol);
		device_del(&vol->dev);
		return ret;
	}

	return 0;
}

int kfastblock_volume_attach(const struct kfastblock_attach_spec *spec, int major,
			     struct bus_type *bus, struct device *parent_dev)
{
	struct kfastblock_volume *vol;
	int i;
	int ret;

	if (!spec || !bus || !parent_dev)
		return -EINVAL;

	vol = kzalloc(sizeof(*vol), GFP_KERNEL);
	if (!vol)
		return -ENOMEM;

	vol->dev_id = -1;
	vol->major = major;
	atomic_set(&vol->open_count, 0);
	atomic_set(&vol->ready, 0);
	vol->queue_paused = false;
	kfastblock_volume_stats_init(vol);
	mutex_init(&vol->inflight_lock);
	init_rwsem(&vol->state_lock);
	for (i = 0; i < KFASTBLOCK_MAX_SOCKET_CACHE; ++i)
		mutex_init(&vol->socket_cache[i].lock);
	for (i = 0; i < KFASTBLOCK_MAX_MONITORS; ++i)
		mutex_init(&vol->monitor_cache[i].lock);
	INIT_DELAYED_WORK(&vol->refresh_work, kfastblock_volume_refresh_workfn);
	INIT_LIST_HEAD(&vol->node);

	ret = kfastblock_volume_copy_attach_spec(&vol->spec, spec);
	if (ret)
		goto err_free;

	ret = kfastblock_meta_bootstrap(&vol->view, spec);
	if (ret)
		goto err_free;

	if (!kfastblock_meta_ready(&vol->view)) {
		ret = -EOPNOTSUPP;
		goto err_free;
	}

	ret = kfastblock_volume_add_disk(vol, bus, parent_dev);
	if (ret)
		goto err_free;

	mutex_lock(&g_kfastblock_volumes_lock);
	list_add_tail(&vol->node, &g_kfastblock_volumes);
	mutex_unlock(&g_kfastblock_volumes_lock);

	atomic_set(&vol->ready, 1);
	kfastblock_volume_update_queue_gate(vol);
	kfastblock_volume_schedule_refresh(vol);
	return 0;

err_free:
	cancel_delayed_work_sync(&vol->refresh_work);
	kfastblock_control_cleanup_attach_spec(&vol->spec);
	kfastblock_meta_cleanup_view(&vol->view);
	kfree(vol);
	return ret;
}

int kfastblock_volume_detach(const struct kfastblock_attach_spec *spec)
{
	struct kfastblock_volume *vol;
	struct kfastblock_volume *found = NULL;

	if (!spec)
		return -EINVAL;

	mutex_lock(&g_kfastblock_volumes_lock);
	list_for_each_entry(vol, &g_kfastblock_volumes, node) {
		if (strcmp(vol->view.image.pool_name, spec->pool_name))
			continue;
		if (strcmp(vol->view.image.image_name, spec->image_name))
			continue;
		found = vol;
		break;
	}
	mutex_unlock(&g_kfastblock_volumes_lock);

	if (!found)
		return -ENOENT;

	atomic_set(&found->ready, 0);
	cancel_delayed_work_sync(&found->refresh_work);
	if (found->disk) {
		found->queue_paused = true;
		blk_mq_quiesce_queue(found->disk->queue);
		del_gendisk(found->disk);
		put_disk(found->disk);
		found->disk = NULL;
	}
	device_unregister(&found->dev);
	return 0;
}

int kfastblock_volume_init(void)
{
	ida_init(&g_kfastblock_dev_ida);
	g_kfastblock_debugfs_root = debugfs_create_dir(KFASTBLOCK_DRV_NAME, NULL);
	return 0;
}

void kfastblock_volume_exit(void)
{
	struct kfastblock_volume *vol;
	struct kfastblock_volume *tmp;

	mutex_lock(&g_kfastblock_volumes_lock);
	list_for_each_entry_safe(vol, tmp, &g_kfastblock_volumes, node) {
		list_del_init(&vol->node);
		atomic_set(&vol->ready, 0);
		cancel_delayed_work_sync(&vol->refresh_work);
		if (vol->disk) {
			del_gendisk(vol->disk);
			put_disk(vol->disk);
			vol->disk = NULL;
		}
		device_unregister(&vol->dev);
	}
	mutex_unlock(&g_kfastblock_volumes_lock);

	debugfs_remove_recursive(g_kfastblock_debugfs_root);
	g_kfastblock_debugfs_root = NULL;
	ida_destroy(&g_kfastblock_dev_ida);
}
