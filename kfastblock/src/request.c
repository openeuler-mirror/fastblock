#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "kfastblock/request.h"
#include "kfastblock/volume.h"

#define KFASTBLOCK_JHASH_GOLDEN_RATIO 0x9e3779b9U

#define KFASTBLOCK_JHASH_MIX(a, b, c) \
	do { \
		(a) = (a) - (b); \
		(a) = (a) - (c); \
		(a) ^= ((c) >> 13); \
		(b) = (b) - (c); \
		(b) = (b) - (a); \
		(b) ^= ((a) << 8); \
		(c) = (c) - (a); \
		(c) = (c) - (b); \
		(c) ^= ((b) >> 13); \
		(a) = (a) - (b); \
		(a) = (a) - (c); \
		(a) ^= ((c) >> 12); \
		(b) = (b) - (c); \
		(b) = (b) - (a); \
		(b) ^= ((a) << 16); \
		(c) = (c) - (a); \
		(c) = (c) - (b); \
		(c) ^= ((b) >> 5); \
		(a) = (a) - (b); \
		(a) = (a) - (c); \
		(a) ^= ((c) >> 3); \
		(b) = (b) - (c); \
		(b) = (b) - (a); \
		(b) ^= ((a) << 10); \
		(c) = (c) - (a); \
		(c) = (c) - (b); \
		(c) ^= ((b) >> 15); \
	} while (0)

static u32 kfastblock_jenkins_hash(const char *str)
{
	const u8 *k = (const u8 *)str;
	u32 a = KFASTBLOCK_JHASH_GOLDEN_RATIO;
	u32 b = KFASTBLOCK_JHASH_GOLDEN_RATIO;
	u32 c = 0;
	u32 len;
	u32 length;

	if (!str)
		return 0;

	length = strlen(str);
	len = length;
	while (len >= 12) {
		a += (u32)k[0] | ((u32)k[1] << 8) | ((u32)k[2] << 16) |
		     ((u32)k[3] << 24);
		b += (u32)k[4] | ((u32)k[5] << 8) | ((u32)k[6] << 16) |
		     ((u32)k[7] << 24);
		c += (u32)k[8] | ((u32)k[9] << 8) | ((u32)k[10] << 16) |
		     ((u32)k[11] << 24);
		KFASTBLOCK_JHASH_MIX(a, b, c);
		k += 12;
		len -= 12;
	}

	c += length;
	switch (len) {
	case 11:
		c += (u32)k[10] << 24;
		fallthrough;
	case 10:
		c += (u32)k[9] << 16;
		fallthrough;
	case 9:
		c += (u32)k[8] << 8;
		fallthrough;
	case 8:
		b += (u32)k[7] << 24;
		fallthrough;
	case 7:
		b += (u32)k[6] << 16;
		fallthrough;
	case 6:
		b += (u32)k[5] << 8;
		fallthrough;
	case 5:
		b += (u32)k[4];
		fallthrough;
	case 4:
		a += (u32)k[3] << 24;
		fallthrough;
	case 3:
		a += (u32)k[2] << 16;
		fallthrough;
	case 2:
		a += (u32)k[1] << 8;
		fallthrough;
	case 1:
		a += (u32)k[0];
		break;
	default:
		break;
	}
	KFASTBLOCK_JHASH_MIX(a, b, c);
	return c;
}

void kfastblock_request_init(struct kfastblock_request *kf_req,
			     struct kfastblock_volume *vol,
			     struct request *rq)
{
	unsigned int i;

	memset(kf_req, 0, sizeof(*kf_req));
	kf_req->rq = rq;
	kf_req->vol = vol;
	kf_req->byte_offset = blk_rq_pos(rq) << SECTOR_SHIFT;
	kf_req->byte_length = blk_rq_bytes(rq);
	atomic_set(&kf_req->pending_objects, 0);
	spin_lock_init(&kf_req->status_lock);
	for (i = 0; i < KFASTBLOCK_MAX_OBJECT_EXTENTS; ++i) {
		kf_req->object_works[i].parent = kf_req;
		kf_req->object_works[i].object_index = i;
	}
}

u32 kfastblock_request_calc_pg(const char *object_name, u32 pg_count)
{
	if (!object_name || !pg_count)
		return 0;

	return kfastblock_jenkins_hash(object_name) % pg_count;
}

void kfastblock_request_build_object_name(char *buf, size_t buf_len,
					  u32 pool_id,
					  const char *image_name,
					  u64 object_seq)
{
	if (!buf || !buf_len)
		return;

	scnprintf(buf, buf_len, "%u__blk_data___%s%llu",
		  pool_id, image_name ? image_name : "", object_seq);
}

int kfastblock_request_split(struct kfastblock_request *kf_req)
{
	const struct kfastblock_cluster_view *view;
	u64 current_offset;
	u32 object_size;
	u32 remaining;

	if (!kf_req || !kf_req->rq || !kf_req->vol)
		return -EINVAL;

	view = &kf_req->vol->view;
	object_size = view->image.object_size;
	if (!object_size || !view->image.pg_count)
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
		extent->request_offset = kf_req->byte_length - remaining;
		extent->object_offset = object_offset;
		extent->length = object_len;
		kfastblock_request_build_object_name(extent->object_name,
						    sizeof(extent->object_name),
						    view->image.pool_id,
						    view->image.image_name,
						    object_seq);
		extent->pg_id = kfastblock_request_calc_pg(extent->object_name,
						  view->image.pg_count);

		remaining -= object_len;
		current_offset += object_len;
		++kf_req->nr_objects;
	}

	return 0;
}
