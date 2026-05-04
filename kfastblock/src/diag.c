#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "kfastblock/buffer.h"
#include "kfastblock/diag.h"
#include "kfastblock/fault.h"
#include "kfastblock/scheduler.h"
#include "kfastblock/selfcheck.h"
#include "kfastblock/volume.h"

static const char *kfastblock_diag_health_state_name(u32 state)
{
	switch (state) {
	case KFASTBLOCK_VOLUME_HEALTH_UNKNOWN:
		return "unknown";
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

static const char *kfastblock_diag_failure_source_name(u32 source)
{
	switch (source) {
	case KFASTBLOCK_VOLUME_SOURCE_NONE:
		return "none";
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
		return "unknown";
	}
}

static void kfastblock_diag_collect_volume(struct kfastblock_volume *vol,
					   struct kfastblock_diag_snapshot *snapshot)
{
	unsigned long flags;

	if (!vol || !snapshot)
		return;

	down_read(&vol->state_lock);
	strscpy(snapshot->volume.disk_name,
		vol->disk ? vol->disk->disk_name : "",
		sizeof(snapshot->volume.disk_name));
	strscpy(snapshot->volume.pool_name, vol->view.image.pool_name,
		sizeof(snapshot->volume.pool_name));
	strscpy(snapshot->volume.image_name, vol->view.image.image_name,
		sizeof(snapshot->volume.image_name));
	snapshot->volume.ready = atomic_read(&vol->ready);
	snapshot->volume.queue_paused = vol->queue_paused ? 1 : 0;
	snapshot->volume.manual_queue_pause = vol->manual_queue_pause ? 1 : 0;
	snapshot->volume.open_count = atomic_read(&vol->open_count);
	snapshot->volume.inflight_ios = atomic_read(&vol->inflight_ios);
	snapshot->volume.health_state = vol->health.state;
	snapshot->volume.last_failure_source = vol->health.last_failure_source;
	snapshot->volume.last_failure_errno = vol->health.last_errno;
	snapshot->volume.health_since_jiffies = vol->health.state_since_jiffies;
	snapshot->volume.last_failure_jiffies = vol->health.last_failure_jiffies;
	snapshot->volume.last_success_jiffies = vol->health.last_success_jiffies;
	snapshot->volume.size_bytes = vol->view.image.size_bytes;
	snapshot->volume.block_size = vol->view.image.block_size;
	snapshot->volume.object_size = vol->view.image.object_size;
	snapshot->volume.pool_id = vol->view.image.pool_id;
	snapshot->volume.pg_count = vol->view.image.pg_count;
	snapshot->volume.read_only = vol->view.image.read_only ? 1 : 0;
	snapshot->volume.sync_state = vol->view.sync_state;
	snapshot->volume.image_epoch = vol->view.image_epoch;
	snapshot->volume.osdmap_epoch = vol->view.osdmap_epoch;
	snapshot->volume.pgmap_epoch = vol->view.pgmap_epoch;
	snapshot->volume.leader_epoch = vol->view.leader_epoch;
	snapshot->volume.osd_count = vol->view.osd_count;
	snapshot->volume.route_count = vol->view.route_count;
	snapshot->volume.dispatch_window = vol->dispatch_window;
	snapshot->volume.refresh_interval_ms = vol->refresh_interval_ms;
	snapshot->volume.image_refresh_interval_ms =
		vol->image_refresh_interval_ms;
	snapshot->volume.last_refresh_jiffies = vol->view.last_refresh_jiffies;
	snapshot->volume.last_image_refresh_jiffies =
		vol->view.last_image_refresh_jiffies;
	up_read(&vol->state_lock);

	spin_lock_irqsave(&vol->event_log.lock, flags);
	snapshot->volume.event_count = vol->event_log.count;
	spin_unlock_irqrestore(&vol->event_log.lock, flags);
}

static void kfastblock_diag_collect_buffer(struct kfastblock_volume *vol,
					   struct kfastblock_diag_snapshot *snapshot)
{
	if (!vol || !snapshot)
		return;

	snapshot->buffer.cached =
		kfastblock_buffer_pool_cached(&vol->object_buffer_pool);
	snapshot->buffer.cache_limit =
		kfastblock_buffer_pool_limit(&vol->object_buffer_pool);
	snapshot->buffer.chunk_bytes =
		kfastblock_buffer_pool_chunk_bytes(&vol->object_buffer_pool);
	snapshot->buffer.hits =
		kfastblock_buffer_pool_hits(&vol->object_buffer_pool);
	snapshot->buffer.misses =
		kfastblock_buffer_pool_misses(&vol->object_buffer_pool);
	snapshot->buffer.returns =
		kfastblock_buffer_pool_returns(&vol->object_buffer_pool);
	snapshot->buffer.evictions =
		kfastblock_buffer_pool_evictions(&vol->object_buffer_pool);
	snapshot->buffer.direct_allocs =
		kfastblock_buffer_pool_direct_allocs(&vol->object_buffer_pool);
}

static void kfastblock_diag_collect_scheduler(
	struct kfastblock_volume *vol,
	struct kfastblock_diag_snapshot *snapshot)
{
	struct kfastblock_scheduler_controller *sched;

	if (!vol || !snapshot)
		return;

	sched = &vol->scheduler;
	snapshot->scheduler.current_window =
		kfastblock_scheduler_current_window(sched);
	snapshot->scheduler.base_window =
		kfastblock_scheduler_base_window(sched);
	snapshot->scheduler.min_window =
		kfastblock_scheduler_min_window(sched);
	snapshot->scheduler.max_window =
		kfastblock_scheduler_max_window(sched);
	snapshot->scheduler.pressure_inflight_limit =
		kfastblock_scheduler_pressure_inflight_limit(sched);
	snapshot->scheduler.cooldown_ms =
		kfastblock_scheduler_cooldown_ms(sched);
	snapshot->scheduler.cooldown_remaining_ms =
		kfastblock_scheduler_cooldown_remaining_ms(sched);
	snapshot->scheduler.grow_events =
		kfastblock_scheduler_grow_events(sched);
	snapshot->scheduler.shrink_events =
		kfastblock_scheduler_shrink_events(sched);
	snapshot->scheduler.retry_events =
		kfastblock_scheduler_retry_events(sched);
	snapshot->scheduler.dispatch_failures =
		kfastblock_scheduler_dispatch_failures(sched);
	snapshot->scheduler.sample_events =
		kfastblock_scheduler_sample_events(sched);
	snapshot->scheduler.pressure_limited_samples =
		kfastblock_scheduler_pressure_limited_samples(sched);
	snapshot->scheduler.cooldown_limited_samples =
		kfastblock_scheduler_cooldown_limited_samples(sched);
	snapshot->scheduler.cooldown_events =
		kfastblock_scheduler_cooldown_events(sched);
	snapshot->scheduler.last_sample_inflight_ios =
		kfastblock_scheduler_last_sample_inflight_ios(sched);
	snapshot->scheduler.last_sample_request_objects =
		kfastblock_scheduler_last_sample_request_objects(sched);
	snapshot->scheduler.last_sample_controller_window =
		kfastblock_scheduler_last_sample_controller_window(sched);
	snapshot->scheduler.last_sample_pressure_window =
		kfastblock_scheduler_last_sample_pressure_window(sched);
	snapshot->scheduler.last_sample_effective_window =
		kfastblock_scheduler_last_sample_effective_window(sched);
	snapshot->scheduler.policy =
		kfastblock_scheduler_policy(sched);
	snapshot->scheduler.dynamic_enabled =
		kfastblock_scheduler_dynamic_enabled(sched);
	snapshot->scheduler.cooldown_active =
		kfastblock_scheduler_cooldown_active(sched);
}

static void kfastblock_diag_collect_selfcheck(
	struct kfastblock_volume *vol,
	struct kfastblock_diag_snapshot *snapshot)
{
	if (!vol || !snapshot)
		return;

	snapshot->selfcheck.run_count =
		kfastblock_selfcheck_run_count(&vol->selfcheck);
	snapshot->selfcheck.failure_runs =
		kfastblock_selfcheck_failure_runs(&vol->selfcheck);
	snapshot->selfcheck.warning_runs =
		kfastblock_selfcheck_warning_runs(&vol->selfcheck);
	snapshot->selfcheck.last_errno =
		kfastblock_selfcheck_last_errno(&vol->selfcheck);
	snapshot->selfcheck.last_failed_checks =
		kfastblock_selfcheck_last_failed_checks(&vol->selfcheck);
	snapshot->selfcheck.last_warning_checks =
		kfastblock_selfcheck_last_warning_checks(&vol->selfcheck);
	snapshot->selfcheck.last_flags =
		kfastblock_selfcheck_last_flags(&vol->selfcheck);
	snapshot->selfcheck.last_run_jiffies =
		kfastblock_selfcheck_last_run_jiffies(&vol->selfcheck);
}

static void kfastblock_diag_collect_fault(struct kfastblock_volume *vol,
					  struct kfastblock_diag_snapshot *snapshot)
{
	if (!vol || !snapshot)
		return;

	snapshot->fault.enabled =
		kfastblock_fault_injection_enabled(&vol->fault_injection);
	snapshot->fault.mask =
		kfastblock_fault_injection_mask(&vol->fault_injection);
	snapshot->fault.errno_value =
		kfastblock_fault_injection_errno(&vol->fault_injection);
	snapshot->fault.budget =
		kfastblock_fault_injection_budget(&vol->fault_injection);
	snapshot->fault.arm_count =
		kfastblock_fault_injection_arm_count(&vol->fault_injection);
	snapshot->fault.reset_count =
		kfastblock_fault_injection_reset_count(&vol->fault_injection);
	snapshot->fault.hit_count =
		kfastblock_fault_injection_hit_count(&vol->fault_injection);
	snapshot->fault.skip_count =
		kfastblock_fault_injection_skip_count(&vol->fault_injection);
	snapshot->fault.last_arm_jiffies =
		kfastblock_fault_injection_last_arm_jiffies(
			&vol->fault_injection);
	snapshot->fault.last_reset_jiffies =
		kfastblock_fault_injection_last_reset_jiffies(
			&vol->fault_injection);
	snapshot->fault.last_trigger_jiffies =
		kfastblock_fault_injection_last_trigger_jiffies(
			&vol->fault_injection);
	snapshot->fault.last_site =
		kfastblock_fault_injection_last_site(&vol->fault_injection);
	snapshot->fault.last_errno =
		kfastblock_fault_injection_last_errno(&vol->fault_injection);
	kfastblock_fault_format_mask(snapshot->fault.mask,
				     snapshot->fault.mask_text,
				     sizeof(snapshot->fault.mask_text));
	strscpy(snapshot->fault.last_site_text,
		kfastblock_fault_site_name(snapshot->fault.last_site),
		sizeof(snapshot->fault.last_site_text));
}

void kfastblock_diag_collect(struct kfastblock_volume *vol,
			     struct kfastblock_diag_snapshot *snapshot)
{
	if (!snapshot)
		return;

	memset(snapshot, 0, sizeof(*snapshot));
	if (!vol)
		return;

	kfastblock_diag_collect_volume(vol, snapshot);
	kfastblock_diag_collect_buffer(vol, snapshot);
	kfastblock_diag_collect_scheduler(vol, snapshot);
	kfastblock_osd_conn_pool_snapshot(vol->socket_cache,
					  KFASTBLOCK_MAX_SOCKET_CACHE,
					  &snapshot->osd_conn);
	kfastblock_monitor_conn_pool_snapshot(vol->monitor_cache,
					      KFASTBLOCK_MAX_MONITORS,
					      &snapshot->monitor_conn);
	kfastblock_diag_collect_selfcheck(vol, snapshot);
	kfastblock_diag_collect_fault(vol, snapshot);
}

int kfastblock_diag_dump_seq(struct seq_file *m,
			     const struct kfastblock_diag_snapshot *snapshot)
{
	if (!m || !snapshot)
		return -EINVAL;

	seq_printf(m, "volume.disk=%s\n", snapshot->volume.disk_name);
	seq_printf(m, "volume.pool=%s\n", snapshot->volume.pool_name);
	seq_printf(m, "volume.image=%s\n", snapshot->volume.image_name);
	seq_printf(m, "volume.ready=%d\n", snapshot->volume.ready);
	seq_printf(m, "volume.queue_paused=%d\n",
		   snapshot->volume.queue_paused);
	seq_printf(m, "volume.manual_queue_pause=%d\n",
		   snapshot->volume.manual_queue_pause);
	seq_printf(m, "volume.open_count=%d\n", snapshot->volume.open_count);
	seq_printf(m, "volume.inflight_ios=%d\n", snapshot->volume.inflight_ios);
	seq_printf(m, "volume.health_state=%s\n",
		   kfastblock_diag_health_state_name(
			   snapshot->volume.health_state));
	seq_printf(m, "volume.last_failure_source=%s\n",
		   kfastblock_diag_failure_source_name(
			   snapshot->volume.last_failure_source));
	seq_printf(m, "volume.last_failure_errno=%d\n",
		   snapshot->volume.last_failure_errno);
	seq_printf(m, "volume.health_since_jiffies=%lu\n",
		   snapshot->volume.health_since_jiffies);
	seq_printf(m, "volume.last_failure_jiffies=%lu\n",
		   snapshot->volume.last_failure_jiffies);
	seq_printf(m, "volume.last_success_jiffies=%lu\n",
		   snapshot->volume.last_success_jiffies);
	seq_printf(m, "volume.size_bytes=%llu\n", snapshot->volume.size_bytes);
	seq_printf(m, "volume.block_size=%u\n", snapshot->volume.block_size);
	seq_printf(m, "volume.object_size=%u\n", snapshot->volume.object_size);
	seq_printf(m, "volume.pool_id=%u\n", snapshot->volume.pool_id);
	seq_printf(m, "volume.pg_count=%u\n", snapshot->volume.pg_count);
	seq_printf(m, "volume.read_only=%u\n", snapshot->volume.read_only);
	seq_printf(m, "volume.sync_state=%u\n", snapshot->volume.sync_state);
	seq_printf(m, "volume.image_epoch=%llu\n", snapshot->volume.image_epoch);
	seq_printf(m, "volume.osdmap_epoch=%llu\n",
		   snapshot->volume.osdmap_epoch);
	seq_printf(m, "volume.pgmap_epoch=%llu\n",
		   snapshot->volume.pgmap_epoch);
	seq_printf(m, "volume.leader_epoch=%llu\n",
		   snapshot->volume.leader_epoch);
	seq_printf(m, "volume.osd_count=%u\n", snapshot->volume.osd_count);
	seq_printf(m, "volume.route_count=%u\n", snapshot->volume.route_count);
	seq_printf(m, "volume.dispatch_window=%u\n",
		   snapshot->volume.dispatch_window);
	seq_printf(m, "volume.refresh_interval_ms=%u\n",
		   snapshot->volume.refresh_interval_ms);
	seq_printf(m, "volume.image_refresh_interval_ms=%u\n",
		   snapshot->volume.image_refresh_interval_ms);
	seq_printf(m, "volume.last_refresh_jiffies=%lu\n",
		   snapshot->volume.last_refresh_jiffies);
	seq_printf(m, "volume.last_image_refresh_jiffies=%lu\n",
		   snapshot->volume.last_image_refresh_jiffies);
	seq_printf(m, "volume.event_count=%u\n", snapshot->volume.event_count);

	seq_printf(m, "buffer.cached=%u\n", snapshot->buffer.cached);
	seq_printf(m, "buffer.cache_limit=%u\n", snapshot->buffer.cache_limit);
	seq_printf(m, "buffer.chunk_bytes=%u\n", snapshot->buffer.chunk_bytes);
	seq_printf(m, "buffer.hits=%llu\n", snapshot->buffer.hits);
	seq_printf(m, "buffer.misses=%llu\n", snapshot->buffer.misses);
	seq_printf(m, "buffer.returns=%llu\n", snapshot->buffer.returns);
	seq_printf(m, "buffer.evictions=%llu\n", snapshot->buffer.evictions);
	seq_printf(m, "buffer.direct_allocs=%llu\n",
		   snapshot->buffer.direct_allocs);

	seq_printf(m, "scheduler.current_window=%u\n",
		   snapshot->scheduler.current_window);
	seq_printf(m, "scheduler.base_window=%u\n",
		   snapshot->scheduler.base_window);
	seq_printf(m, "scheduler.min_window=%u\n",
		   snapshot->scheduler.min_window);
	seq_printf(m, "scheduler.max_window=%u\n",
		   snapshot->scheduler.max_window);
	seq_printf(m, "scheduler.policy=%s\n",
		   kfastblock_scheduler_policy_name(
			   snapshot->scheduler.policy));
	seq_printf(m, "scheduler.dynamic_enabled=%u\n",
		   snapshot->scheduler.dynamic_enabled ? 1 : 0);
	seq_printf(m, "scheduler.pressure_inflight_limit=%u\n",
		   snapshot->scheduler.pressure_inflight_limit);
	seq_printf(m, "scheduler.cooldown_ms=%u\n",
		   snapshot->scheduler.cooldown_ms);
	seq_printf(m, "scheduler.cooldown_active=%u\n",
		   snapshot->scheduler.cooldown_active ? 1 : 0);
	seq_printf(m, "scheduler.cooldown_remaining_ms=%u\n",
		   snapshot->scheduler.cooldown_remaining_ms);
	seq_printf(m, "scheduler.grow_events=%u\n",
		   snapshot->scheduler.grow_events);
	seq_printf(m, "scheduler.shrink_events=%u\n",
		   snapshot->scheduler.shrink_events);
	seq_printf(m, "scheduler.retry_events=%u\n",
		   snapshot->scheduler.retry_events);
	seq_printf(m, "scheduler.dispatch_failures=%u\n",
		   snapshot->scheduler.dispatch_failures);
	seq_printf(m, "scheduler.sample_events=%u\n",
		   snapshot->scheduler.sample_events);
	seq_printf(m, "scheduler.pressure_limited_samples=%u\n",
		   snapshot->scheduler.pressure_limited_samples);
	seq_printf(m, "scheduler.cooldown_limited_samples=%u\n",
		   snapshot->scheduler.cooldown_limited_samples);
	seq_printf(m, "scheduler.cooldown_events=%u\n",
		   snapshot->scheduler.cooldown_events);
	seq_printf(m, "scheduler.last_sample_inflight_ios=%u\n",
		   snapshot->scheduler.last_sample_inflight_ios);
	seq_printf(m, "scheduler.last_sample_request_objects=%u\n",
		   snapshot->scheduler.last_sample_request_objects);
	seq_printf(m, "scheduler.last_sample_controller_window=%u\n",
		   snapshot->scheduler.last_sample_controller_window);
	seq_printf(m, "scheduler.last_sample_pressure_window=%u\n",
		   snapshot->scheduler.last_sample_pressure_window);
	seq_printf(m, "scheduler.last_sample_effective_window=%u\n",
		   snapshot->scheduler.last_sample_effective_window);

	seq_printf(m, "osd_conn.total_slots=%u\n", snapshot->osd_conn.total_slots);
	seq_printf(m, "osd_conn.empty_slots=%u\n", snapshot->osd_conn.empty_slots);
	seq_printf(m, "osd_conn.connecting_slots=%u\n",
		   snapshot->osd_conn.connecting_slots);
	seq_printf(m, "osd_conn.ready_slots=%u\n", snapshot->osd_conn.ready_slots);
	seq_printf(m, "osd_conn.backoff_slots=%u\n",
		   snapshot->osd_conn.backoff_slots);
	seq_printf(m, "osd_conn.active_sockets=%u\n",
		   snapshot->osd_conn.active_sockets);
	seq_printf(m, "osd_conn.min_health_score=%u\n",
		   snapshot->osd_conn.min_health_score);
	seq_printf(m, "osd_conn.max_health_score=%u\n",
		   snapshot->osd_conn.max_health_score);
	seq_printf(m, "osd_conn.avg_health_score=%u\n",
		   snapshot->osd_conn.avg_health_score);
	seq_printf(m, "osd_conn.connect_attempts=%llu\n",
		   snapshot->osd_conn.connect_attempts);
	seq_printf(m, "osd_conn.reuse_hits=%llu\n",
		   snapshot->osd_conn.reuse_hits);
	seq_printf(m, "osd_conn.success_count=%llu\n",
		   snapshot->osd_conn.success_count);
	seq_printf(m, "osd_conn.failure_count=%llu\n",
		   snapshot->osd_conn.failure_count);
	seq_printf(m, "osd_conn.oldest_last_use_jiffies=%lu\n",
		   snapshot->osd_conn.oldest_last_use_jiffies);
	seq_printf(m, "osd_conn.newest_last_use_jiffies=%lu\n",
		   snapshot->osd_conn.newest_last_use_jiffies);

	seq_printf(m, "monitor_conn.total_slots=%u\n",
		   snapshot->monitor_conn.total_slots);
	seq_printf(m, "monitor_conn.empty_slots=%u\n",
		   snapshot->monitor_conn.empty_slots);
	seq_printf(m, "monitor_conn.connecting_slots=%u\n",
		   snapshot->monitor_conn.connecting_slots);
	seq_printf(m, "monitor_conn.ready_slots=%u\n",
		   snapshot->monitor_conn.ready_slots);
	seq_printf(m, "monitor_conn.backoff_slots=%u\n",
		   snapshot->monitor_conn.backoff_slots);
	seq_printf(m, "monitor_conn.active_sockets=%u\n",
		   snapshot->monitor_conn.active_sockets);
	seq_printf(m, "monitor_conn.min_health_score=%u\n",
		   snapshot->monitor_conn.min_health_score);
	seq_printf(m, "monitor_conn.max_health_score=%u\n",
		   snapshot->monitor_conn.max_health_score);
	seq_printf(m, "monitor_conn.avg_health_score=%u\n",
		   snapshot->monitor_conn.avg_health_score);
	seq_printf(m, "monitor_conn.connect_attempts=%llu\n",
		   snapshot->monitor_conn.connect_attempts);
	seq_printf(m, "monitor_conn.reuse_hits=%llu\n",
		   snapshot->monitor_conn.reuse_hits);
	seq_printf(m, "monitor_conn.success_count=%llu\n",
		   snapshot->monitor_conn.success_count);
	seq_printf(m, "monitor_conn.failure_count=%llu\n",
		   snapshot->monitor_conn.failure_count);
	seq_printf(m, "monitor_conn.oldest_last_use_jiffies=%lu\n",
		   snapshot->monitor_conn.oldest_last_use_jiffies);
	seq_printf(m, "monitor_conn.newest_last_use_jiffies=%lu\n",
		   snapshot->monitor_conn.newest_last_use_jiffies);

	seq_printf(m, "selfcheck.run_count=%u\n", snapshot->selfcheck.run_count);
	seq_printf(m, "selfcheck.failure_runs=%u\n",
		   snapshot->selfcheck.failure_runs);
	seq_printf(m, "selfcheck.warning_runs=%u\n",
		   snapshot->selfcheck.warning_runs);
	seq_printf(m, "selfcheck.last_errno=%d\n",
		   snapshot->selfcheck.last_errno);
	seq_printf(m, "selfcheck.last_failed_checks=%u\n",
		   snapshot->selfcheck.last_failed_checks);
	seq_printf(m, "selfcheck.last_warning_checks=%u\n",
		   snapshot->selfcheck.last_warning_checks);
	seq_printf(m, "selfcheck.last_flags=0x%x\n",
		   snapshot->selfcheck.last_flags);
	seq_printf(m, "selfcheck.last_run_jiffies=%lu\n",
		   snapshot->selfcheck.last_run_jiffies);

	seq_printf(m, "fault.enabled=%u\n", snapshot->fault.enabled ? 1 : 0);
	seq_printf(m, "fault.mask=%s\n", snapshot->fault.mask_text);
	seq_printf(m, "fault.errno_value=%d\n", snapshot->fault.errno_value);
	seq_printf(m, "fault.budget=%u\n", snapshot->fault.budget);
	seq_printf(m, "fault.arm_count=%u\n", snapshot->fault.arm_count);
	seq_printf(m, "fault.reset_count=%u\n", snapshot->fault.reset_count);
	seq_printf(m, "fault.hit_count=%u\n", snapshot->fault.hit_count);
	seq_printf(m, "fault.skip_count=%u\n", snapshot->fault.skip_count);
	seq_printf(m, "fault.last_site=%s\n", snapshot->fault.last_site_text);
	seq_printf(m, "fault.last_errno=%d\n", snapshot->fault.last_errno);
	seq_printf(m, "fault.last_arm_jiffies=%lu\n",
		   snapshot->fault.last_arm_jiffies);
	seq_printf(m, "fault.last_reset_jiffies=%lu\n",
		   snapshot->fault.last_reset_jiffies);
	seq_printf(m, "fault.last_trigger_jiffies=%lu\n",
		   snapshot->fault.last_trigger_jiffies);
	return 0;
}
