#ifndef KFASTBLOCK_REQUEST_H
#define KFASTBLOCK_REQUEST_H

#include <linux/blkdev.h>
#include <linux/types.h>

struct kfastblock_volume;

#define KFASTBLOCK_MAX_OBJECT_EXTENTS 128

struct kfastblock_object_extent {
	u64 object_seq;
	u32 object_offset;
	u32 length;
	u32 pg_id;
};

struct kfastblock_request {
	struct request *rq;
	struct kfastblock_volume *vol;
	u64 byte_offset;
	u32 byte_length;
	unsigned int nr_objects;
	int status;
	struct kfastblock_object_extent objects[KFASTBLOCK_MAX_OBJECT_EXTENTS];
};

void kfastblock_request_init(struct kfastblock_request *kf_req,
			     struct kfastblock_volume *vol,
			     struct request *rq);
int kfastblock_request_split(struct kfastblock_request *kf_req, u32 object_size);

#endif
