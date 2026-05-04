#ifndef KFASTBLOCK_REQUEST_H
#define KFASTBLOCK_REQUEST_H

#include <linux/blkdev.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "kfastblock/meta.h"

struct kfastblock_volume;

#define KFASTBLOCK_MAX_OBJECT_EXTENTS 128
#define KFASTBLOCK_MAX_OBJECT_NAME_LEN 192

struct kfastblock_request;

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
	struct mutex dispatch_lock;
	u32 *unique_pgs;
	struct kfastblock_request_pg_hint *pg_hints;
	struct kfastblock_object_extent *objects;
	struct kfastblock_object_work *object_works;
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

#endif
