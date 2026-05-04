#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "kfastblock/buffer.h"
#include "kfastblock/connpool.h"
#include "kfastblock/fault.h"
#include "kfastblock/meta.h"
#include "kfastblock/request.h"
#include "kfastblock/scheduler.h"
#include "kfastblock/selfcheck.h"
#include "kfastblock/volume.h"

static void kfastblock_selfcheck_note(struct kfastblock_selfcheck_report *report,
				      struct seq_file *m,
				      const char *name,
				      bool pass,
				      bool warning,
				      u32 flag,
				      int err,
				      const char *detail)
{
	if (!report)
		return;

	report->total_checks++;
	if (!pass) {
		report->failed_checks++;
		report->flags |= flag;
		if (!report->result_errno)
			report->result_errno = err ? err : -EUCLEAN;
	} else if (warning) {
		report->warning_checks++;
		report->flags |= flag;
	}

	if (m) {
		seq_printf(m, "%s: %s", name,
			   pass ? (warning ? "warn" : "ok") : "fail");
		if (!pass && err)
			seq_printf(m, " errno=%d", err);
		if (detail && detail[0])
			seq_printf(m, " detail=%s", detail);
		seq_putc(m, '\n');
	}
}

static void kfastblock_selfcheck_format_u32(char *buf, size_t buf_len,
					    const char *label, u32 value)
{
	if (!buf || !buf_len)
		return;

	scnprintf(buf, buf_len, "%s=%u", label, value);
}

static void kfastblock_selfcheck_check_volume_core(
	struct kfastblock_volume *vol,
	struct kfastblock_selfcheck_report *report,
	struct seq_file *m)
{
	char detail[160];
	int inflight;
	int open_count;
	bool io_ready;

	if (!vol)
		return;

	open_count = atomic_read(&vol->open_count);
	inflight = atomic_read(&vol->inflight_ios);
	io_ready = kfastblock_meta_io_ready(&vol->view);

	kfastblock_selfcheck_note(report, m, "volume.present",
				  vol->disk != NULL, false,
				  KFASTBLOCK_SELFCHECK_VOLUME_CORE, -ENODEV,
				  vol->disk ? "disk=present" : "disk=missing");
	scnprintf(detail, sizeof(detail), "dispatch_window=%u",
		  vol->dispatch_window);
	kfastblock_selfcheck_note(report, m, "volume.dispatch_window",
				  vol->dispatch_window > 0 &&
				  vol->dispatch_window <= KFASTBLOCK_MAX_OBJECT_EXTENTS,
				  false, KFASTBLOCK_SELFCHECK_VOLUME_CORE,
				  -EINVAL, detail);
	kfastblock_selfcheck_format_u32(detail, sizeof(detail), "refresh_interval_ms",
					 vol->refresh_interval_ms);
	kfastblock_selfcheck_note(report, m, "volume.refresh_interval",
				  vol->refresh_interval_ms > 0, false,
				  KFASTBLOCK_SELFCHECK_VOLUME_CORE, -EINVAL,
				  detail);
	kfastblock_selfcheck_format_u32(detail, sizeof(detail),
					 "image_refresh_interval_ms",
					 vol->image_refresh_interval_ms);
	kfastblock_selfcheck_note(report, m, "volume.image_refresh_interval",
				  vol->image_refresh_interval_ms > 0, false,
				  KFASTBLOCK_SELFCHECK_VOLUME_CORE, -EINVAL,
				  detail);
	scnprintf(detail, sizeof(detail), "open_count=%d", open_count);
	kfastblock_selfcheck_note(report, m, "volume.open_count",
				  open_count >= 0, false,
				  KFASTBLOCK_SELFCHECK_VOLUME_CORE, -EINVAL,
				  detail);
	scnprintf(detail, sizeof(detail), "inflight_ios=%d", inflight);
	kfastblock_selfcheck_note(report, m, "volume.inflight_ios",
				  inflight >= 0, false,
				  KFASTBLOCK_SELFCHECK_VOLUME_CORE, -EINVAL,
				  detail);
	scnprintf(detail, sizeof(detail),
		  "queue_paused=%u manual_queue_pause=%u io_ready=%u ready=%d",
		  vol->queue_paused ? 1 : 0,
		  vol->manual_queue_pause ? 1 : 0,
		  io_ready ? 1 : 0, atomic_read(&vol->ready));
	kfastblock_selfcheck_note(report, m, "volume.queue_gate",
				  !vol->queue_paused ||
				  vol->manual_queue_pause ||
				  !io_ready ||
				  !atomic_read(&vol->ready),
				  vol->queue_paused && !vol->manual_queue_pause &&
				  atomic_read(&vol->ready),
				  KFASTBLOCK_SELFCHECK_QUEUE_GATE, -EAGAIN,
				  detail);
}

static void kfastblock_selfcheck_check_meta_view(
	struct kfastblock_volume *vol,
	struct kfastblock_selfcheck_report *report,
	struct seq_file *m)
{
	char detail[192];
	u32 i;

	if (!vol)
		return;

	scnprintf(detail, sizeof(detail),
		  "sync_state=%u size_bytes=%llu object_size=%u pg_count=%u",
		  vol->view.sync_state, vol->view.image.size_bytes,
		  vol->view.image.object_size, vol->view.image.pg_count);
	kfastblock_selfcheck_note(report, m, "meta.image_shape",
				  vol->view.sync_state != KFASTBLOCK_META_SYNC_READY ||
				  (vol->view.image.size_bytes > 0 &&
				   vol->view.image.object_size > 0 &&
				   vol->view.image.pg_count > 0),
				  false, KFASTBLOCK_SELFCHECK_META_VIEW,
				  -EINVAL, detail);
	scnprintf(detail, sizeof(detail), "osd_count=%u route_count=%u",
		  vol->view.osd_count, vol->view.route_count);
	kfastblock_selfcheck_note(report, m, "meta.topology_arrays",
				  (vol->view.osd_count == 0 || vol->view.osds) &&
				  (vol->view.route_count == 0 || vol->view.routes),
				  false, KFASTBLOCK_SELFCHECK_META_VIEW,
				  -EINVAL, detail);

	for (i = 0; i < vol->view.osd_count; ++i) {
		const struct kfastblock_osd_endpoint *osd = &vol->view.osds[i];

		scnprintf(detail, sizeof(detail),
			  "osd[%u] id=%u shard_count=%u addr=%s", i,
			  osd->osd_id, osd->shard_count, osd->address);
		kfastblock_selfcheck_note(report, m, "meta.osd.endpoint",
					  osd->osd_id > 0 &&
					  osd->address[0] != '\0' &&
					  (osd->shard_count == 0 || osd->shards),
					  false, KFASTBLOCK_SELFCHECK_META_VIEW,
					  -EINVAL, detail);
	}

	for (i = 0; i < vol->view.route_count; ++i) {
		const struct kfastblock_pg_route *route = &vol->view.routes[i];

		scnprintf(detail, sizeof(detail),
			  "route[%u] pool=%u pg=%u replicas=%u leader_valid=%u",
			  i, route->pool_id, route->pg_id,
			  route->replica_count, route->leader_valid ? 1 : 0);
		kfastblock_selfcheck_note(report, m, "meta.route.replicas",
					  route->replica_count > 0 &&
					  route->osd_ids != NULL,
					  false, KFASTBLOCK_SELFCHECK_META_VIEW,
					  -EINVAL, detail);
		if (route->leader_valid) {
			scnprintf(detail, sizeof(detail),
				  "route[%u] leader_osd=%u addr=%s port=%u",
				  i, route->leader.osd_id,
				  route->leader.address,
				  route->leader.port);
			kfastblock_selfcheck_note(report, m, "meta.route.leader",
						  route->leader.osd_id > 0 &&
						  route->leader.address[0] != '\0' &&
						  route->leader.port > 0,
						  false,
						  KFASTBLOCK_SELFCHECK_META_VIEW,
						  -EINVAL, detail);
		}
	}
}

static void kfastblock_selfcheck_check_scheduler(
	struct kfastblock_volume *vol,
	struct kfastblock_selfcheck_report *report,
	struct seq_file *m)
{
	struct kfastblock_scheduler_controller *sched;
	char detail[192];

	if (!vol)
		return;

	sched = &vol->scheduler;
	scnprintf(detail, sizeof(detail),
		  "min=%u current=%u base=%u max=%u policy=%u dynamic=%u",
		  sched->min_window, sched->current_window, sched->base_window,
		  sched->max_window, sched->policy, sched->dynamic_enabled ? 1 : 0);
	kfastblock_selfcheck_note(report, m, "scheduler.window_bounds",
				  sched->min_window > 0 &&
				  sched->min_window <= sched->current_window &&
				  sched->current_window <= sched->base_window &&
				  sched->base_window <= sched->max_window,
				  false, KFASTBLOCK_SELFCHECK_SCHEDULER,
				  -EINVAL, detail);
	scnprintf(detail, sizeof(detail),
		  "sample_events=%u limited=%u cooldown_limited=%u",
		  sched->sample_events, sched->pressure_limited_samples,
		  sched->cooldown_limited_samples);
	kfastblock_selfcheck_note(report, m, "scheduler.sample_counters",
				  sched->pressure_limited_samples <= sched->sample_events &&
				  sched->cooldown_limited_samples <= sched->sample_events,
				  false, KFASTBLOCK_SELFCHECK_SCHEDULER,
				  -EINVAL, detail);
	scnprintf(detail, sizeof(detail), "policy=%u", sched->policy);
	kfastblock_selfcheck_note(report, m, "scheduler.policy",
				  sched->policy <= KFASTBLOCK_SCHED_POLICY_PRESSURE,
				  false, KFASTBLOCK_SELFCHECK_SCHEDULER,
				  -EINVAL, detail);
	if (!sched->dynamic_enabled) {
		scnprintf(detail, sizeof(detail),
			  "dynamic=0 current=%u base=%u",
			  sched->current_window, sched->base_window);
		kfastblock_selfcheck_note(report, m, "scheduler.static_mode",
					  sched->current_window == sched->base_window,
					  true, KFASTBLOCK_SELFCHECK_SCHEDULER,
					  -EINVAL, detail);
	}
}

static void kfastblock_selfcheck_check_buffer_pool(
	struct kfastblock_volume *vol,
	struct kfastblock_selfcheck_report *report,
	struct seq_file *m)
{
	struct kfastblock_object_buffer_pool *pool;
	char detail[160];
	bool list_empty_now;

	if (!vol)
		return;

	pool = &vol->object_buffer_pool;
	mutex_lock(&pool->lock);
	list_empty_now = list_empty(&pool->free_list);
	scnprintf(detail, sizeof(detail),
		  "cached=%u limit=%u chunk_bytes=%u list_empty=%u",
		  pool->cached, pool->max_cached, pool->chunk_bytes,
		  list_empty_now ? 1 : 0);
	kfastblock_selfcheck_note(report, m, "buffer_pool.cache_bounds",
				  pool->chunk_bytes > 0 &&
				  pool->cached <= pool->max_cached,
				  false, KFASTBLOCK_SELFCHECK_BUFFER_POOL,
				  -EINVAL, detail);
	kfastblock_selfcheck_note(report, m, "buffer_pool.free_list",
				  pool->cached == 0 ? list_empty_now : !list_empty_now,
				  pool->cached == 0 && !list_empty_now,
				  KFASTBLOCK_SELFCHECK_BUFFER_POOL,
				  -EINVAL, detail);
	mutex_unlock(&pool->lock);
}

static void kfastblock_selfcheck_check_osd_conn_pool(
	struct kfastblock_volume *vol,
	struct kfastblock_selfcheck_report *report,
	struct seq_file *m)
{
	struct kfastblock_conn_pool_snapshot snapshot = {};
	char detail[192];
	u32 i;

	if (!vol)
		return;

	kfastblock_osd_conn_pool_snapshot(vol->socket_cache,
					  KFASTBLOCK_MAX_SOCKET_CACHE,
					  &snapshot);
	scnprintf(detail, sizeof(detail),
		  "total=%u ready=%u backoff=%u connecting=%u empty=%u active=%u",
		  snapshot.total_slots, snapshot.ready_slots,
		  snapshot.backoff_slots, snapshot.connecting_slots,
		  snapshot.empty_slots, snapshot.active_sockets);
	kfastblock_selfcheck_note(report, m, "osd_conn_pool.snapshot_totals",
				  snapshot.total_slots ==
				  snapshot.ready_slots + snapshot.backoff_slots +
				  snapshot.connecting_slots + snapshot.empty_slots,
				  false, KFASTBLOCK_SELFCHECK_OSD_CONN_POOL,
				  -EINVAL, detail);
	for (i = 0; i < KFASTBLOCK_MAX_SOCKET_CACHE; ++i) {
		struct kfastblock_cached_socket *cached = &vol->socket_cache[i];

		mutex_lock(&cached->lock);
		scnprintf(detail, sizeof(detail),
			  "slot=%u state=%s sock=%u connecting=%u fail_streak=%u health=%u next_seq=%llu",
			  i, kfastblock_conn_state_name(cached->state),
			  cached->sock ? 1 : 0, cached->connecting ? 1 : 0,
			  cached->fail_streak, cached->health_score,
			  cached->next_seq);
		kfastblock_selfcheck_note(report, m, "osd_conn_pool.slot_state",
					  (cached->state != KFASTBLOCK_CONN_STATE_READY ||
					   cached->sock != NULL) &&
					  (cached->state != KFASTBLOCK_CONN_STATE_CONNECTING ||
					   (cached->connecting && !cached->sock)) &&
					  cached->health_score <= 100,
					  false, KFASTBLOCK_SELFCHECK_OSD_CONN_POOL,
					  -EINVAL, detail);
		kfastblock_selfcheck_note(report, m, "osd_conn_pool.identity",
					  !cached->sock ||
					  (cached->osd_id > 0 && cached->port > 0 &&
					   cached->address[0] != '\0'),
					  false, KFASTBLOCK_SELFCHECK_OSD_CONN_POOL,
					  -EINVAL, detail);
		mutex_unlock(&cached->lock);
	}
}

static void kfastblock_selfcheck_check_monitor_conn_pool(
	struct kfastblock_volume *vol,
	struct kfastblock_selfcheck_report *report,
	struct seq_file *m)
{
	struct kfastblock_conn_pool_snapshot snapshot = {};
	char detail[192];
	u32 i;

	if (!vol)
		return;

	kfastblock_monitor_conn_pool_snapshot(vol->monitor_cache,
					      KFASTBLOCK_MAX_MONITORS,
					      &snapshot);
	scnprintf(detail, sizeof(detail),
		  "total=%u ready=%u backoff=%u connecting=%u empty=%u active=%u",
		  snapshot.total_slots, snapshot.ready_slots,
		  snapshot.backoff_slots, snapshot.connecting_slots,
		  snapshot.empty_slots, snapshot.active_sockets);
	kfastblock_selfcheck_note(report, m, "monitor_conn_pool.snapshot_totals",
				  snapshot.total_slots ==
				  snapshot.ready_slots + snapshot.backoff_slots +
				  snapshot.connecting_slots + snapshot.empty_slots,
				  false, KFASTBLOCK_SELFCHECK_MON_CONN_POOL,
				  -EINVAL, detail);
	for (i = 0; i < KFASTBLOCK_MAX_MONITORS; ++i) {
		struct kfastblock_cached_monitor_socket *cached =
			&vol->monitor_cache[i];

		mutex_lock(&cached->lock);
		scnprintf(detail, sizeof(detail),
			  "slot=%u state=%s sock=%u fail_streak=%u health=%u next_seq=%llu",
			  i, kfastblock_conn_state_name(cached->state),
			  cached->sock ? 1 : 0, cached->fail_streak,
			  cached->health_score, cached->next_seq);
		kfastblock_selfcheck_note(report, m, "monitor_conn_pool.slot_state",
					  (cached->state != KFASTBLOCK_CONN_STATE_READY ||
					   cached->sock != NULL) &&
					  cached->health_score <= 100,
					  false, KFASTBLOCK_SELFCHECK_MON_CONN_POOL,
					  -EINVAL, detail);
		kfastblock_selfcheck_note(report, m, "monitor_conn_pool.identity",
					  !cached->sock ||
					  (cached->port > 0 && cached->address[0] != '\0'),
					  false, KFASTBLOCK_SELFCHECK_MON_CONN_POOL,
					  -EINVAL, detail);
		mutex_unlock(&cached->lock);
	}
}

static void kfastblock_selfcheck_check_fault_injection(
	struct kfastblock_volume *vol,
	struct kfastblock_selfcheck_report *report,
	struct seq_file *m)
{
	char detail[192];
	char mask_buf[128];
	u32 mask;
	u32 budget;
	s32 err;
	bool enabled;

	if (!vol)
		return;

	mask = kfastblock_fault_injection_mask(&vol->fault_injection);
	budget = kfastblock_fault_injection_budget(&vol->fault_injection);
	err = kfastblock_fault_injection_errno(&vol->fault_injection);
	enabled = kfastblock_fault_injection_enabled(&vol->fault_injection);
	kfastblock_fault_format_mask(mask, mask_buf, sizeof(mask_buf));
	scnprintf(detail, sizeof(detail),
		  "enabled=%u mask=%s errno=%d budget=%u hits=%u skips=%u",
		  enabled ? 1 : 0, mask_buf, err, budget,
		  kfastblock_fault_injection_hit_count(&vol->fault_injection),
		  kfastblock_fault_injection_skip_count(&vol->fault_injection));
	kfastblock_selfcheck_note(report, m, "fault_injection.config",
				  !enabled || (mask != 0 && err < 0 && budget > 0),
				  enabled,
				  KFASTBLOCK_SELFCHECK_FAULT_INJECTION,
				  -EINVAL, detail);
}

static void kfastblock_selfcheck_commit(struct kfastblock_volume *vol,
					const struct kfastblock_selfcheck_report *report)
{
	unsigned long flags;

	if (!vol || !report)
		return;

	spin_lock_irqsave(&vol->selfcheck.lock, flags);
	vol->selfcheck.last_run_jiffies = jiffies;
	vol->selfcheck.run_count++;
	if (report->failed_checks)
		vol->selfcheck.failure_runs++;
	if (report->warning_checks)
		vol->selfcheck.warning_runs++;
	vol->selfcheck.last_errno = report->result_errno;
	vol->selfcheck.last_failed_checks = report->failed_checks;
	vol->selfcheck.last_warning_checks = report->warning_checks;
	vol->selfcheck.last_flags = report->flags;
	spin_unlock_irqrestore(&vol->selfcheck.lock, flags);
}

void kfastblock_selfcheck_state_init(
	struct kfastblock_selfcheck_state *state)
{
	if (!state)
		return;

	memset(state, 0, sizeof(*state));
	spin_lock_init(&state->lock);
}

int kfastblock_selfcheck_run(struct kfastblock_volume *vol,
			     struct kfastblock_selfcheck_report *report,
			     struct seq_file *m)
{
	struct kfastblock_selfcheck_report local = {};

	if (!vol)
		return -EINVAL;

	local.result_errno = 0;
	if (m)
		seq_printf(m, "volume=%s\n",
			   vol->disk ? vol->disk->disk_name : "<none>");
	kfastblock_selfcheck_check_volume_core(vol, &local, m);
	kfastblock_selfcheck_check_meta_view(vol, &local, m);
	kfastblock_selfcheck_check_scheduler(vol, &local, m);
	kfastblock_selfcheck_check_buffer_pool(vol, &local, m);
	kfastblock_selfcheck_check_osd_conn_pool(vol, &local, m);
	kfastblock_selfcheck_check_monitor_conn_pool(vol, &local, m);
	kfastblock_selfcheck_check_fault_injection(vol, &local, m);
	if (m) {
		seq_printf(m,
			   "summary total=%u failed=%u warnings=%u flags=0x%x result_errno=%d\n",
			   local.total_checks, local.failed_checks,
			   local.warning_checks, local.flags,
			   local.result_errno);
	}
	kfastblock_selfcheck_commit(vol, &local);
	if (report)
		*report = local;
	return 0;
}

#define KFASTBLOCK_SELFCHECK_GETTER(name, field, type) \
type name(struct kfastblock_selfcheck_state *state) \
{ \
	unsigned long flags; \
	type value = 0; \
	if (!state) \
		return 0; \
	spin_lock_irqsave(&state->lock, flags); \
	value = state->field; \
	spin_unlock_irqrestore(&state->lock, flags); \
	return value; \
}

KFASTBLOCK_SELFCHECK_GETTER(kfastblock_selfcheck_run_count, run_count, u32)
KFASTBLOCK_SELFCHECK_GETTER(kfastblock_selfcheck_failure_runs, failure_runs, u32)
KFASTBLOCK_SELFCHECK_GETTER(kfastblock_selfcheck_warning_runs, warning_runs, u32)
KFASTBLOCK_SELFCHECK_GETTER(kfastblock_selfcheck_last_errno, last_errno, s32)
KFASTBLOCK_SELFCHECK_GETTER(kfastblock_selfcheck_last_failed_checks,
			    last_failed_checks, u32)
KFASTBLOCK_SELFCHECK_GETTER(kfastblock_selfcheck_last_warning_checks,
			    last_warning_checks, u32)
KFASTBLOCK_SELFCHECK_GETTER(kfastblock_selfcheck_last_flags, last_flags, u32)
KFASTBLOCK_SELFCHECK_GETTER(kfastblock_selfcheck_last_run_jiffies,
			    last_run_jiffies, unsigned long)
