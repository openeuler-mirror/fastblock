#ifndef KFASTBLOCK_DIAG_H
#define KFASTBLOCK_DIAG_H

#include <linux/blkdev.h>
#include <linux/types.h>

#include "kfastblock/connpool.h"

struct kfastblock_volume;
struct seq_file;

enum kfastblock_diag_anomaly_flag {
	KFASTBLOCK_DIAG_ANOMALY_QUEUE_PAUSED = 1U << 0,
	KFASTBLOCK_DIAG_ANOMALY_HEALTH_DEGRADED = 1U << 1,
	KFASTBLOCK_DIAG_ANOMALY_META_STALE = 1U << 2,
	KFASTBLOCK_DIAG_ANOMALY_BUFFER_PRESSURE = 1U << 3,
	KFASTBLOCK_DIAG_ANOMALY_SCHEDULER_SHRUNK = 1U << 4,
	KFASTBLOCK_DIAG_ANOMALY_OSD_CONN_UNSTABLE = 1U << 5,
	KFASTBLOCK_DIAG_ANOMALY_MONITOR_CONN_UNSTABLE = 1U << 6,
	KFASTBLOCK_DIAG_ANOMALY_SELFCHECK_FAILING = 1U << 7,
	KFASTBLOCK_DIAG_ANOMALY_SELFCHECK_WARNING = 1U << 8,
	KFASTBLOCK_DIAG_ANOMALY_FAULT_ARMED = 1U << 9,
	KFASTBLOCK_DIAG_ANOMALY_EVENT_ERROR_SPIKE = 1U << 10,
};

struct kfastblock_diag_volume_snapshot {
	char disk_name[DISK_NAME_LEN];
	char pool_name[KFASTBLOCK_MAX_NAME_LEN];
	char image_name[KFASTBLOCK_MAX_NAME_LEN];
	int ready;
	int queue_paused;
	int manual_queue_pause;
	int open_count;
	int inflight_ios;
	u32 health_state;
	u32 last_failure_source;
	s32 last_failure_errno;
	unsigned long health_since_jiffies;
	unsigned long last_failure_jiffies;
	unsigned long last_success_jiffies;
	u64 size_bytes;
	u32 block_size;
	u32 object_size;
	u32 pool_id;
	u32 pg_count;
	u32 read_only;
	u32 sync_state;
	u64 image_epoch;
	u64 osdmap_epoch;
	u64 pgmap_epoch;
	u64 leader_epoch;
	u32 osd_count;
	u32 route_count;
	u32 dispatch_window;
	u32 refresh_interval_ms;
	u32 image_refresh_interval_ms;
	unsigned long last_refresh_jiffies;
	unsigned long last_image_refresh_jiffies;
	u32 event_count;
};

struct kfastblock_diag_buffer_snapshot {
	u32 cached;
	u32 cache_limit;
	u32 chunk_bytes;
	u64 hits;
	u64 misses;
	u64 returns;
	u64 evictions;
	u64 direct_allocs;
};

struct kfastblock_diag_scheduler_snapshot {
	u32 current_window;
	u32 base_window;
	u32 min_window;
	u32 max_window;
	u32 pressure_inflight_limit;
	u32 cooldown_ms;
	u32 cooldown_remaining_ms;
	u32 grow_events;
	u32 shrink_events;
	u32 retry_events;
	u32 dispatch_failures;
	u32 sample_events;
	u32 pressure_limited_samples;
	u32 cooldown_limited_samples;
	u32 cooldown_events;
	u32 last_sample_inflight_ios;
	u32 last_sample_request_objects;
	u32 last_sample_controller_window;
	u32 last_sample_pressure_window;
	u32 last_sample_effective_window;
	u32 policy;
	bool dynamic_enabled;
	bool cooldown_active;
};

struct kfastblock_diag_selfcheck_snapshot {
	u32 run_count;
	u32 failure_runs;
	u32 warning_runs;
	s32 last_errno;
	u32 last_failed_checks;
	u32 last_warning_checks;
	u32 last_flags;
	unsigned long last_run_jiffies;
};

struct kfastblock_diag_fault_snapshot {
	bool enabled;
	u32 mask;
	s32 errno_value;
	u32 budget;
	u32 arm_count;
	u32 reset_count;
	u32 hit_count;
	u32 skip_count;
	unsigned long last_arm_jiffies;
	unsigned long last_reset_jiffies;
	unsigned long last_trigger_jiffies;
	u32 last_site;
	s32 last_errno;
	char mask_text[128];
	char last_site_text[48];
};

struct kfastblock_diag_event_snapshot {
	u32 total_events;
	u32 health_change_events;
	u32 metadata_stale_events;
	u32 cluster_refresh_fail_events;
	u32 image_refresh_fail_events;
	u32 leader_query_fail_events;
	u32 object_retry_events;
	u32 object_error_events;
	u32 socket_drop_events;
	u32 socket_backoff_events;
	u32 socket_backoff_wait_events;
	u32 fault_events;
	u32 manual_events;
	u32 last_type;
	s32 last_errno;
	unsigned long oldest_jiffies;
	unsigned long newest_jiffies;
	char last_type_text[48];
};

struct kfastblock_diag_snapshot {
	struct kfastblock_diag_volume_snapshot volume;
	struct kfastblock_diag_buffer_snapshot buffer;
	struct kfastblock_diag_scheduler_snapshot scheduler;
	struct kfastblock_conn_pool_snapshot osd_conn;
	struct kfastblock_conn_pool_snapshot monitor_conn;
	struct kfastblock_diag_selfcheck_snapshot selfcheck;
	struct kfastblock_diag_fault_snapshot fault;
	struct kfastblock_diag_event_snapshot events;
	u32 anomaly_score;
	u32 anomaly_flags;
};

const char *kfastblock_diag_anomaly_status(u32 score);
void kfastblock_diag_collect(struct kfastblock_volume *vol,
			     struct kfastblock_diag_snapshot *snapshot);
int kfastblock_diag_dump_seq(struct seq_file *m,
			     const struct kfastblock_diag_snapshot *snapshot);

#endif
