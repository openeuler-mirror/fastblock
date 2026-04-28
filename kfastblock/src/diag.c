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

static const char *kfastblock_diag_event_type_name(u32 type)
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
	case KFASTBLOCK_VOLUME_EVENT_OBJECT_DONE:
		return "object_done";
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
	case KFASTBLOCK_VOLUME_EVENT_MANUAL_QUEUE_PAUSE:
		return "manual_queue_pause";
	case KFASTBLOCK_VOLUME_EVENT_MANUAL_QUEUE_RESUME:
		return "manual_queue_resume";
	case KFASTBLOCK_VOLUME_EVENT_SOCKET_BACKOFF_WAIT:
		return "socket_backoff_wait";
	case KFASTBLOCK_VOLUME_EVENT_MANUAL_FAULT_INJECTION_ARM:
		return "manual_fault_injection_arm";
	case KFASTBLOCK_VOLUME_EVENT_MANUAL_FAULT_INJECTION_RESET:
		return "manual_fault_injection_reset";
	case KFASTBLOCK_VOLUME_EVENT_FAULT_INJECTION_TRIGGER:
		return "fault_injection_trigger";
	default:
		return "unknown";
	}
}

const char *kfastblock_diag_anomaly_status(u32 score)
{
	if (score >= 60)
		return "critical";
	if (score >= 25)
		return "warn";
	return "ok";
}

const char *kfastblock_diag_drift_status(bool valid, u32 score)
{
	if (!valid)
		return "unset";
	if (score >= 40)
		return "critical";
	if (score >= 15)
		return "warn";
	return "ok";
}

static void kfastblock_diag_collect_volume(struct kfastblock_volume *vol,
					   struct kfastblock_diag_snapshot *snapshot)
{
	unsigned long flags;

	if (!vol || !snapshot)
		return;

	down_read(&vol->state_lock);
	strscpy(snapshot->volume.disk_name,
		vol->disk_name,
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

static void kfastblock_diag_collect_pipeline(
	struct kfastblock_volume *vol,
	struct kfastblock_diag_snapshot *snapshot)
{
	struct kfastblock_pipeline_snapshot pipe_snapshot = {};

	if (!vol || !snapshot)
		return;

	kfastblock_volume_get_pipeline_snapshot(vol, &pipe_snapshot);
	snapshot->pipeline.capacity = pipe_snapshot.capacity;
	snapshot->pipeline.inflight = pipe_snapshot.inflight;
	snapshot->pipeline.peak_inflight = pipe_snapshot.peak_inflight;
	snapshot->pipeline.free_entries = pipe_snapshot.free_entries;
	if (pipe_snapshot.capacity)
		snapshot->pipeline.utilization_pct =
			(pipe_snapshot.inflight * 100) / pipe_snapshot.capacity;
	snapshot->pipeline.request_prepares =
		atomic64_read(&vol->pipeline_stats.request_prepares);
	snapshot->pipeline.request_cleanups =
		atomic64_read(&vol->pipeline_stats.request_cleanups);
	snapshot->pipeline.dispatch_batches =
		atomic64_read(&vol->pipeline_stats.dispatch_batches);
	snapshot->pipeline.queued_objects =
		atomic64_read(&vol->pipeline_stats.queued_objects);
	snapshot->pipeline.retry_objects =
		atomic64_read(&vol->pipeline_stats.retry_objects);
	snapshot->pipeline.completed_objects =
		atomic64_read(&vol->pipeline_stats.completed_objects);
	snapshot->pipeline.failed_objects =
		atomic64_read(&vol->pipeline_stats.failed_objects);
	snapshot->pipeline.cancelled_objects =
		atomic64_read(&vol->pipeline_stats.cancelled_objects);
	snapshot->pipeline.seq_records =
		atomic64_read(&vol->pipeline_stats.seq_records);
	snapshot->pipeline.last_response_status =
		atomic_read(&vol->pipeline_stats.last_response_status);
	snapshot->pipeline.last_response_body_len =
		(u32)atomic_read(&vol->pipeline_stats.last_response_body_len);
	snapshot->pipeline.last_transport_flags =
		(u32)atomic_read(&vol->pipeline_stats.last_transport_flags);
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

static void kfastblock_diag_collect_events(struct kfastblock_volume *vol,
					   struct kfastblock_diag_snapshot *snapshot)
{
	unsigned long flags;
	u32 count;
	u32 start;
	u32 i;

	if (!vol || !snapshot)
		return;

	spin_lock_irqsave(&vol->event_log.lock, flags);
	count = vol->event_log.count;
	start = (vol->event_log.next_index + KFASTBLOCK_MAX_VOLUME_EVENTS - count) %
		KFASTBLOCK_MAX_VOLUME_EVENTS;
	snapshot->events.total_events = count;
	for (i = 0; i < count; ++i) {
		const struct kfastblock_volume_event *event =
			&vol->event_log.entries[(start + i) %
						KFASTBLOCK_MAX_VOLUME_EVENTS];

		if (!snapshot->events.oldest_jiffies ||
		    event->jiffies < snapshot->events.oldest_jiffies)
			snapshot->events.oldest_jiffies = event->jiffies;
		if (event->jiffies >= snapshot->events.newest_jiffies) {
			snapshot->events.newest_jiffies = event->jiffies;
			snapshot->events.last_type = event->type;
			snapshot->events.last_errno = event->ret;
		}

		switch (event->type) {
		case KFASTBLOCK_VOLUME_EVENT_HEALTH_CHANGE:
			snapshot->events.health_change_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_METADATA_STALE:
			snapshot->events.metadata_stale_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_CLUSTER_REFRESH_FAIL:
			snapshot->events.cluster_refresh_fail_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_IMAGE_REFRESH_FAIL:
			snapshot->events.image_refresh_fail_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_LEADER_QUERY_FAIL:
			snapshot->events.leader_query_fail_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_OBJECT_RETRY:
			snapshot->events.object_retry_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_OBJECT_ERROR:
			snapshot->events.object_error_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_OSD_SOCKET_DROP:
		case KFASTBLOCK_VOLUME_EVENT_MONITOR_SOCKET_DROP:
			snapshot->events.socket_drop_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_SOCKET_BACKOFF:
			snapshot->events.socket_backoff_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_SOCKET_BACKOFF_WAIT:
			snapshot->events.socket_backoff_wait_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_FAULT_INJECTION_TRIGGER:
			snapshot->events.fault_events++;
			break;
		case KFASTBLOCK_VOLUME_EVENT_MANUAL_REFRESH:
		case KFASTBLOCK_VOLUME_EVENT_MANUAL_RESET_BACKOFF:
		case KFASTBLOCK_VOLUME_EVENT_MANUAL_DROP_TRANSPORT:
		case KFASTBLOCK_VOLUME_EVENT_MANUAL_RESET_LEADERS:
		case KFASTBLOCK_VOLUME_EVENT_MANUAL_QUEUE_PAUSE:
		case KFASTBLOCK_VOLUME_EVENT_MANUAL_QUEUE_RESUME:
		case KFASTBLOCK_VOLUME_EVENT_MANUAL_FAULT_INJECTION_ARM:
		case KFASTBLOCK_VOLUME_EVENT_MANUAL_FAULT_INJECTION_RESET:
			snapshot->events.manual_events++;
			break;
		default:
			break;
		}
	}
	spin_unlock_irqrestore(&vol->event_log.lock, flags);

	strscpy(snapshot->events.last_type_text,
		kfastblock_diag_event_type_name(snapshot->events.last_type),
		sizeof(snapshot->events.last_type_text));
}

static void kfastblock_diag_compute_anomaly(struct kfastblock_diag_snapshot *snapshot)
{
	u32 score = 0;
	u32 flags = 0;
	u32 refresh_fail_events;
	u32 socket_events;

	if (!snapshot)
		return;

	refresh_fail_events = snapshot->events.cluster_refresh_fail_events +
		snapshot->events.image_refresh_fail_events +
		snapshot->events.leader_query_fail_events;
	socket_events = snapshot->events.socket_drop_events +
		snapshot->events.socket_backoff_events +
		snapshot->events.socket_backoff_wait_events;

	if (snapshot->volume.queue_paused && !snapshot->volume.manual_queue_pause) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_QUEUE_PAUSED;
		score += 15;
	}
	if (snapshot->volume.health_state == KFASTBLOCK_VOLUME_HEALTH_DEGRADED) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_HEALTH_DEGRADED;
		score += 15;
	} else if (snapshot->volume.health_state == KFASTBLOCK_VOLUME_HEALTH_STALE) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_HEALTH_DEGRADED |
			 KFASTBLOCK_DIAG_ANOMALY_META_STALE;
		score += 30;
	}
	if (snapshot->events.metadata_stale_events) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_META_STALE;
		score += min_t(u32, 20, snapshot->events.metadata_stale_events * 5);
	}
	if (snapshot->buffer.cache_limit &&
	    snapshot->buffer.cached >= snapshot->buffer.cache_limit) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_BUFFER_PRESSURE;
		score += 8;
	}
	if (snapshot->scheduler.current_window < snapshot->scheduler.base_window) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_SCHEDULER_SHRUNK;
		score += 6;
	}
	if (snapshot->osd_conn.backoff_slots || snapshot->osd_conn.failure_count) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_OSD_CONN_UNSTABLE;
		score += min_t(u32, 12,
			       snapshot->osd_conn.backoff_slots * 4 +
			       (u32)min_t(u64, snapshot->osd_conn.failure_count, 4));
	}
	if (snapshot->monitor_conn.backoff_slots ||
	    snapshot->monitor_conn.failure_count) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_MONITOR_CONN_UNSTABLE;
		score += min_t(u32, 12,
			       snapshot->monitor_conn.backoff_slots * 4 +
			       (u32)min_t(u64, snapshot->monitor_conn.failure_count, 4));
	}
	if (snapshot->selfcheck.failure_runs ||
	    snapshot->selfcheck.last_failed_checks) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_SELFCHECK_FAILING;
		score += 18;
	}
	if (snapshot->selfcheck.warning_runs ||
	    snapshot->selfcheck.last_warning_checks) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_SELFCHECK_WARNING;
		score += 6;
	}
	if (snapshot->fault.enabled) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_FAULT_ARMED;
		score += 8;
	}
	if (snapshot->pipeline.retry_objects ||
	    snapshot->pipeline.failed_objects ||
	    snapshot->pipeline.cancelled_objects ||
	    snapshot->pipeline.last_response_status) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_PIPELINE_UNSTABLE;
		score += min_t(u32, 15,
			       (u32)min_t(u64, snapshot->pipeline.retry_objects, 4) * 2 +
			       (u32)min_t(u64, snapshot->pipeline.failed_objects, 4) * 3 +
			       (u32)min_t(u64, snapshot->pipeline.cancelled_objects, 2) * 2 +
			       (snapshot->pipeline.last_response_status ? 3 : 0));
	}
	if (snapshot->events.object_error_events || refresh_fail_events ||
	    socket_events) {
		flags |= KFASTBLOCK_DIAG_ANOMALY_EVENT_ERROR_SPIKE;
		score += min_t(u32, 20,
			       snapshot->events.object_error_events * 4 +
			       refresh_fail_events * 3 +
			       socket_events * 2);
	}

	snapshot->anomaly_flags = flags;
	snapshot->anomaly_score = min_t(u32, score, 100);
}

void kfastblock_diag_baseline_init(
	struct kfastblock_diag_baseline_state *state)
{
	if (!state)
		return;

	memset(state, 0, sizeof(*state));
	mutex_init(&state->lock);
}

void kfastblock_diag_baseline_capture(
	struct kfastblock_diag_baseline_state *state,
	const struct kfastblock_diag_snapshot *snapshot)
{
	if (!state || !snapshot)
		return;

	mutex_lock(&state->lock);
	state->snapshot = *snapshot;
	state->valid = true;
	state->capture_count++;
	state->last_capture_jiffies = jiffies;
	mutex_unlock(&state->lock);
}

void kfastblock_diag_baseline_reset(
	struct kfastblock_diag_baseline_state *state)
{
	if (!state)
		return;

	mutex_lock(&state->lock);
	state->valid = false;
	memset(&state->snapshot, 0, sizeof(state->snapshot));
	state->last_drift_score = 0;
	state->last_drift_flags = 0;
	state->last_compare_jiffies = 0;
	state->reset_count++;
	state->last_reset_jiffies = jiffies;
	mutex_unlock(&state->lock);
}

static u32 kfastblock_diag_compare_conn_pool(
	const struct kfastblock_conn_pool_snapshot *base,
	const struct kfastblock_conn_pool_snapshot *current_snapshot)
{
	u32 score = 0;

	if (!base || !current_snapshot)
		return 0;

	if (current_snapshot->backoff_slots > base->backoff_slots)
		score += (current_snapshot->backoff_slots - base->backoff_slots) * 4;
	if (current_snapshot->failure_count > base->failure_count)
		score += min_t(u32, 10,
			       (u32)(current_snapshot->failure_count -
				     base->failure_count));
	if (current_snapshot->avg_health_score + 5 < base->avg_health_score)
		score += 6;
	return score;
}

void kfastblock_diag_compare_baseline(
	struct kfastblock_diag_baseline_state *state,
	const struct kfastblock_diag_snapshot *current_snapshot,
	u32 *score_out,
	u32 *flags_out,
	bool *valid_out)
{
	struct kfastblock_diag_snapshot base = {};
	u32 score = 0;
	u32 flags = 0;
	bool valid = false;

	if (score_out)
		*score_out = 0;
	if (flags_out)
		*flags_out = 0;
	if (valid_out)
		*valid_out = false;
	if (!state || !current_snapshot)
		return;

	mutex_lock(&state->lock);
	if (state->valid) {
		base = state->snapshot;
		valid = true;
	}
	mutex_unlock(&state->lock);

	if (!valid) {
		if (valid_out)
			*valid_out = false;
		return;
	}

	if (base.volume.health_state != current_snapshot->volume.health_state ||
	    base.volume.last_failure_source !=
		    current_snapshot->volume.last_failure_source) {
		flags |= KFASTBLOCK_DIAG_DRIFT_HEALTH;
		score += 18;
	}
	if (base.volume.sync_state != current_snapshot->volume.sync_state ||
	    base.volume.image_epoch != current_snapshot->volume.image_epoch ||
	    base.volume.osdmap_epoch != current_snapshot->volume.osdmap_epoch ||
	    base.volume.pgmap_epoch != current_snapshot->volume.pgmap_epoch) {
		flags |= KFASTBLOCK_DIAG_DRIFT_META;
		score += 16;
	}
	if (base.scheduler.policy != current_snapshot->scheduler.policy ||
	    base.scheduler.current_window !=
		    current_snapshot->scheduler.current_window ||
	    base.scheduler.base_window != current_snapshot->scheduler.base_window) {
		flags |= KFASTBLOCK_DIAG_DRIFT_SCHEDULER;
		score += 10;
	}
	if (current_snapshot->buffer.evictions > base.buffer.evictions ||
	    current_snapshot->buffer.direct_allocs > base.buffer.direct_allocs ||
	    current_snapshot->buffer.cached > current_snapshot->buffer.cache_limit) {
		flags |= KFASTBLOCK_DIAG_DRIFT_BUFFER;
		score += 8;
	}
	if (kfastblock_diag_compare_conn_pool(&base.osd_conn,
					      &current_snapshot->osd_conn)) {
		flags |= KFASTBLOCK_DIAG_DRIFT_OSD_CONN;
		score += kfastblock_diag_compare_conn_pool(&base.osd_conn,
							      &current_snapshot->osd_conn);
	}
	if (kfastblock_diag_compare_conn_pool(&base.monitor_conn,
					      &current_snapshot->monitor_conn)) {
		flags |= KFASTBLOCK_DIAG_DRIFT_MONITOR_CONN;
		score += kfastblock_diag_compare_conn_pool(&base.monitor_conn,
							      &current_snapshot->monitor_conn);
	}
	if (current_snapshot->selfcheck.failure_runs >
		    base.selfcheck.failure_runs ||
	    current_snapshot->selfcheck.last_failed_checks >
		    base.selfcheck.last_failed_checks ||
	    current_snapshot->selfcheck.last_warning_checks >
		    base.selfcheck.last_warning_checks) {
		flags |= KFASTBLOCK_DIAG_DRIFT_SELFCHECK;
		score += 15;
	}
	if (base.fault.enabled != current_snapshot->fault.enabled ||
	    base.fault.mask != current_snapshot->fault.mask ||
	    base.fault.budget != current_snapshot->fault.budget ||
	    base.fault.errno_value != current_snapshot->fault.errno_value) {
		flags |= KFASTBLOCK_DIAG_DRIFT_FAULT;
		score += 12;
	}
	if (current_snapshot->pipeline.dispatch_batches !=
		    base.pipeline.dispatch_batches ||
	    current_snapshot->pipeline.retry_objects >
		    base.pipeline.retry_objects ||
	    current_snapshot->pipeline.failed_objects >
		    base.pipeline.failed_objects ||
	    current_snapshot->pipeline.cancelled_objects >
		    base.pipeline.cancelled_objects ||
	    current_snapshot->pipeline.last_response_status !=
		    base.pipeline.last_response_status) {
		flags |= KFASTBLOCK_DIAG_DRIFT_PIPELINE;
		score += 10;
	}
	if (current_snapshot->events.object_error_events >
		    base.events.object_error_events ||
	    current_snapshot->events.cluster_refresh_fail_events >
		    base.events.cluster_refresh_fail_events ||
	    current_snapshot->events.image_refresh_fail_events >
		    base.events.image_refresh_fail_events ||
	    current_snapshot->events.leader_query_fail_events >
		    base.events.leader_query_fail_events ||
	    current_snapshot->events.socket_drop_events >
		    base.events.socket_drop_events ||
	    current_snapshot->events.fault_events > base.events.fault_events) {
		flags |= KFASTBLOCK_DIAG_DRIFT_EVENTS;
		score += 12;
	}

	score = min_t(u32, score, 100);
	mutex_lock(&state->lock);
	state->last_drift_score = score;
	state->last_drift_flags = flags;
	state->last_compare_jiffies = jiffies;
	mutex_unlock(&state->lock);

	if (score_out)
		*score_out = score;
	if (flags_out)
		*flags_out = flags;
	if (valid_out)
		*valid_out = true;
}

static int kfastblock_diag_dump_snapshot_prefixed(
	struct seq_file *m,
	const char *prefix,
	const struct kfastblock_diag_snapshot *snapshot)
{
	if (!m || !prefix || !snapshot)
		return -EINVAL;

	seq_printf(m, "%svolume.disk=%s\n", prefix, snapshot->volume.disk_name);
	seq_printf(m, "%svolume.pool=%s\n", prefix, snapshot->volume.pool_name);
	seq_printf(m, "%svolume.image=%s\n", prefix, snapshot->volume.image_name);
	seq_printf(m, "%svolume.ready=%d\n", prefix, snapshot->volume.ready);
	seq_printf(m, "%svolume.queue_paused=%d\n", prefix,
		   snapshot->volume.queue_paused);
	seq_printf(m, "%svolume.manual_queue_pause=%d\n", prefix,
		   snapshot->volume.manual_queue_pause);
	seq_printf(m, "%svolume.open_count=%d\n", prefix,
		   snapshot->volume.open_count);
	seq_printf(m, "%svolume.inflight_ios=%d\n", prefix,
		   snapshot->volume.inflight_ios);
	seq_printf(m, "%svolume.health_state=%s\n", prefix,
		   kfastblock_diag_health_state_name(
			   snapshot->volume.health_state));
	seq_printf(m, "%svolume.last_failure_source=%s\n", prefix,
		   kfastblock_diag_failure_source_name(
			   snapshot->volume.last_failure_source));
	seq_printf(m, "%svolume.last_failure_errno=%d\n", prefix,
		   snapshot->volume.last_failure_errno);
	seq_printf(m, "%svolume.health_since_jiffies=%lu\n", prefix,
		   snapshot->volume.health_since_jiffies);
	seq_printf(m, "%svolume.last_failure_jiffies=%lu\n", prefix,
		   snapshot->volume.last_failure_jiffies);
	seq_printf(m, "%svolume.last_success_jiffies=%lu\n", prefix,
		   snapshot->volume.last_success_jiffies);
	seq_printf(m, "%svolume.size_bytes=%llu\n", prefix,
		   snapshot->volume.size_bytes);
	seq_printf(m, "%svolume.block_size=%u\n", prefix,
		   snapshot->volume.block_size);
	seq_printf(m, "%svolume.object_size=%u\n", prefix,
		   snapshot->volume.object_size);
	seq_printf(m, "%svolume.pool_id=%u\n", prefix,
		   snapshot->volume.pool_id);
	seq_printf(m, "%svolume.pg_count=%u\n", prefix,
		   snapshot->volume.pg_count);
	seq_printf(m, "%svolume.read_only=%u\n", prefix,
		   snapshot->volume.read_only);
	seq_printf(m, "%svolume.sync_state=%u\n", prefix,
		   snapshot->volume.sync_state);
	seq_printf(m, "%svolume.image_epoch=%llu\n", prefix,
		   snapshot->volume.image_epoch);
	seq_printf(m, "%svolume.osdmap_epoch=%llu\n", prefix,
		   snapshot->volume.osdmap_epoch);
	seq_printf(m, "%svolume.pgmap_epoch=%llu\n", prefix,
		   snapshot->volume.pgmap_epoch);
	seq_printf(m, "%svolume.leader_epoch=%llu\n", prefix,
		   snapshot->volume.leader_epoch);
	seq_printf(m, "%svolume.osd_count=%u\n", prefix,
		   snapshot->volume.osd_count);
	seq_printf(m, "%svolume.route_count=%u\n", prefix,
		   snapshot->volume.route_count);
	seq_printf(m, "%svolume.dispatch_window=%u\n", prefix,
		   snapshot->volume.dispatch_window);
	seq_printf(m, "%svolume.refresh_interval_ms=%u\n", prefix,
		   snapshot->volume.refresh_interval_ms);
	seq_printf(m, "%svolume.image_refresh_interval_ms=%u\n", prefix,
		   snapshot->volume.image_refresh_interval_ms);
	seq_printf(m, "%svolume.last_refresh_jiffies=%lu\n", prefix,
		   snapshot->volume.last_refresh_jiffies);
	seq_printf(m, "%svolume.last_image_refresh_jiffies=%lu\n", prefix,
		   snapshot->volume.last_image_refresh_jiffies);
	seq_printf(m, "%svolume.event_count=%u\n", prefix,
		   snapshot->volume.event_count);
	seq_printf(m, "%spipeline.request_prepares=%llu\n", prefix,
		   snapshot->pipeline.request_prepares);
	seq_printf(m, "%spipeline.capacity=%u\n", prefix,
		   snapshot->pipeline.capacity);
	seq_printf(m, "%spipeline.inflight=%u\n", prefix,
		   snapshot->pipeline.inflight);
	seq_printf(m, "%spipeline.peak_inflight=%u\n", prefix,
		   snapshot->pipeline.peak_inflight);
	seq_printf(m, "%spipeline.free_entries=%u\n", prefix,
		   snapshot->pipeline.free_entries);
	seq_printf(m, "%spipeline.utilization_pct=%u\n", prefix,
		   snapshot->pipeline.utilization_pct);
	seq_printf(m, "%spipeline.oldest_inflight_seq=%llu\n", prefix,
		   snapshot->pipeline.oldest_inflight_seq);
	seq_printf(m, "%spipeline.newest_inflight_seq=%llu\n", prefix,
		   snapshot->pipeline.newest_inflight_seq);
	seq_printf(m, "%spipeline.oldest_queued_jiffies=%lu\n", prefix,
		   snapshot->pipeline.oldest_queued_jiffies);
	seq_printf(m, "%spipeline.newest_queued_jiffies=%lu\n", prefix,
		   snapshot->pipeline.newest_queued_jiffies);
	seq_printf(m, "%spipeline.request_cleanups=%llu\n", prefix,
		   snapshot->pipeline.request_cleanups);
	seq_printf(m, "%spipeline.dispatch_batches=%llu\n", prefix,
		   snapshot->pipeline.dispatch_batches);
	seq_printf(m, "%spipeline.queued_objects=%llu\n", prefix,
		   snapshot->pipeline.queued_objects);
	seq_printf(m, "%spipeline.retry_objects=%llu\n", prefix,
		   snapshot->pipeline.retry_objects);
	seq_printf(m, "%spipeline.completed_objects=%llu\n", prefix,
		   snapshot->pipeline.completed_objects);
	seq_printf(m, "%spipeline.failed_objects=%llu\n", prefix,
		   snapshot->pipeline.failed_objects);
	seq_printf(m, "%spipeline.cancelled_objects=%llu\n", prefix,
		   snapshot->pipeline.cancelled_objects);
	seq_printf(m, "%spipeline.seq_records=%llu\n", prefix,
		   snapshot->pipeline.seq_records);
	seq_printf(m, "%spipeline.last_response_status=%d\n", prefix,
		   snapshot->pipeline.last_response_status);
	seq_printf(m, "%spipeline.last_response_body_len=%u\n", prefix,
		   snapshot->pipeline.last_response_body_len);
	seq_printf(m, "%spipeline.last_transport_flags=0x%x\n", prefix,
		   snapshot->pipeline.last_transport_flags);
	seq_printf(m, "%sdiagnostics.anomaly_score=%u\n", prefix,
		   snapshot->anomaly_score);
	seq_printf(m, "%sdiagnostics.anomaly_status=%s\n", prefix,
		   kfastblock_diag_anomaly_status(snapshot->anomaly_score));
	seq_printf(m, "%sdiagnostics.anomaly_flags=0x%x\n", prefix,
		   snapshot->anomaly_flags);
	return 0;
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
	kfastblock_diag_collect_pipeline(vol, snapshot);
	kfastblock_osd_conn_pool_snapshot(vol->socket_cache,
					  KFASTBLOCK_MAX_SOCKET_CACHE,
					  &snapshot->osd_conn);
	kfastblock_monitor_conn_pool_snapshot(vol->monitor_cache,
					      KFASTBLOCK_MAX_MONITORS,
					      &snapshot->monitor_conn);
	kfastblock_diag_collect_selfcheck(vol, snapshot);
	kfastblock_diag_collect_fault(vol, snapshot);
	kfastblock_diag_collect_events(vol, snapshot);
	kfastblock_diag_compute_anomaly(snapshot);
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

	seq_printf(m, "pipeline.request_prepares=%llu\n",
		   snapshot->pipeline.request_prepares);
	seq_printf(m, "pipeline.capacity=%u\n",
		   snapshot->pipeline.capacity);
	seq_printf(m, "pipeline.inflight=%u\n",
		   snapshot->pipeline.inflight);
	seq_printf(m, "pipeline.peak_inflight=%u\n",
		   snapshot->pipeline.peak_inflight);
	seq_printf(m, "pipeline.free_entries=%u\n",
		   snapshot->pipeline.free_entries);
	seq_printf(m, "pipeline.utilization_pct=%u\n",
		   snapshot->pipeline.utilization_pct);
	seq_printf(m, "pipeline.oldest_inflight_seq=%llu\n",
		   snapshot->pipeline.oldest_inflight_seq);
	seq_printf(m, "pipeline.newest_inflight_seq=%llu\n",
		   snapshot->pipeline.newest_inflight_seq);
	seq_printf(m, "pipeline.oldest_queued_jiffies=%lu\n",
		   snapshot->pipeline.oldest_queued_jiffies);
	seq_printf(m, "pipeline.newest_queued_jiffies=%lu\n",
		   snapshot->pipeline.newest_queued_jiffies);
	seq_printf(m, "pipeline.request_cleanups=%llu\n",
		   snapshot->pipeline.request_cleanups);
	seq_printf(m, "pipeline.dispatch_batches=%llu\n",
		   snapshot->pipeline.dispatch_batches);
	seq_printf(m, "pipeline.queued_objects=%llu\n",
		   snapshot->pipeline.queued_objects);
	seq_printf(m, "pipeline.retry_objects=%llu\n",
		   snapshot->pipeline.retry_objects);
	seq_printf(m, "pipeline.completed_objects=%llu\n",
		   snapshot->pipeline.completed_objects);
	seq_printf(m, "pipeline.failed_objects=%llu\n",
		   snapshot->pipeline.failed_objects);
	seq_printf(m, "pipeline.cancelled_objects=%llu\n",
		   snapshot->pipeline.cancelled_objects);
	seq_printf(m, "pipeline.seq_records=%llu\n",
		   snapshot->pipeline.seq_records);
	seq_printf(m, "pipeline.last_response_status=%d\n",
		   snapshot->pipeline.last_response_status);
	seq_printf(m, "pipeline.last_response_body_len=%u\n",
		   snapshot->pipeline.last_response_body_len);
	seq_printf(m, "pipeline.last_transport_flags=0x%x\n",
		   snapshot->pipeline.last_transport_flags);

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
	seq_printf(m, "events.total=%u\n", snapshot->events.total_events);
	seq_printf(m, "events.health_change=%u\n",
		   snapshot->events.health_change_events);
	seq_printf(m, "events.metadata_stale=%u\n",
		   snapshot->events.metadata_stale_events);
	seq_printf(m, "events.cluster_refresh_fail=%u\n",
		   snapshot->events.cluster_refresh_fail_events);
	seq_printf(m, "events.image_refresh_fail=%u\n",
		   snapshot->events.image_refresh_fail_events);
	seq_printf(m, "events.leader_query_fail=%u\n",
		   snapshot->events.leader_query_fail_events);
	seq_printf(m, "events.object_retry=%u\n",
		   snapshot->events.object_retry_events);
	seq_printf(m, "events.object_error=%u\n",
		   snapshot->events.object_error_events);
	seq_printf(m, "events.socket_drop=%u\n",
		   snapshot->events.socket_drop_events);
	seq_printf(m, "events.socket_backoff=%u\n",
		   snapshot->events.socket_backoff_events);
	seq_printf(m, "events.socket_backoff_wait=%u\n",
		   snapshot->events.socket_backoff_wait_events);
	seq_printf(m, "events.fault=%u\n", snapshot->events.fault_events);
	seq_printf(m, "events.manual=%u\n", snapshot->events.manual_events);
	seq_printf(m, "events.last_type=%s\n",
		   snapshot->events.last_type_text);
	seq_printf(m, "events.last_errno=%d\n",
		   snapshot->events.last_errno);
	seq_printf(m, "events.oldest_jiffies=%lu\n",
		   snapshot->events.oldest_jiffies);
	seq_printf(m, "events.newest_jiffies=%lu\n",
		   snapshot->events.newest_jiffies);
	seq_printf(m, "diagnostics.anomaly_score=%u\n",
		   snapshot->anomaly_score);
	seq_printf(m, "diagnostics.anomaly_status=%s\n",
		   kfastblock_diag_anomaly_status(snapshot->anomaly_score));
	seq_printf(m, "diagnostics.anomaly_flags=0x%x\n",
		   snapshot->anomaly_flags);
	return 0;
}

#define KFASTBLOCK_DIAG_BASELINE_GETTER(name, field, type) \
type name(struct kfastblock_diag_baseline_state *state) \
{ \
	type value = 0; \
	if (!state) \
		return 0; \
	mutex_lock(&state->lock); \
	value = state->field; \
	mutex_unlock(&state->lock); \
	return value; \
}

bool kfastblock_diag_baseline_valid(
	struct kfastblock_diag_baseline_state *state)
{
	bool valid = false;

	if (!state)
		return false;

	mutex_lock(&state->lock);
	valid = state->valid;
	mutex_unlock(&state->lock);
	return valid;
}

KFASTBLOCK_DIAG_BASELINE_GETTER(kfastblock_diag_baseline_capture_count,
				capture_count, u32)
KFASTBLOCK_DIAG_BASELINE_GETTER(kfastblock_diag_baseline_reset_count,
				reset_count, u32)
KFASTBLOCK_DIAG_BASELINE_GETTER(kfastblock_diag_baseline_last_capture_jiffies,
				last_capture_jiffies, unsigned long)
KFASTBLOCK_DIAG_BASELINE_GETTER(kfastblock_diag_baseline_last_reset_jiffies,
				last_reset_jiffies, unsigned long)
KFASTBLOCK_DIAG_BASELINE_GETTER(kfastblock_diag_baseline_last_compare_jiffies,
				last_compare_jiffies, unsigned long)
KFASTBLOCK_DIAG_BASELINE_GETTER(kfastblock_diag_baseline_last_drift_score,
				last_drift_score, u32)
KFASTBLOCK_DIAG_BASELINE_GETTER(kfastblock_diag_baseline_last_drift_flags,
				last_drift_flags, u32)

int kfastblock_diag_dump_baseline_seq(
	struct seq_file *m,
	struct kfastblock_diag_baseline_state *state,
	const struct kfastblock_diag_snapshot *current_snapshot)
{
	struct kfastblock_diag_snapshot baseline = {};
	u32 drift_score = 0;
	u32 drift_flags = 0;
	unsigned long last_capture = 0;
	unsigned long last_reset = 0;
	unsigned long last_compare = 0;
	u32 capture_count = 0;
	u32 reset_count = 0;
	bool valid = false;

	if (!m || !state || !current_snapshot)
		return -EINVAL;

	mutex_lock(&state->lock);
	valid = state->valid;
	if (valid)
		baseline = state->snapshot;
	capture_count = state->capture_count;
	reset_count = state->reset_count;
	last_capture = state->last_capture_jiffies;
	last_reset = state->last_reset_jiffies;
	mutex_unlock(&state->lock);

	kfastblock_diag_compare_baseline(state, current_snapshot,
					 &drift_score, &drift_flags, &valid);
	last_compare = kfastblock_diag_baseline_last_compare_jiffies(state);

	seq_printf(m, "baseline.valid=%u\n", valid ? 1 : 0);
	seq_printf(m, "baseline.capture_count=%u\n", capture_count);
	seq_printf(m, "baseline.reset_count=%u\n", reset_count);
	seq_printf(m, "baseline.last_capture_jiffies=%lu\n", last_capture);
	seq_printf(m, "baseline.last_reset_jiffies=%lu\n", last_reset);
	seq_printf(m, "baseline.last_compare_jiffies=%lu\n", last_compare);
	seq_printf(m, "drift.score=%u\n", drift_score);
	seq_printf(m, "drift.flags=0x%x\n", drift_flags);
	seq_printf(m, "drift.status=%s\n",
		   kfastblock_diag_drift_status(valid, drift_score));
	if (valid)
		kfastblock_diag_dump_snapshot_prefixed(m, "baseline.",
						       &baseline);
	kfastblock_diag_dump_snapshot_prefixed(m, "current.",
					       current_snapshot);
	return 0;
}
