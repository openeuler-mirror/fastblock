#ifndef KFASTBLOCK_VOLUME_H
#define KFASTBLOCK_VOLUME_H

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

struct dentry;

#include "kfastblock/control.h"
#include "kfastblock/buffer.h"
#include "kfastblock/connpool.h"
#include "kfastblock/diag.h"
#include "kfastblock/fault.h"
#include "kfastblock/meta.h"
#include "kfastblock/scheduler.h"
#include "kfastblock/selfcheck.h"

#define KFASTBLOCK_MAX_SOCKET_CACHE 16
#define KFASTBLOCK_MAX_VOLUME_EVENTS 128

enum kfastblock_volume_event_type {
	KFASTBLOCK_VOLUME_EVENT_ATTACH_READY = 1,
	KFASTBLOCK_VOLUME_EVENT_QUEUE_PAUSE,
	KFASTBLOCK_VOLUME_EVENT_QUEUE_RESUME,
	KFASTBLOCK_VOLUME_EVENT_METADATA_STALE,
	KFASTBLOCK_VOLUME_EVENT_CLUSTER_REFRESH_FAIL,
	KFASTBLOCK_VOLUME_EVENT_IMAGE_REFRESH_FAIL,
	KFASTBLOCK_VOLUME_EVENT_LEADER_QUERY_FAIL,
	KFASTBLOCK_VOLUME_EVENT_OBJECT_DONE,
	KFASTBLOCK_VOLUME_EVENT_OBJECT_RETRY,
	KFASTBLOCK_VOLUME_EVENT_OBJECT_ERROR,
	KFASTBLOCK_VOLUME_EVENT_REFRESH_KICK,
	KFASTBLOCK_VOLUME_EVENT_LEADER_INVALIDATE,
	KFASTBLOCK_VOLUME_EVENT_OSD_SOCKET_DROP,
	KFASTBLOCK_VOLUME_EVENT_MONITOR_SOCKET_DROP,
	KFASTBLOCK_VOLUME_EVENT_HEALTH_CHANGE,
	KFASTBLOCK_VOLUME_EVENT_SOCKET_BACKOFF,
	KFASTBLOCK_VOLUME_EVENT_MANUAL_REFRESH,
	KFASTBLOCK_VOLUME_EVENT_MANUAL_RESET_BACKOFF,
	KFASTBLOCK_VOLUME_EVENT_MANUAL_DROP_TRANSPORT,
	KFASTBLOCK_VOLUME_EVENT_MANUAL_RESET_LEADERS,
	KFASTBLOCK_VOLUME_EVENT_MANUAL_QUEUE_PAUSE,
	KFASTBLOCK_VOLUME_EVENT_MANUAL_QUEUE_RESUME,
	KFASTBLOCK_VOLUME_EVENT_SOCKET_BACKOFF_WAIT,
	KFASTBLOCK_VOLUME_EVENT_MANUAL_FAULT_INJECTION_ARM,
	KFASTBLOCK_VOLUME_EVENT_MANUAL_FAULT_INJECTION_RESET,
	KFASTBLOCK_VOLUME_EVENT_FAULT_INJECTION_TRIGGER,
};

enum kfastblock_volume_health_state {
	KFASTBLOCK_VOLUME_HEALTH_UNKNOWN = 0,
	KFASTBLOCK_VOLUME_HEALTH_READY = 1,
	KFASTBLOCK_VOLUME_HEALTH_DEGRADED = 2,
	KFASTBLOCK_VOLUME_HEALTH_STALE = 3,
};

enum kfastblock_volume_failure_source {
	KFASTBLOCK_VOLUME_SOURCE_NONE = 0,
	KFASTBLOCK_VOLUME_SOURCE_ATTACH = 1,
	KFASTBLOCK_VOLUME_SOURCE_QUEUE_GATE = 2,
	KFASTBLOCK_VOLUME_SOURCE_CLUSTER_REFRESH = 3,
	KFASTBLOCK_VOLUME_SOURCE_IMAGE_REFRESH = 4,
	KFASTBLOCK_VOLUME_SOURCE_LEADER_QUERY = 5,
	KFASTBLOCK_VOLUME_SOURCE_OBJECT_IO = 6,
	KFASTBLOCK_VOLUME_SOURCE_MONITOR_SOCKET = 7,
	KFASTBLOCK_VOLUME_SOURCE_OSD_SOCKET = 8,
};

struct kfastblock_volume_stats {
	atomic64_t io_submitted;
	atomic64_t io_completed;
	atomic64_t io_failed;
	atomic64_t read_requests;
	atomic64_t write_requests;
	atomic64_t discard_requests;
	atomic64_t flush_requests;
	atomic64_t write_zeroes_requests;
	atomic64_t read_bytes;
	atomic64_t write_bytes;
	atomic64_t discard_bytes;
	atomic64_t queue_pause_events;
	atomic64_t queue_resume_events;
	atomic64_t metadata_stale_events;
	atomic64_t cluster_refresh_ok;
	atomic64_t cluster_refresh_fail;
	atomic64_t image_refresh_ok;
	atomic64_t image_refresh_fail;
	atomic64_t leader_query_ok;
	atomic64_t leader_query_fail;
	atomic64_t object_io_completed;
	atomic64_t object_io_retries;
	atomic64_t object_io_errors;
	atomic64_t refresh_kicks;
	atomic64_t leader_invalidations;
	atomic64_t osd_socket_drops;
	atomic64_t monitor_socket_drops;
	atomic64_t osd_backoff_hits;
	atomic64_t monitor_backoff_hits;
	atomic64_t osd_backoff_waits;
	atomic64_t monitor_backoff_waits;
	atomic64_t manual_refreshes;
	atomic64_t manual_reset_backoffs;
	atomic64_t manual_transport_drops;
	atomic64_t manual_leader_resets;
	atomic64_t manual_queue_pauses;
	atomic64_t manual_queue_resumes;
};

struct kfastblock_volume_health {
	u32 state;
	u32 last_failure_source;
	s32 last_errno;
	unsigned long state_since_jiffies;
	unsigned long last_failure_jiffies;
	unsigned long last_success_jiffies;
};

struct kfastblock_volume_event {
	u64 jiffies;
	s32 ret;
	u32 type;
	u32 pg_id;
	u32 osd_id;
	u32 arg0;
	u32 arg1;
	u8 op;
	u8 reserved[3];
};

struct kfastblock_volume_event_log {
	spinlock_t lock;
	u32 next_index;
	u32 count;
	struct kfastblock_volume_event entries[KFASTBLOCK_MAX_VOLUME_EVENTS];
};

struct kfastblock_volume {
	int dev_id;
	int major;
	int minor;

	struct gendisk *disk;
	struct blk_mq_tag_set tag_set;

	atomic_t open_count;
	atomic_t ready;
	atomic_t inflight_ios;

	struct kfastblock_attach_spec spec;
	struct kfastblock_cluster_view view;
	struct kfastblock_volume_stats stats;
	struct kfastblock_volume_health health;
	struct kfastblock_volume_event_log event_log;
	struct kfastblock_selfcheck_state selfcheck;
	struct kfastblock_fault_injection_state fault_injection;
	struct kfastblock_diag_baseline_state diag_baseline;

	struct list_head node;
	struct device dev;
	struct mutex lifecycle_lock;
	struct mutex inflight_lock;
	struct kfastblock_object_buffer_pool object_buffer_pool;
	struct kfastblock_scheduler_controller scheduler;
	wait_queue_head_t inflight_wq;
	bool flush_in_progress;
	struct rw_semaphore state_lock;
	struct delayed_work refresh_work;
	struct dentry *debugfs_dir;
	bool queue_paused;
	bool manual_queue_pause;
	u32 dispatch_window;
	u32 refresh_interval_ms;
	u32 image_refresh_interval_ms;
	struct kfastblock_cached_socket socket_cache[KFASTBLOCK_MAX_SOCKET_CACHE];
	struct kfastblock_cached_monitor_socket monitor_cache[KFASTBLOCK_MAX_MONITORS];
};

int kfastblock_volume_init(void);
void kfastblock_volume_exit(void);
int kfastblock_volume_attach(const struct kfastblock_attach_spec *spec, int major,
			     struct bus_type *bus, struct device *parent_dev);
int kfastblock_volume_detach(const struct kfastblock_attach_spec *spec);
void kfastblock_volume_kick_refresh(struct kfastblock_volume *vol);
void kfastblock_volume_get_io(struct kfastblock_volume *vol);
void kfastblock_volume_put_io(struct kfastblock_volume *vol);
int kfastblock_volume_drain_io(struct kfastblock_volume *vol,
			     unsigned long timeout_jiffies);
void kfastblock_volume_stats_init(struct kfastblock_volume *vol);
void kfastblock_volume_account_io_submit(struct kfastblock_volume *vol,
				       enum req_op op, u32 bytes);
void kfastblock_volume_account_io_complete(struct kfastblock_volume *vol, int ret);
void kfastblock_volume_account_object_success(struct kfastblock_volume *vol,
				      enum req_op op, u32 pg_id,
				      u32 osd_id, u32 length);
void kfastblock_volume_account_object_retry(struct kfastblock_volume *vol,
				      enum req_op op, u32 pg_id,
				      u32 osd_id, u32 length, int ret);
void kfastblock_volume_account_object_error(struct kfastblock_volume *vol,
				     enum req_op op, u32 pg_id,
				     u32 osd_id, u32 length, int ret);
void kfastblock_volume_account_leader_query(struct kfastblock_volume *vol,
				     u32 pg_id, u32 osd_id, int ret);
void kfastblock_volume_account_cluster_refresh(struct kfastblock_volume *vol,
				       int ret);
void kfastblock_volume_account_image_refresh(struct kfastblock_volume *vol,
				     int ret);
void kfastblock_volume_account_metadata_stale(struct kfastblock_volume *vol);
void kfastblock_volume_account_refresh_kick(struct kfastblock_volume *vol,
				      u32 pg_id, int ret);
void kfastblock_volume_account_leader_invalidate(struct kfastblock_volume *vol,
				       u32 pg_id, int ret);
void kfastblock_volume_account_socket_drop(struct kfastblock_volume *vol,
				    bool monitor_socket, u32 osd_id,
				    u16 port, int ret);
void kfastblock_volume_account_socket_backoff(struct kfastblock_volume *vol,
				       bool monitor_socket, u32 osd_id,
				       u16 port, u32 fail_streak,
				       unsigned long backoff_jiffies, int ret);
void kfastblock_volume_account_socket_backoff_wait(struct kfastblock_volume *vol,
					 bool monitor_socket, u32 osd_id,
					 u16 port,
					 unsigned long remaining_jiffies,
					 int ret);
void kfastblock_volume_account_fault_injection(struct kfastblock_volume *vol,
					 u32 site, int ret);
void kfastblock_volume_update_health(struct kfastblock_volume *vol,
				     u32 new_state, u32 source, int ret);
void kfastblock_volume_mark_success(struct kfastblock_volume *vol, u32 source);

#endif
