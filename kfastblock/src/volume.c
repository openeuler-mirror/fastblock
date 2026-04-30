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

static struct kfastblock_volume *
kfastblock_volume_find_locked(const char *pool_name, const char *image_name)
{
	struct kfastblock_volume *vol;

	if (!pool_name || !image_name)
		return NULL;

	list_for_each_entry(vol, &g_kfastblock_volumes, node) {
		if (strcmp(vol->view.image.pool_name, pool_name))
			continue;
		if (strcmp(vol->view.image.image_name, image_name))
			continue;
		return vol;
	}

	return NULL;
}

static void kfastblock_volume_record_event(
	struct kfastblock_volume *vol,
	enum kfastblock_volume_event_type type,
	int ret,
	enum req_op op,
	u32 pg_id,
	u32 osd_id,
	u32 arg0,
	u32 arg1);

static const char *
kfastblock_volume_event_type_name(const enum kfastblock_volume_event_type type)
{
	switch (type) {
	case KFASTBLOCK_VOLUME_EVENT_ATTACH_READY:
		return "attach_ready";
	case KFASTBLOCK_VOLUME_EVENT_QUEUE_PAUSE:
		return "queue_pause";
	case KFASTBLOCK_VOLUME_EVENT_QUEUE_RESUME:
		return "queue_resume";
	case KFASTBLOCK_VOLUME_EVENT_METADATA_STALE:
		return "metadata_stale";
	case KFASTBLOCK_VOLUME_EVENT_CLUSTER_REFRESH_FAIL:
		return "cluster_refresh_fail";
	case KFASTBLOCK_VOLUME_EVENT_IMAGE_REFRESH_FAIL:
		return "image_refresh_fail";
	case KFASTBLOCK_VOLUME_EVENT_LEADER_QUERY_FAIL:
		return "leader_query_fail";
	case KFASTBLOCK_VOLUME_EVENT_OBJECT_RETRY:
		return "object_retry";
	case KFASTBLOCK_VOLUME_EVENT_OBJECT_ERROR:
		return "object_error";
	case KFASTBLOCK_VOLUME_EVENT_REFRESH_KICK:
		return "refresh_kick";
	case KFASTBLOCK_VOLUME_EVENT_LEADER_INVALIDATE:
		return "leader_invalidate";
	case KFASTBLOCK_VOLUME_EVENT_OSD_SOCKET_DROP:
		return "osd_socket_drop";
	case KFASTBLOCK_VOLUME_EVENT_MONITOR_SOCKET_DROP:
		return "monitor_socket_drop";
	case KFASTBLOCK_VOLUME_EVENT_HEALTH_CHANGE:
		return "health_change";
	case KFASTBLOCK_VOLUME_EVENT_SOCKET_BACKOFF:
		return "socket_backoff";
	case KFASTBLOCK_VOLUME_EVENT_MANUAL_REFRESH:
		return "manual_refresh";
	case KFASTBLOCK_VOLUME_EVENT_MANUAL_RESET_BACKOFF:
		return "manual_reset_backoff";
	case KFASTBLOCK_VOLUME_EVENT_MANUAL_DROP_TRANSPORT:
		return "manual_drop_transport";
	case KFASTBLOCK_VOLUME_EVENT_MANUAL_RESET_LEADERS:
		return "manual_reset_leaders";
	case KFASTBLOCK_VOLUME_EVENT_SOCKET_BACKOFF_WAIT:
		return "socket_backoff_wait";
	default:
		return "unknown";
	}
}

static const char *kfastblock_volume_req_op_name(const u8 op)
{
	switch ((enum req_op)op) {
	case REQ_OP_READ:
		return "read";
	case REQ_OP_WRITE:
		return "write";
	case REQ_OP_FLUSH:
		return "flush";
	case REQ_OP_DISCARD:
		return "discard";
	case REQ_OP_WRITE_ZEROES:
		return "write_zeroes";
	default:
		return "other";
	}
}

static const char *kfastblock_volume_health_state_name(const u32 state)
{
	switch (state) {
	case KFASTBLOCK_VOLUME_HEALTH_READY:
		return "ready";
	case KFASTBLOCK_VOLUME_HEALTH_DEGRADED:
		return "degraded";
	case KFASTBLOCK_VOLUME_HEALTH_STALE:
		return "stale";
	default:
		return "unknown";
	}
}

static const char *kfastblock_volume_failure_source_name(const u32 source)
{
	switch (source) {
	case KFASTBLOCK_VOLUME_SOURCE_ATTACH:
		return "attach";
	case KFASTBLOCK_VOLUME_SOURCE_QUEUE_GATE:
		return "queue_gate";
	case KFASTBLOCK_VOLUME_SOURCE_CLUSTER_REFRESH:
		return "cluster_refresh";
	case KFASTBLOCK_VOLUME_SOURCE_IMAGE_REFRESH:
		return "image_refresh";
	case KFASTBLOCK_VOLUME_SOURCE_LEADER_QUERY:
		return "leader_query";
	case KFASTBLOCK_VOLUME_SOURCE_OBJECT_IO:
		return "object_io";
	case KFASTBLOCK_VOLUME_SOURCE_MONITOR_SOCKET:
		return "monitor_socket";
	case KFASTBLOCK_VOLUME_SOURCE_OSD_SOCKET:
		return "osd_socket";
	default:
		return "none";
	}
}

void kfastblock_volume_update_health(struct kfastblock_volume *vol,
				     u32 new_state, u32 source, int ret)
{
	u32 old_state;

	if (!vol)
		return;

	old_state = vol->health.state;
	if (ret) {
		vol->health.last_errno = ret;
		vol->health.last_failure_source = source;
		vol->health.last_failure_jiffies = jiffies;
	}
	if (new_state == old_state)
		return;

	vol->health.state = new_state;
	vol->health.state_since_jiffies = jiffies;
	kfastblock_volume_record_event(vol, KFASTBLOCK_VOLUME_EVENT_HEALTH_CHANGE,
				      ret, REQ_OP_FLUSH, 0, 0, old_state, new_state);
}

void kfastblock_volume_mark_success(struct kfastblock_volume *vol, u32 source)
{
	if (!vol)
		return;

	vol->health.last_success_jiffies = jiffies;
	if (!atomic_read(&vol->ready))
		return;
	if (!kfastblock_meta_io_ready(&vol->view) || vol->queue_paused) {
		kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_STALE,
				      source ? source : KFASTBLOCK_VOLUME_SOURCE_QUEUE_GATE,
				      -ESTALE);
		return;
	}
	kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_READY,
				      source, 0);
}

static u32 kfastblock_volume_events_count(struct kfastblock_volume *vol)
{
	unsigned long flags;
	u32 count = 0;

	if (!vol)
		return 0;

	spin_lock_irqsave(&vol->event_log.lock, flags);
	count = vol->event_log.count;
	spin_unlock_irqrestore(&vol->event_log.lock, flags);
	return count;
}

static void kfastblock_volume_record_event(
	struct kfastblock_volume *vol,
	enum kfastblock_volume_event_type type,
	int ret,
	enum req_op op,
	u32 pg_id,
	u32 osd_id,
	u32 arg0,
	u32 arg1)
{
	struct kfastblock_volume_event *event;
	unsigned long flags;

	if (!vol)
		return;

	spin_lock_irqsave(&vol->event_log.lock, flags);
	event = &vol->event_log.entries[vol->event_log.next_index];
	event->jiffies = get_jiffies_64();
	event->ret = ret;
	event->type = type;
	event->pg_id = pg_id;
	event->osd_id = osd_id;
	event->arg0 = arg0;
	event->arg1 = arg1;
	event->op = (u8)op;
	vol->event_log.next_index =
		(vol->event_log.next_index + 1) % KFASTBLOCK_MAX_VOLUME_EVENTS;
	if (vol->event_log.count < KFASTBLOCK_MAX_VOLUME_EVENTS)
		++vol->event_log.count;
	spin_unlock_irqrestore(&vol->event_log.lock, flags);
}

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
	atomic64_set(&vol->stats.refresh_kicks, 0);
	atomic64_set(&vol->stats.leader_invalidations, 0);
	atomic64_set(&vol->stats.osd_socket_drops, 0);
	atomic64_set(&vol->stats.monitor_socket_drops, 0);
	atomic64_set(&vol->stats.osd_backoff_hits, 0);
	atomic64_set(&vol->stats.monitor_backoff_hits, 0);
	atomic64_set(&vol->stats.osd_backoff_waits, 0);
	atomic64_set(&vol->stats.monitor_backoff_waits, 0);
	atomic64_set(&vol->stats.manual_refreshes, 0);
	atomic64_set(&vol->stats.manual_reset_backoffs, 0);
	atomic64_set(&vol->stats.manual_transport_drops, 0);
	atomic64_set(&vol->stats.manual_leader_resets, 0);
	spin_lock_init(&vol->event_log.lock);
	vol->event_log.next_index = 0;
	vol->event_log.count = 0;
	memset(vol->event_log.entries, 0, sizeof(vol->event_log.entries));
	vol->health.state = KFASTBLOCK_VOLUME_HEALTH_UNKNOWN;
	vol->health.last_failure_source = KFASTBLOCK_VOLUME_SOURCE_NONE;
	vol->health.last_errno = 0;
	vol->health.state_since_jiffies = jiffies;
	vol->health.last_failure_jiffies = 0;
	vol->health.last_success_jiffies = 0;
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

void kfastblock_volume_account_object_retry(struct kfastblock_volume *vol,
				      enum req_op op, u32 pg_id,
				      u32 osd_id, u32 length, int ret)
{
	if (!vol)
		return;

	atomic64_inc(&vol->stats.object_io_retries);
	kfastblock_volume_record_event(vol, KFASTBLOCK_VOLUME_EVENT_OBJECT_RETRY,
				      ret, op, pg_id, osd_id, length, 0);
	kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      KFASTBLOCK_VOLUME_SOURCE_OBJECT_IO, ret);
}

void kfastblock_volume_account_object_error(struct kfastblock_volume *vol,
				     enum req_op op, u32 pg_id,
				     u32 osd_id, u32 length, int ret)
{
	if (!vol)
		return;

	atomic64_inc(&vol->stats.object_io_errors);
	kfastblock_volume_record_event(vol, KFASTBLOCK_VOLUME_EVENT_OBJECT_ERROR,
				      ret, op, pg_id, osd_id, length, 0);
	kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      KFASTBLOCK_VOLUME_SOURCE_OBJECT_IO, ret);
}

void kfastblock_volume_account_leader_query(struct kfastblock_volume *vol,
				     u32 pg_id, u32 osd_id, int ret)
{
	if (!vol)
		return;

	if (ret) {
		atomic64_inc(&vol->stats.leader_query_fail);
		kfastblock_volume_record_event(
			vol, KFASTBLOCK_VOLUME_EVENT_LEADER_QUERY_FAIL, ret,
			REQ_OP_FLUSH, pg_id, osd_id, (u32)vol->view.leader_epoch, 0);
		kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      KFASTBLOCK_VOLUME_SOURCE_LEADER_QUERY, ret);
	} else {
		atomic64_inc(&vol->stats.leader_query_ok);
		kfastblock_volume_mark_success(vol, KFASTBLOCK_VOLUME_SOURCE_LEADER_QUERY);
	}
}

void kfastblock_volume_account_cluster_refresh(struct kfastblock_volume *vol,
				       int ret)
{
	if (!vol)
		return;

	if (ret) {
		atomic64_inc(&vol->stats.cluster_refresh_fail);
		kfastblock_volume_record_event(
			vol, KFASTBLOCK_VOLUME_EVENT_CLUSTER_REFRESH_FAIL, ret,
			REQ_OP_FLUSH, 0, 0, (u32)vol->view.osdmap_epoch,
			(u32)vol->view.pgmap_epoch);
		kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      KFASTBLOCK_VOLUME_SOURCE_CLUSTER_REFRESH, ret);
	} else {
		atomic64_inc(&vol->stats.cluster_refresh_ok);
		kfastblock_volume_mark_success(vol, KFASTBLOCK_VOLUME_SOURCE_CLUSTER_REFRESH);
	}
}

void kfastblock_volume_account_image_refresh(struct kfastblock_volume *vol,
				     int ret)
{
	if (!vol)
		return;

	if (ret) {
		atomic64_inc(&vol->stats.image_refresh_fail);
		kfastblock_volume_record_event(
			vol, KFASTBLOCK_VOLUME_EVENT_IMAGE_REFRESH_FAIL, ret,
			REQ_OP_FLUSH, 0, 0, (u32)vol->view.image_epoch,
			vol->view.image.object_size);
		kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      KFASTBLOCK_VOLUME_SOURCE_IMAGE_REFRESH, ret);
	} else {
		atomic64_inc(&vol->stats.image_refresh_ok);
		kfastblock_volume_mark_success(vol, KFASTBLOCK_VOLUME_SOURCE_IMAGE_REFRESH);
	}
}

void kfastblock_volume_account_metadata_stale(struct kfastblock_volume *vol)
{
	if (!vol)
		return;

	atomic64_inc(&vol->stats.metadata_stale_events);
	kfastblock_volume_record_event(vol, KFASTBLOCK_VOLUME_EVENT_METADATA_STALE,
				      -ESTALE, REQ_OP_FLUSH, 0, 0,
				      vol->view.sync_state,
				      (u32)vol->view.pgmap_epoch);
	kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_STALE,
				      KFASTBLOCK_VOLUME_SOURCE_CLUSTER_REFRESH, -ESTALE);
}

void kfastblock_volume_account_refresh_kick(struct kfastblock_volume *vol,
				      u32 pg_id, int ret)
{
	if (!vol)
		return;

	atomic64_inc(&vol->stats.refresh_kicks);
	kfastblock_volume_record_event(vol, KFASTBLOCK_VOLUME_EVENT_REFRESH_KICK,
				      ret, REQ_OP_FLUSH, pg_id, 0,
				      vol->view.sync_state, (u32)vol->view.pgmap_epoch);
	kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      KFASTBLOCK_VOLUME_SOURCE_QUEUE_GATE, ret);
}

void kfastblock_volume_account_leader_invalidate(struct kfastblock_volume *vol,
				       u32 pg_id, int ret)
{
	if (!vol)
		return;

	atomic64_inc(&vol->stats.leader_invalidations);
	kfastblock_volume_record_event(
		vol, KFASTBLOCK_VOLUME_EVENT_LEADER_INVALIDATE, ret, REQ_OP_FLUSH,
		pg_id, 0, (u32)vol->view.leader_epoch, (u32)vol->view.osdmap_epoch);
	kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      KFASTBLOCK_VOLUME_SOURCE_LEADER_QUERY, ret);
}

void kfastblock_volume_account_socket_drop(struct kfastblock_volume *vol,
				    bool monitor_socket, u32 osd_id,
				    u16 port, int ret)
{
	if (!vol)
		return;

	if (monitor_socket) {
		atomic64_inc(&vol->stats.monitor_socket_drops);
		kfastblock_volume_record_event(
			vol, KFASTBLOCK_VOLUME_EVENT_MONITOR_SOCKET_DROP, ret,
			REQ_OP_FLUSH, 0, 0, port, 0);
		kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      KFASTBLOCK_VOLUME_SOURCE_MONITOR_SOCKET, ret);
	} else {
		atomic64_inc(&vol->stats.osd_socket_drops);
		kfastblock_volume_record_event(
			vol, KFASTBLOCK_VOLUME_EVENT_OSD_SOCKET_DROP, ret,
			REQ_OP_FLUSH, 0, osd_id, port, 0);
		kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      KFASTBLOCK_VOLUME_SOURCE_OSD_SOCKET, ret);
	}
}

void kfastblock_volume_account_socket_backoff(struct kfastblock_volume *vol,
				       bool monitor_socket, u32 osd_id,
				       u16 port, u32 fail_streak,
				       unsigned long backoff_jiffies, int ret)
{
	if (!vol)
		return;

	if (monitor_socket)
		atomic64_inc(&vol->stats.monitor_backoff_hits);
	else
		atomic64_inc(&vol->stats.osd_backoff_hits);
	kfastblock_volume_record_event(vol, KFASTBLOCK_VOLUME_EVENT_SOCKET_BACKOFF,
				      ret, REQ_OP_FLUSH, fail_streak, osd_id,
				      port, jiffies_to_msecs(backoff_jiffies));
	kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      monitor_socket ? KFASTBLOCK_VOLUME_SOURCE_MONITOR_SOCKET :
				      KFASTBLOCK_VOLUME_SOURCE_OSD_SOCKET, ret);
}

void kfastblock_volume_account_socket_backoff_wait(struct kfastblock_volume *vol,
					 bool monitor_socket, u32 osd_id,
					 u16 port,
					 unsigned long remaining_jiffies,
					 int ret)
{
	if (!vol)
		return;

	if (monitor_socket)
		atomic64_inc(&vol->stats.monitor_backoff_waits);
	else
		atomic64_inc(&vol->stats.osd_backoff_waits);
	kfastblock_volume_record_event(vol,
				      KFASTBLOCK_VOLUME_EVENT_SOCKET_BACKOFF_WAIT,
				      ret, REQ_OP_FLUSH, 0, osd_id,
				      port, jiffies_to_msecs(remaining_jiffies));
	kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_DEGRADED,
				      monitor_socket ? KFASTBLOCK_VOLUME_SOURCE_MONITOR_SOCKET :
				      KFASTBLOCK_VOLUME_SOURCE_OSD_SOCKET, ret);
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
	seq_printf(m, "health_state=%s\n",
		   kfastblock_volume_health_state_name(vol->health.state));
	seq_printf(m, "health_since_jiffies=%lu\n", vol->health.state_since_jiffies);
	seq_printf(m, "last_failure_errno=%d\n", vol->health.last_errno);
	seq_printf(m, "last_failure_source=%s\n",
		   kfastblock_volume_failure_source_name(vol->health.last_failure_source));
	seq_printf(m, "last_failure_jiffies=%lu\n", vol->health.last_failure_jiffies);
	seq_printf(m, "last_success_jiffies=%lu\n", vol->health.last_success_jiffies);
	seq_printf(m, "open_count=%d\n", atomic_read(&vol->open_count));
	seq_printf(m, "inflight_ios=%d\n", atomic_read(&vol->inflight_ios));
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
	seq_printf(m, "event_count=%u\n",
		   kfastblock_volume_events_count(vol));
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
			   "osd_cache[%d] active=%u osd_id=%u address=%s port=%u next_seq=%llu fail_streak=%u last_error=%d last_failure_jiffies=%lu backoff_until_jiffies=%lu\n",
			   i, cached->sock ? 1 : 0, cached->osd_id, cached->address,
			   cached->port, cached->next_seq, cached->fail_streak,
			   cached->last_error, cached->last_failure_jiffies,
			   cached->backoff_until_jiffies);
	}
	for (i = 0; i < KFASTBLOCK_MAX_MONITORS; ++i) {
		const struct kfastblock_cached_monitor_socket *cached =
			&vol->monitor_cache[i];

		seq_printf(m,
			   "monitor_cache[%d] active=%u address=%s port=%u next_seq=%llu fail_streak=%u last_error=%d last_failure_jiffies=%lu backoff_until_jiffies=%lu\n",
			   i, cached->sock ? 1 : 0, cached->address, cached->port,
			   cached->next_seq, cached->fail_streak, cached->last_error,
			   cached->last_failure_jiffies, cached->backoff_until_jiffies);
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
	seq_printf(m, "refresh_kicks=%lld\n",
		   atomic64_read(&vol->stats.refresh_kicks));
	seq_printf(m, "leader_invalidations=%lld\n",
		   atomic64_read(&vol->stats.leader_invalidations));
	seq_printf(m, "osd_socket_drops=%lld\n",
		   atomic64_read(&vol->stats.osd_socket_drops));
	seq_printf(m, "monitor_socket_drops=%lld\n",
		   atomic64_read(&vol->stats.monitor_socket_drops));
	seq_printf(m, "osd_backoff_hits=%lld\n",
		   atomic64_read(&vol->stats.osd_backoff_hits));
	seq_printf(m, "monitor_backoff_hits=%lld\n",
		   atomic64_read(&vol->stats.monitor_backoff_hits));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kfastblock_volume_stats);

static int kfastblock_volume_events_show(struct seq_file *m, void *v)
{
	struct kfastblock_volume *vol = m->private;
	struct kfastblock_volume_event *snapshot = NULL;
	unsigned long flags;
	u32 count;
	u32 start;
	u32 i;

	if (!vol)
		return -ENODEV;

	count = kfastblock_volume_events_count(vol);
	if (!count)
		return 0;

	snapshot = kcalloc(count, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	spin_lock_irqsave(&vol->event_log.lock, flags);
	if (count > vol->event_log.count)
		count = vol->event_log.count;
	start = (vol->event_log.next_index + KFASTBLOCK_MAX_VOLUME_EVENTS - count) %
		KFASTBLOCK_MAX_VOLUME_EVENTS;
	for (i = 0; i < count; ++i)
		snapshot[i] = vol->event_log.entries[(start + i) %
					KFASTBLOCK_MAX_VOLUME_EVENTS];
	spin_unlock_irqrestore(&vol->event_log.lock, flags);

	for (i = 0; i < count; ++i) {
		const struct kfastblock_volume_event *event = &snapshot[i];

		seq_printf(m,
			   "idx=%u jiffies=%llu type=%s ret=%d op=%s pg_id=%u osd_id=%u arg0=%u arg1=%u\n",
			   i, event->jiffies,
			   kfastblock_volume_event_type_name(event->type),
			   event->ret,
			   kfastblock_volume_req_op_name(event->op),
			   event->pg_id, event->osd_id, event->arg0, event->arg1);
	}

	kfree(snapshot);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(kfastblock_volume_events);

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
	debugfs_create_file("events", 0444, vol->debugfs_dir, vol,
			    &kfastblock_volume_events_fops);
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
		struct kfastblock_cached_socket *cached = &vol->socket_cache[i];

		mutex_lock(&cached->lock);
		if (cached->sock) {
			sock_release(cached->sock);
			cached->sock = NULL;
		}
		memset(cached->address, 0, sizeof(cached->address));
		cached->osd_id = 0;
		cached->port = 0;
		cached->connecting = false;
		cached->next_seq = 0;
		cached->fail_streak = 0;
		cached->last_error = 0;
		cached->last_failure_jiffies = 0;
		cached->backoff_until_jiffies = 0;
		mutex_unlock(&cached->lock);
	}
}

static void kfastblock_volume_close_monitor_cached_sockets(struct kfastblock_volume *vol)
{
	int i;

	if (!vol)
		return;

	for (i = 0; i < KFASTBLOCK_MAX_MONITORS; ++i) {
		struct kfastblock_cached_monitor_socket *cached =
			&vol->monitor_cache[i];

		mutex_lock(&cached->lock);
		if (cached->sock) {
			sock_release(cached->sock);
			cached->sock = NULL;
		}
		memset(cached->address, 0, sizeof(cached->address));
		cached->port = 0;
		cached->next_seq = 0;
		cached->fail_streak = 0;
		cached->last_error = 0;
		cached->last_failure_jiffies = 0;
		cached->backoff_until_jiffies = 0;
		mutex_unlock(&cached->lock);
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
		kfastblock_volume_record_event(
			vol, KFASTBLOCK_VOLUME_EVENT_QUEUE_PAUSE, 0, REQ_OP_FLUSH,
			0, 0, vol->view.sync_state, atomic_read(&vol->ready));
		kfastblock_volume_update_health(vol, KFASTBLOCK_VOLUME_HEALTH_STALE,
				      KFASTBLOCK_VOLUME_SOURCE_QUEUE_GATE, -ESTALE);
	} else {
		blk_mq_unquiesce_queue(vol->disk->queue);
		vol->queue_paused = false;
		atomic64_inc(&vol->stats.queue_resume_events);
		kfastblock_volume_record_event(
			vol, KFASTBLOCK_VOLUME_EVENT_QUEUE_RESUME, 0, REQ_OP_FLUSH,
			0, 0, vol->view.sync_state, atomic_read(&vol->ready));
		kfastblock_volume_mark_success(vol, KFASTBLOCK_VOLUME_SOURCE_QUEUE_GATE);
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

static void kfastblock_volume_apply_queue_limits(struct kfastblock_volume *vol)
{
	struct request_queue *q;
	u32 max_io_bytes;
	u32 block_size;
	u32 object_size;
	u32 discard_sectors;

	if (!vol || !vol->disk || !vol->disk->queue)
		return;

	q = vol->disk->queue;
	block_size = vol->view.image.block_size ?
		vol->view.image.block_size : KFASTBLOCK_DEFAULT_BLOCK_SIZE;
	object_size = vol->view.image.object_size;
	max_io_bytes = kfastblock_volume_effective_max_io_bytes(vol);
	blk_queue_logical_block_size(q, block_size);
	blk_queue_max_hw_sectors(q, max_io_bytes >> SECTOR_SHIFT);
	blk_queue_max_segment_size(q, max_io_bytes);
	blk_queue_max_segments(q, USHRT_MAX);
	blk_queue_write_cache(q, true, false);

	if (object_size) {
		discard_sectors = max_io_bytes >> SECTOR_SHIFT;
		blk_queue_io_opt(q, object_size);
		blk_queue_chunk_sectors(q, object_size >> SECTOR_SHIFT);
		blk_queue_max_discard_sectors(q, discard_sectors);
		blk_queue_max_write_zeroes_sectors(q, discard_sectors);
		blk_queue_max_discard_segments(q, 1);
		q->limits.discard_granularity = block_size;
		q->limits.discard_alignment = 0;
	} else {
		blk_queue_max_discard_sectors(q, 0);
		blk_queue_max_write_zeroes_sectors(q, 0);
		blk_queue_max_discard_segments(q, 0);
		q->limits.discard_granularity = 0;
		q->limits.discard_alignment = 0;
	}
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

void kfastblock_volume_get_io(struct kfastblock_volume *vol)
{
	if (!vol)
		return;

	atomic_inc(&vol->inflight_ios);
}

void kfastblock_volume_put_io(struct kfastblock_volume *vol)
{
	if (!vol)
		return;

	if (atomic_dec_and_test(&vol->inflight_ios))
		wake_up_all(&vol->inflight_wq);
}

static int kfastblock_volume_begin_flush(struct kfastblock_volume *vol)
{
	int ret = 0;

	if (!vol)
		return -EINVAL;

	mutex_lock(&vol->inflight_lock);
	if (vol->flush_in_progress)
		ret = -EBUSY;
	else
		vol->flush_in_progress = true;
	mutex_unlock(&vol->inflight_lock);
	return ret;
}

static void kfastblock_volume_end_flush(struct kfastblock_volume *vol)
{
	if (!vol)
		return;

	mutex_lock(&vol->inflight_lock);
	vol->flush_in_progress = false;
	mutex_unlock(&vol->inflight_lock);
}

static blk_status_t kfastblock_volume_handle_flush(struct kfastblock_volume *vol,
					     struct request *rq)
{
	int ret;
	blk_status_t status;

	if (!vol || !rq) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}

	ret = kfastblock_volume_begin_flush(vol);
	if (ret) {
		blk_mq_end_request(rq, BLK_STS_RESOURCE);
		return BLK_STS_RESOURCE;
	}

	kfastblock_volume_account_io_submit(vol, REQ_OP_FLUSH, 0);
	ret = kfastblock_volume_drain_io(vol, msecs_to_jiffies(5000));
	kfastblock_volume_end_flush(vol);
	kfastblock_volume_account_io_complete(vol, ret);
	if (!ret)
		kfastblock_volume_mark_success(vol, KFASTBLOCK_VOLUME_SOURCE_OBJECT_IO);
	status = ret ? BLK_STS_RESOURCE : BLK_STS_OK;
	blk_mq_end_request(rq, status);
	return status;
}

int kfastblock_volume_drain_io(struct kfastblock_volume *vol,
			     unsigned long timeout_jiffies)
{
	long remaining;

	if (!vol)
		return -EINVAL;
	if (!atomic_read(&vol->inflight_ios))
		return 0;

	remaining = wait_event_timeout(
		vol->inflight_wq,
		atomic_read(&vol->inflight_ios) == 0,
		timeout_jiffies);
	if (remaining || !atomic_read(&vol->inflight_ios))
		return 0;

	return -ETIMEDOUT;
}

static int kfastblock_volume_begin_stop(struct kfastblock_volume *vol,
				 bool rollback_on_timeout)
{
	int ret;

	if (!vol)
		return -EINVAL;

	atomic_set(&vol->ready, 0);
	cancel_delayed_work_sync(&vol->refresh_work);
	if (vol->disk) {
		vol->queue_paused = true;
		blk_mq_quiesce_queue(vol->disk->queue);
	}

	ret = kfastblock_volume_drain_io(vol, msecs_to_jiffies(5000));
	if (!ret)
		return 0;

	if (rollback_on_timeout) {
		atomic_set(&vol->ready, 1);
		kfastblock_volume_update_queue_gate(vol);
		kfastblock_volume_schedule_refresh(vol);
	}
	return ret;
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

enum kfastblock_volume_manual_refresh_scope {
	KFASTBLOCK_VOLUME_REFRESH_ALL = 0,
	KFASTBLOCK_VOLUME_REFRESH_CLUSTER = 1,
	KFASTBLOCK_VOLUME_REFRESH_IMAGE = 2,
};

enum kfastblock_volume_manual_transport_scope {
	KFASTBLOCK_VOLUME_TRANSPORT_ALL = 0,
	KFASTBLOCK_VOLUME_TRANSPORT_OSD = 1,
	KFASTBLOCK_VOLUME_TRANSPORT_MONITOR = 2,
};

static int kfastblock_volume_parse_refresh_scope(const char *buf, size_t count,
				 enum kfastblock_volume_manual_refresh_scope *scope)
{
	char *scratch;
	char *value;
	int ret = 0;

	if (!scope)
		return -EINVAL;

	scratch = kmemdup_nul(buf, count, GFP_KERNEL);
	if (!scratch)
		return -ENOMEM;
	value = strim(scratch);
	if (!*value || sysfs_streq(value, "1") || sysfs_streq(value, "all"))
		*scope = KFASTBLOCK_VOLUME_REFRESH_ALL;
	else if (sysfs_streq(value, "cluster"))
		*scope = KFASTBLOCK_VOLUME_REFRESH_CLUSTER;
	else if (sysfs_streq(value, "image"))
		*scope = KFASTBLOCK_VOLUME_REFRESH_IMAGE;
	else
		ret = -EINVAL;
	kfree(scratch);
	return ret;
}

static int kfastblock_volume_parse_transport_scope(const char *buf, size_t count,
				   enum kfastblock_volume_manual_transport_scope *scope)
{
	char *scratch;
	char *value;
	int ret = 0;

	if (!scope)
		return -EINVAL;

	scratch = kmemdup_nul(buf, count, GFP_KERNEL);
	if (!scratch)
		return -ENOMEM;
	value = strim(scratch);
	if (!*value || sysfs_streq(value, "1") || sysfs_streq(value, "all"))
		*scope = KFASTBLOCK_VOLUME_TRANSPORT_ALL;
	else if (sysfs_streq(value, "osd"))
		*scope = KFASTBLOCK_VOLUME_TRANSPORT_OSD;
	else if (sysfs_streq(value, "monitor"))
		*scope = KFASTBLOCK_VOLUME_TRANSPORT_MONITOR;
	else
		ret = -EINVAL;
	kfree(scratch);
	return ret;
}

static void kfastblock_volume_reset_osd_backoff(struct kfastblock_volume *vol)
{
	int i;

	if (!vol)
		return;

	for (i = 0; i < KFASTBLOCK_MAX_SOCKET_CACHE; ++i) {
		struct kfastblock_cached_socket *cached = &vol->socket_cache[i];

		mutex_lock(&cached->lock);
		cached->fail_streak = 0;
		cached->last_error = 0;
		cached->last_failure_jiffies = 0;
		cached->backoff_until_jiffies = 0;
		mutex_unlock(&cached->lock);
	}
}

static void kfastblock_volume_reset_monitor_backoff(struct kfastblock_volume *vol)
{
	int i;

	if (!vol)
		return;

	for (i = 0; i < KFASTBLOCK_MAX_MONITORS; ++i) {
		struct kfastblock_cached_monitor_socket *cached =
			&vol->monitor_cache[i];

		mutex_lock(&cached->lock);
		cached->fail_streak = 0;
		cached->last_error = 0;
		cached->last_failure_jiffies = 0;
		cached->backoff_until_jiffies = 0;
		mutex_unlock(&cached->lock);
	}
}

static int kfastblock_volume_refresh_locked(struct kfastblock_volume *vol,
				   bool refresh_cluster,
				   bool refresh_image,
				   bool warn_image_fail)
{
	u64 old_size;
	u64 old_osdmap_epoch;
	u64 old_pgmap_epoch;
	u32 old_block_size;
	u32 old_object_size;
	bool old_read_only;
	int ret = 0;
	int image_ret = 0;

	if (!vol)
		return -EINVAL;

	old_size = vol->view.image.size_bytes;
	old_osdmap_epoch = vol->view.osdmap_epoch;
	old_pgmap_epoch = vol->view.pgmap_epoch;
	old_block_size = vol->view.image.block_size;
	old_object_size = vol->view.image.object_size;
	old_read_only = vol->view.image.read_only;

	if (refresh_cluster)
		ret = kfastblock_transport_refresh_cluster_map_volume(vol);
	if (!ret && refresh_image)
		image_ret = kfastblock_transport_refresh_image_volume(vol);

	if (!ret && vol->disk) {
		if (refresh_cluster &&
		    (old_osdmap_epoch != vol->view.osdmap_epoch ||
		     old_pgmap_epoch != vol->view.pgmap_epoch))
			kfastblock_volume_close_osd_cached_sockets(vol);
		if (old_size != vol->view.image.size_bytes)
			set_capacity_and_notify(vol->disk,
					vol->view.image.size_bytes >> SECTOR_SHIFT);
		if ((old_block_size != vol->view.image.block_size &&
		     vol->view.image.block_size) ||
		    (old_object_size != vol->view.image.object_size &&
		     vol->view.image.object_size))
			kfastblock_volume_apply_queue_limits(vol);
		if (old_read_only != vol->view.image.read_only)
			set_disk_ro(vol->disk, vol->view.image.read_only);
		if (image_ret && warn_image_fail)
			dev_warn(&vol->dev, "image metadata refresh failed: %d\n", image_ret);
	} else if (ret) {
		kfastblock_volume_close_osd_cached_sockets(vol);
		kfastblock_volume_close_monitor_cached_sockets(vol);
		vol->view.sync_state = KFASTBLOCK_META_SYNC_STALE;
		kfastblock_volume_account_metadata_stale(vol);
	}

	kfastblock_volume_update_queue_gate(vol);
	if (ret)
		return ret;
	return image_ret;
}

static void kfastblock_volume_record_manual_op(struct kfastblock_volume *vol,
				      enum kfastblock_volume_event_type type,
				      u32 scope, int ret)
{
	if (!vol)
		return;

	kfastblock_volume_record_event(vol, type, ret, REQ_OP_FLUSH, 0, 0, scope, 0);
}

static ssize_t force_refresh_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);
	enum kfastblock_volume_manual_refresh_scope scope;
	bool refresh_cluster;
	bool refresh_image;
	int ret;

	if (!vol)
		return -ENODEV;
	ret = kfastblock_volume_parse_refresh_scope(buf, count, &scope);
	if (ret)
		return ret;

	refresh_cluster = scope != KFASTBLOCK_VOLUME_REFRESH_IMAGE;
	refresh_image = scope != KFASTBLOCK_VOLUME_REFRESH_CLUSTER;
	cancel_delayed_work_sync(&vol->refresh_work);
	down_write(&vol->state_lock);
	if (!atomic_read(&vol->ready)) {
		up_write(&vol->state_lock);
		return -ENODEV;
	}
	ret = kfastblock_volume_refresh_locked(vol, refresh_cluster, refresh_image, false);
	if (!ret)
		atomic64_inc(&vol->stats.manual_refreshes);
	kfastblock_volume_record_manual_op(vol, KFASTBLOCK_VOLUME_EVENT_MANUAL_REFRESH,
				      scope, ret);
	up_write(&vol->state_lock);
	if (atomic_read(&vol->ready))
		kfastblock_volume_schedule_refresh(vol);
	return ret ? ret : (ssize_t)count;
}

static ssize_t reset_backoff_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);
	enum kfastblock_volume_manual_transport_scope scope;
	int ret;

	if (!vol)
		return -ENODEV;
	ret = kfastblock_volume_parse_transport_scope(buf, count, &scope);
	if (ret)
		return ret;

	down_write(&vol->state_lock);
	if (scope != KFASTBLOCK_VOLUME_TRANSPORT_MONITOR)
		kfastblock_volume_reset_osd_backoff(vol);
	if (scope != KFASTBLOCK_VOLUME_TRANSPORT_OSD)
		kfastblock_volume_reset_monitor_backoff(vol);
	atomic64_inc(&vol->stats.manual_reset_backoffs);
	kfastblock_volume_record_manual_op(vol,
				      KFASTBLOCK_VOLUME_EVENT_MANUAL_RESET_BACKOFF,
				      scope, 0);
	up_write(&vol->state_lock);
	return count;
}

static ssize_t drop_transport_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);
	enum kfastblock_volume_manual_transport_scope scope;
	int ret;

	if (!vol)
		return -ENODEV;
	ret = kfastblock_volume_parse_transport_scope(buf, count, &scope);
	if (ret)
		return ret;

	down_write(&vol->state_lock);
	if (scope != KFASTBLOCK_VOLUME_TRANSPORT_MONITOR)
		kfastblock_volume_close_osd_cached_sockets(vol);
	if (scope != KFASTBLOCK_VOLUME_TRANSPORT_OSD)
		kfastblock_volume_close_monitor_cached_sockets(vol);
	atomic64_inc(&vol->stats.manual_transport_drops);
	kfastblock_volume_record_manual_op(vol,
				      KFASTBLOCK_VOLUME_EVENT_MANUAL_DROP_TRANSPORT,
				      scope, 0);
	up_write(&vol->state_lock);
	return count;
}

static ssize_t reset_leaders_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);
	int ret = 0;

	if (!vol)
		return -ENODEV;
	if (buf && count) {
		char *scratch = kmemdup_nul(buf, count, GFP_KERNEL);

		if (!scratch)
			return -ENOMEM;
		if (*strim(scratch) && !sysfs_streq(strim(scratch), "1") &&
		    !sysfs_streq(strim(scratch), "all"))
			ret = -EINVAL;
		kfree(scratch);
		if (ret)
			return ret;
	}

	down_write(&vol->state_lock);
	kfastblock_meta_invalidate_all_pg_leaders(&vol->view);
	atomic64_inc(&vol->stats.manual_leader_resets);
	kfastblock_volume_record_manual_op(vol,
				      KFASTBLOCK_VOLUME_EVENT_MANUAL_RESET_LEADERS,
				      0, 0);
	up_write(&vol->state_lock);
	return count;
}

static void kfastblock_volume_refresh_workfn(struct work_struct *work)
{
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct kfastblock_volume *vol = container_of(delayed_work,
					      struct kfastblock_volume,
					      refresh_work);
	bool refresh_image;

	if (!atomic_read(&vol->ready))
		return;

	down_write(&vol->state_lock);
	if (!atomic_read(&vol->ready)) {
		up_write(&vol->state_lock);
		return;
	}

	refresh_image = time_after_eq(
		jiffies,
		vol->view.last_image_refresh_jiffies +
		kfastblock_volume_image_refresh_interval());
	(void)kfastblock_volume_refresh_locked(vol, true, refresh_image, true);
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

static ssize_t open_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&vol->open_count));
}

static ssize_t queue_paused_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->queue_paused ? 1 : 0);
}

static ssize_t flush_in_progress_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", vol->flush_in_progress ? 1 : 0);
}

static ssize_t inflight_ios_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&vol->inflight_ios));
}

static ssize_t health_state_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 kfastblock_volume_health_state_name(vol->health.state));
}

static ssize_t health_since_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lu\n", vol->health.state_since_jiffies);
}

static ssize_t last_failure_errno_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%d\n", vol->health.last_errno);
}

static ssize_t last_failure_source_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 kfastblock_volume_failure_source_name(vol->health.last_failure_source));
}

static ssize_t last_failure_jiffies_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lu\n", vol->health.last_failure_jiffies);
}

static ssize_t last_success_jiffies_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lu\n", vol->health.last_success_jiffies);
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

static ssize_t refresh_kicks_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.refresh_kicks));
}

static ssize_t leader_invalidations_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.leader_invalidations));
}

static ssize_t osd_socket_drops_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.osd_socket_drops));
}

static ssize_t monitor_socket_drops_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.monitor_socket_drops));
}

static ssize_t osd_backoff_hits_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.osd_backoff_hits));
}

static ssize_t monitor_backoff_hits_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.monitor_backoff_hits));
}

static ssize_t osd_backoff_waits_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.osd_backoff_waits));
}

static ssize_t monitor_backoff_waits_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.monitor_backoff_waits));
}

static ssize_t manual_refreshes_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.manual_refreshes));
}

static ssize_t manual_reset_backoffs_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.manual_reset_backoffs));
}

static ssize_t manual_transport_drops_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.manual_transport_drops));
}

static ssize_t manual_leader_resets_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct kfastblock_volume *vol = dev_get_drvdata(dev);

	if (!vol)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			 atomic64_read(&vol->stats.manual_leader_resets));
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
static DEVICE_ATTR_RO(open_count);
static DEVICE_ATTR_RO(sync_state);
static DEVICE_ATTR_RO(queue_paused);
static DEVICE_ATTR_RO(flush_in_progress);
static DEVICE_ATTR_RO(inflight_ios);
static DEVICE_ATTR_RO(health_state);
static DEVICE_ATTR_RO(health_since);
static DEVICE_ATTR_RO(last_failure_errno);
static DEVICE_ATTR_RO(last_failure_source);
static DEVICE_ATTR_RO(last_failure_jiffies);
static DEVICE_ATTR_RO(last_success_jiffies);
static DEVICE_ATTR_RO(io_submitted);
static DEVICE_ATTR_RO(io_completed);
static DEVICE_ATTR_RO(io_failed);
static DEVICE_ATTR_RO(object_io_retries);
static DEVICE_ATTR_RO(metadata_stale_events);
static DEVICE_ATTR_RO(cluster_refresh_fail);
static DEVICE_ATTR_RO(image_refresh_fail);
static DEVICE_ATTR_RO(leader_query_fail);
static DEVICE_ATTR_RO(refresh_kicks);
static DEVICE_ATTR_RO(leader_invalidations);
static DEVICE_ATTR_RO(osd_socket_drops);
static DEVICE_ATTR_RO(monitor_socket_drops);
static DEVICE_ATTR_RO(osd_backoff_hits);
static DEVICE_ATTR_RO(monitor_backoff_hits);
static DEVICE_ATTR_RO(osd_backoff_waits);
static DEVICE_ATTR_RO(monitor_backoff_waits);
static DEVICE_ATTR_RO(manual_refreshes);
static DEVICE_ATTR_RO(manual_reset_backoffs);
static DEVICE_ATTR_RO(manual_transport_drops);
static DEVICE_ATTR_RO(manual_leader_resets);
static DEVICE_ATTR_WO(force_refresh);
static DEVICE_ATTR_WO(reset_backoff);
static DEVICE_ATTR_WO(drop_transport);
static DEVICE_ATTR_WO(reset_leaders);

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
	&dev_attr_open_count.attr,
	&dev_attr_sync_state.attr,
	&dev_attr_queue_paused.attr,
	&dev_attr_flush_in_progress.attr,
	&dev_attr_inflight_ios.attr,
	&dev_attr_health_state.attr,
	&dev_attr_health_since.attr,
	&dev_attr_last_failure_errno.attr,
	&dev_attr_last_failure_source.attr,
	&dev_attr_last_failure_jiffies.attr,
	&dev_attr_last_success_jiffies.attr,
	&dev_attr_io_submitted.attr,
	&dev_attr_io_completed.attr,
	&dev_attr_io_failed.attr,
	&dev_attr_object_io_retries.attr,
	&dev_attr_metadata_stale_events.attr,
	&dev_attr_cluster_refresh_fail.attr,
	&dev_attr_image_refresh_fail.attr,
	&dev_attr_leader_query_fail.attr,
	&dev_attr_refresh_kicks.attr,
	&dev_attr_leader_invalidations.attr,
	&dev_attr_osd_socket_drops.attr,
	&dev_attr_monitor_socket_drops.attr,
	&dev_attr_osd_backoff_hits.attr,
	&dev_attr_monitor_backoff_hits.attr,
	&dev_attr_osd_backoff_waits.attr,
	&dev_attr_monitor_backoff_waits.attr,
	&dev_attr_manual_refreshes.attr,
	&dev_attr_manual_reset_backoffs.attr,
	&dev_attr_manual_transport_drops.attr,
	&dev_attr_manual_leader_resets.attr,
	&dev_attr_force_refresh.attr,
	&dev_attr_reset_backoff.attr,
	&dev_attr_drop_transport.attr,
	&dev_attr_reset_leaders.attr,
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
	if (req_op(rq) == REQ_OP_FLUSH)
		return kfastblock_volume_handle_flush(vol, rq);
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
		kfastblock_volume_account_refresh_kick(vol, 0, -ESTALE);
		kfastblock_volume_kick_refresh(vol);
		blk_mq_end_request(rq, BLK_STS_RESOURCE);
		return BLK_STS_RESOURCE;
	}
	mutex_lock(&vol->inflight_lock);
	if (vol->flush_in_progress) {
		mutex_unlock(&vol->inflight_lock);
		up_read(&vol->state_lock);
		blk_mq_end_request(rq, BLK_STS_RESOURCE);
		return BLK_STS_RESOURCE;
	}
	kfastblock_volume_get_io(vol);
	mutex_unlock(&vol->inflight_lock);
	kfastblock_request_init(kf_req, vol, rq);
	ret = kfastblock_request_split(kf_req);
	up_read(&vol->state_lock);
	if (ret) {
		kfastblock_volume_put_io(vol);
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}

	kfastblock_volume_account_io_submit(vol, req_op(rq), blk_rq_bytes(rq));
	ret = kfastblock_transport_submit(kf_req);
	if (ret) {
		kfastblock_volume_account_io_complete(vol, ret);
		kfastblock_volume_put_io(vol);
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
	kfastblock_volume_apply_queue_limits(vol);
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

	mutex_lock(&g_kfastblock_volumes_lock);
	if (kfastblock_volume_find_locked(spec->pool_name, spec->image_name)) {
		mutex_unlock(&g_kfastblock_volumes_lock);
		return -EEXIST;
	}
	mutex_unlock(&g_kfastblock_volumes_lock);

	vol = kzalloc(sizeof(*vol), GFP_KERNEL);
	if (!vol)
		return -ENOMEM;

	vol->dev_id = -1;
	vol->major = major;
	atomic_set(&vol->open_count, 0);
	atomic_set(&vol->ready, 0);
	atomic_set(&vol->inflight_ios, 0);
	vol->queue_paused = false;
	vol->flush_in_progress = false;
	kfastblock_volume_stats_init(vol);
	mutex_init(&vol->inflight_lock);
	init_waitqueue_head(&vol->inflight_wq);
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
	if (kfastblock_volume_find_locked(vol->view.image.pool_name,
					 vol->view.image.image_name)) {
		mutex_unlock(&g_kfastblock_volumes_lock);
		ret = -EEXIST;
		goto err_unregister_disk;
	}
	list_add_tail(&vol->node, &g_kfastblock_volumes);
	mutex_unlock(&g_kfastblock_volumes_lock);

	atomic_set(&vol->ready, 1);
	kfastblock_volume_record_event(
		vol, KFASTBLOCK_VOLUME_EVENT_ATTACH_READY, 0, REQ_OP_FLUSH,
		0, 0, vol->view.image.pool_id, vol->view.image.pg_count);
	kfastblock_volume_mark_success(vol, KFASTBLOCK_VOLUME_SOURCE_ATTACH);
	kfastblock_volume_update_queue_gate(vol);
	kfastblock_volume_schedule_refresh(vol);
	return 0;

err_unregister_disk:
	if (vol->disk) {
		del_gendisk(vol->disk);
		put_disk(vol->disk);
		vol->disk = NULL;
	}
	device_del(&vol->dev);
err_free:
	cancel_delayed_work_sync(&vol->refresh_work);
	if (vol->tag_set.tags)
		blk_mq_free_tag_set(&vol->tag_set);
	if (vol->dev_id >= 0)
		ida_free(&g_kfastblock_dev_ida, vol->dev_id);
	kfastblock_volume_close_cached_sockets(vol);
	kfastblock_control_cleanup_attach_spec(&vol->spec);
	kfastblock_meta_cleanup_view(&vol->view);
	kfree(vol);
	return ret;
}

int kfastblock_volume_detach(const struct kfastblock_attach_spec *spec)
{
	struct kfastblock_volume *vol;
	struct kfastblock_volume *found = NULL;
	int ret;

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
	if (atomic_read(&found->open_count) > 0)
		return -EBUSY;

	ret = kfastblock_volume_begin_stop(found, true);
	if (ret)
		return ret;
	if (found->disk) {
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

	for (;;) {
		mutex_lock(&g_kfastblock_volumes_lock);
		if (list_empty(&g_kfastblock_volumes)) {
			mutex_unlock(&g_kfastblock_volumes_lock);
			break;
		}
		vol = list_first_entry(&g_kfastblock_volumes,
					 struct kfastblock_volume, node);
		list_del_init(&vol->node);
		mutex_unlock(&g_kfastblock_volumes_lock);

		(void)kfastblock_volume_begin_stop(vol, false);
		if (vol->disk) {
			del_gendisk(vol->disk);
			put_disk(vol->disk);
			vol->disk = NULL;
		}
		device_unregister(&vol->dev);
	}

	debugfs_remove_recursive(g_kfastblock_debugfs_root);
	g_kfastblock_debugfs_root = NULL;
	ida_destroy(&g_kfastblock_dev_ida);
}
