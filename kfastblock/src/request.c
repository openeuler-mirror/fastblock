#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "kfastblock/request.h"

void kfastblock_request_init(struct kfastblock_request *kf_req,
			     struct kfastblock_volume *vol,
			     struct request *rq)
{
	memset(kf_req, 0, sizeof(*kf_req));
	kf_req->rq = rq;
	kf_req->vol = vol;
	kf_req->byte_offset = blk_rq_pos(rq) << SECTOR_SHIFT;
	kf_req->byte_length = blk_rq_bytes(rq);
}

int kfastblock_request_split(struct kfastblock_request *kf_req, u32 object_size)
{
	u64 current_offset;
	u32 remaining;

	if (!kf_req || !kf_req->rq || !object_size)
		return -EINVAL;

	current_offset = kf_req->byte_offset;
	remaining = kf_req->byte_length;
	while (remaining) {
		struct kfastblock_object_extent *extent;
		u64 object_seq;
		u32 object_offset;
		u32 object_len;

		if (kf_req->nr_objects >= KFASTBLOCK_MAX_OBJECT_EXTENTS)
			return -E2BIG;

		extent = &kf_req->objects[kf_req->nr_objects];
		object_seq = div_u64(current_offset, object_size);
		object_offset = current_offset % object_size;
		object_len = min_t(u32, remaining, object_size - object_offset);

		extent->object_seq = object_seq;
		extent->object_offset = object_offset;
		extent->length = object_len;
		extent->pg_id = 0;

		remaining -= object_len;
		current_offset += object_len;
		++kf_req->nr_objects;
	}

	return 0;
}
