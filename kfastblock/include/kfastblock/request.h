#ifndef KFASTBLOCK_REQUEST_H
#define KFASTBLOCK_REQUEST_H

#include <linux/blkdev.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "kfastblock/meta.h"
#include "kfastblock/pipeline.h"

struct kfastblock_volume;

#define KFASTBLOCK_MAX_OBJECT_EXTENTS 128
#define KFASTBLOCK_MAX_OBJECT_NAME_LEN 192

struct kfastblock_request;

enum kfastblock_request_object_state {
	KFASTBLOCK_OBJECT_INIT = 0,
	KFASTBLOCK_OBJECT_READY,
	KFASTBLOCK_OBJECT_QUEUED,
	KFASTBLOCK_OBJECT_IN_FLIGHT,
	KFASTBLOCK_OBJECT_DONE,
	KFASTBLOCK_OBJECT_FAILED,
	KFASTBLOCK_OBJECT_CANCELLED,
};

struct kfastblock_request_pg_target {
	u32 osd_id;
	u32 flags;
	u16 port;
	char address[KFASTBLOCK_MAX_ADDR_LEN];
};

struct kfastblock_object_extent {
	u64 object_seq;
	u32 request_offset;
	u32 object_offset;
	u32 length;
	u32 pg_id;
	char object_name[KFASTBLOCK_MAX_OBJECT_NAME_LEN];
};

struct kfastblock_object_work {
	struct work_struct work;
	struct kfastblock_request *parent;
	unsigned int object_index;
};

struct kfastblock_request_pg_hint {
	spinlock_t lock;
	u32 pg_id;
	unsigned int nr_targets;
	bool leader_valid;
	struct kfastblock_leader_info leader;
	struct kfastblock_request_pg_target *targets;
};

struct kfastblock_request_object_runtime {
	enum kfastblock_request_object_state state;
	int last_error;
	u16 attempt_count;
	u16 dispatch_count;
	u16 retry_count;
	u64 wire_seq;
	s32 response_status;
	u32 response_body_len;
	u32 transport_flags;
	unsigned long queued_jiffies;
	unsigned long last_retry_jiffies;
	unsigned long completed_jiffies;
};

struct kfastblock_request_dispatch_batch {
	unsigned int nr_indexes;
	unsigned int indexes[KFASTBLOCK_MAX_OBJECT_EXTENTS];
};

struct kfastblock_request {
	struct request *rq;
	struct kfastblock_volume *vol;
	u64 byte_offset;
	u64 request_osdmap_epoch;
	u64 request_pgmap_epoch;
	u32 byte_length;
	u32 request_pool_id;
	u32 request_object_size;
	unsigned int nr_objects;
	unsigned int nr_unique_pgs;
	unsigned int next_object_to_queue;
	unsigned int dispatch_window;
	unsigned int max_object_extents;
	int status;
	atomic_t pending_objects;
	spinlock_t status_lock;
	spinlock_t object_state_lock;
	struct mutex dispatch_lock;
	unsigned int dispatch_cursor;
	unsigned int queued_objects;
	unsigned int inflight_objects;
	unsigned int completed_objects;
	unsigned int failed_objects;
	unsigned int cancelled_objects;
	u32 dispatch_generation;
	u32 *unique_pgs;
	struct kfastblock_request_pg_hint *pg_hints;
	struct kfastblock_object_extent *objects;
	struct kfastblock_object_work *object_works;
	struct kfastblock_request_object_runtime *object_runtime;
	struct kfastblock_pipeline_state pipeline;
};

int kfastblock_request_init(struct kfastblock_request *kf_req,
			    struct kfastblock_volume *vol,
			    struct request *rq);
void kfastblock_request_cleanup(struct kfastblock_request *kf_req);
int kfastblock_request_split(struct kfastblock_request *kf_req);
int kfastblock_request_get_pg_hint_leader(
	struct kfastblock_request_pg_hint *hint,
	struct kfastblock_leader_info *leader);
void kfastblock_request_set_pg_hint_leader(
	struct kfastblock_request_pg_hint *hint,
	const struct kfastblock_leader_info *leader);
void kfastblock_request_invalidate_pg_hint_leader(
	struct kfastblock_request_pg_hint *hint);
u32 kfastblock_request_calc_pg(const char *object_name, u32 pg_count);
void kfastblock_request_build_object_name(char *buf, size_t buf_len,
					  u32 pool_id,
					  const char *image_name,
					  u64 object_seq);
void kfastblock_request_dispatch_batch_reset(
	struct kfastblock_request_dispatch_batch *batch);
void kfastblock_request_prepare_runtime(struct kfastblock_request *kf_req);
int kfastblock_request_pick_dispatch_batch(
	struct kfastblock_request *kf_req,
	struct kfastblock_request_dispatch_batch *batch,
	unsigned int max_dispatch);
int kfastblock_request_mark_object_queued(
	struct kfastblock_request *kf_req,
	unsigned int object_index);
void kfastblock_request_mark_object_inflight(
	struct kfastblock_request *kf_req,
	unsigned int object_index);
void kfastblock_request_mark_object_complete(
	struct kfastblock_request *kf_req,
	unsigned int object_index,
	int ret);
void kfastblock_request_record_object_seq(
	struct kfastblock_request *kf_req,
	unsigned int object_index,
	u64 seq);
void kfastblock_request_record_object_response(
	struct kfastblock_request *kf_req,
	unsigned int object_index,
	s32 response_status,
	u32 response_body_len,
	u32 transport_flags);
int kfastblock_request_record_object_response_by_seq(
	struct kfastblock_request *kf_req,
	u64 seq,
	s32 response_status,
	u32 response_body_len,
	u32 transport_flags);
void kfastblock_request_note_object_retry(
	struct kfastblock_request *kf_req,
	unsigned int object_index,
	int ret);
int kfastblock_request_lookup_object_by_seq(
	struct kfastblock_request *kf_req,
	u64 seq,
	unsigned int *object_index);
int kfastblock_request_complete_object_by_seq(
	struct kfastblock_request *kf_req,
	u64 seq,
	int ret);
int kfastblock_request_cancel_unqueued(struct kfastblock_request *kf_req);
unsigned int kfastblock_request_dispatch_credits(
	const struct kfastblock_request *kf_req);
unsigned int kfastblock_request_queued_objects(
	const struct kfastblock_request *kf_req);
unsigned int kfastblock_request_inflight_objects(
	const struct kfastblock_request *kf_req);
unsigned int kfastblock_request_completed_objects(
	const struct kfastblock_request *kf_req);

#endif
