#ifndef KFASTBLOCK_REQUEST_H
#define KFASTBLOCK_REQUEST_H

#include <linux/blkdev.h>
#include <linux/types.h>

struct kfastblock_volume;

#define KFASTBLOCK_MAX_OBJECT_EXTENTS 128
#define KFASTBLOCK_MAX_OBJECT_NAME_LEN 192

struct kfastblock_object_extent {
	u64 object_seq;
	u32 object_offset;
	u32 length;
	u32 pg_id;
	char object_name[KFASTBLOCK_MAX_OBJECT_NAME_LEN];
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
int kfastblock_request_split(struct kfastblock_request *kf_req);
u32 kfastblock_request_calc_pg(const char *object_name, u32 pg_count);
void kfastblock_request_build_object_name(char *buf, size_t buf_len,
					  u32 pool_id,
					  const char *image_name,
					  u64 object_seq);

#endif
