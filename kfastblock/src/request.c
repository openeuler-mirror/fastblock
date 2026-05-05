#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kfastblock/request.h"
#include "kfastblock/scheduler.h"
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

static void kfastblock_request_track_unique_pg(struct kfastblock_request *kf_req,
					       u32 pg_id)
{
	unsigned int i;

	if (!kf_req)
		return;

	for (i = 0; i < kf_req->nr_unique_pgs; ++i) {
		if (kf_req->unique_pgs[i] == pg_id)
			return;
	}
	if (kf_req->nr_unique_pgs >= kf_req->max_object_extents)
		return;

	kf_req->unique_pgs[kf_req->nr_unique_pgs++] = pg_id;
	kf_req->pg_hints[kf_req->nr_unique_pgs - 1].pg_id = pg_id;
	kf_req->pg_hints[kf_req->nr_unique_pgs - 1].nr_targets = 0;
	kf_req->pg_hints[kf_req->nr_unique_pgs - 1].leader_valid = false;
	kfree(kf_req->pg_hints[kf_req->nr_unique_pgs - 1].targets);
	kf_req->pg_hints[kf_req->nr_unique_pgs - 1].targets = NULL;
}

static unsigned int
kfastblock_request_estimate_extent_count(const struct kfastblock_request *kf_req)
{
	u64 first_object;
	u64 last_object;

	if (!kf_req || !kf_req->byte_length || !kf_req->request_object_size)
		return 0;

	first_object = div_u64(kf_req->byte_offset, kf_req->request_object_size);
	last_object = div_u64(kf_req->byte_offset + kf_req->byte_length - 1,
			      kf_req->request_object_size);

	if (last_object < first_object)
		return 0;
	if (last_object - first_object + 1 > KFASTBLOCK_MAX_OBJECT_EXTENTS)
		return KFASTBLOCK_MAX_OBJECT_EXTENTS + 1;

	return (unsigned int)(last_object - first_object + 1);
}

static int kfastblock_request_alloc_state(struct kfastblock_request *kf_req)
{
	unsigned int count;

	if (!kf_req)
		return -EINVAL;

	count = kfastblock_request_estimate_extent_count(kf_req);
	if (!count)
		return 0;
	if (count > KFASTBLOCK_MAX_OBJECT_EXTENTS)
		return -E2BIG;

	kf_req->unique_pgs = kvcalloc(count, sizeof(*kf_req->unique_pgs), GFP_NOIO);
	if (!kf_req->unique_pgs)
		return -ENOMEM;

	kf_req->pg_hints = kvcalloc(count, sizeof(*kf_req->pg_hints), GFP_NOIO);
	if (!kf_req->pg_hints)
		goto err_free_unique;

	kf_req->objects = kvcalloc(count, sizeof(*kf_req->objects), GFP_NOIO);
	if (!kf_req->objects)
		goto err_free_hints;

	kf_req->object_works = kvcalloc(count, sizeof(*kf_req->object_works),
					GFP_NOIO);
	if (!kf_req->object_works)
		goto err_free_objects;

	kf_req->object_runtime = kvcalloc(count, sizeof(*kf_req->object_runtime),
					  GFP_NOIO);
	if (!kf_req->object_runtime)
		goto err_free_object_works;

	kf_req->max_object_extents = count;
	return 0;

err_free_object_works:
	kvfree(kf_req->object_works);
	kf_req->object_works = NULL;
err_free_objects:
	kvfree(kf_req->objects);
	kf_req->objects = NULL;
err_free_hints:
	kvfree(kf_req->pg_hints);
	kf_req->pg_hints = NULL;
err_free_unique:
	kvfree(kf_req->unique_pgs);
	kf_req->unique_pgs = NULL;
	return -ENOMEM;
}

static void kfastblock_request_free_pg_hint_targets(struct kfastblock_request *kf_req)
{
	unsigned int i;

	if (!kf_req || !kf_req->pg_hints)
		return;

	for (i = 0; i < kf_req->max_object_extents; ++i) {
		kfree(kf_req->pg_hints[i].targets);
		kf_req->pg_hints[i].targets = NULL;
		kf_req->pg_hints[i].nr_targets = 0;
		kf_req->pg_hints[i].leader_valid = false;
		memset(&kf_req->pg_hints[i].leader, 0,
		       sizeof(kf_req->pg_hints[i].leader));
	}
}

static int kfastblock_request_capture_pg_hints(
	struct kfastblock_request *kf_req,
	const struct kfastblock_cluster_view *view)
{
	unsigned int i;

	if (!kf_req || !view)
		return -EINVAL;

	for (i = 0; i < kf_req->nr_unique_pgs; ++i) {
		struct kfastblock_request_pg_hint *hint = &kf_req->pg_hints[i];
		const struct kfastblock_pg_route *route;
		u32 replica_idx;

		route = kfastblock_meta_find_pg_route(view, kf_req->request_pool_id,
						      hint->pg_id);
		if (!route || !route->replica_count)
			return -ENOENT;

		hint->nr_targets = route->replica_count;
		hint->leader_valid = route->leader_valid;
		if (route->leader_valid)
			hint->leader = route->leader;
		else
			memset(&hint->leader, 0, sizeof(hint->leader));

		hint->targets = kcalloc(route->replica_count, sizeof(*hint->targets),
					 GFP_NOIO);
		if (!hint->targets)
			return -ENOMEM;

		for (replica_idx = 0; replica_idx < route->replica_count; ++replica_idx) {
			const struct kfastblock_pg_route *resolved_route;
			const struct kfastblock_osd_endpoint *osd;
			const struct kfastblock_osd_shard *shard;
			int ret;

			ret = kfastblock_meta_resolve_pg_target(view,
					kf_req->request_pool_id,
					hint->pg_id,
					route->osd_ids[replica_idx],
					&resolved_route,
					&osd,
					&shard);
			if (ret)
				return ret;

			hint->targets[replica_idx].osd_id = osd->osd_id;
			hint->targets[replica_idx].flags = osd->flags;
			hint->targets[replica_idx].port = shard->port;
			strscpy(hint->targets[replica_idx].address, osd->address,
				sizeof(hint->targets[replica_idx].address));
		}
	}

	return 0;
}

void kfastblock_request_cleanup(struct kfastblock_request *kf_req)
{
	if (!kf_req)
		return;

	kvfree(kf_req->unique_pgs);
	kfastblock_request_free_pg_hint_targets(kf_req);
	kvfree(kf_req->pg_hints);
	kvfree(kf_req->objects);
	kvfree(kf_req->object_works);
	kvfree(kf_req->object_runtime);
	kf_req->unique_pgs = NULL;
	kf_req->pg_hints = NULL;
	kf_req->objects = NULL;
	kf_req->object_works = NULL;
	kf_req->object_runtime = NULL;
	kf_req->max_object_extents = 0;
}

int kfastblock_request_init(struct kfastblock_request *kf_req,
			    struct kfastblock_volume *vol,
			    struct request *rq)
{
	unsigned int i;
	int ret;

	if (!kf_req || !vol || !rq)
		return -EINVAL;

	kfastblock_request_cleanup(kf_req);
	memset(kf_req, 0, sizeof(*kf_req));
	kf_req->rq = rq;
	kf_req->vol = vol;
	kf_req->byte_offset = blk_rq_pos(rq) << SECTOR_SHIFT;
	kf_req->byte_length = blk_rq_bytes(rq);
	kf_req->request_pool_id = vol->view.image.pool_id;
	kf_req->request_object_size = vol->view.image.object_size;
	kf_req->request_osdmap_epoch = vol->view.osdmap_epoch;
	kf_req->request_pgmap_epoch = vol->view.pgmap_epoch;
	ret = kfastblock_request_alloc_state(kf_req);
	if (ret)
		return ret;
	kf_req->dispatch_window = clamp_t(u32,
					  kfastblock_scheduler_sample_window(
						  &vol->scheduler,
						  atomic_read(&vol->inflight_ios),
						  kf_req->max_object_extents,
						  NULL),
					  1,
					  kf_req->max_object_extents ?
					  kf_req->max_object_extents : 1);
	kf_req->next_object_to_queue = 0;
	atomic_set(&kf_req->pending_objects, 0);
	spin_lock_init(&kf_req->status_lock);
	spin_lock_init(&kf_req->object_state_lock);
	mutex_init(&kf_req->dispatch_lock);
	for (i = 0; i < kf_req->max_object_extents; ++i) {
		kf_req->object_works[i].parent = kf_req;
		kf_req->object_works[i].object_index = i;
		spin_lock_init(&kf_req->pg_hints[i].lock);
		kf_req->pg_hints[i].pg_id = 0;
		kf_req->pg_hints[i].nr_targets = 0;
		kf_req->pg_hints[i].leader_valid = false;
		kf_req->pg_hints[i].targets = NULL;
	}

	return 0;
}

void kfastblock_request_dispatch_batch_reset(
	struct kfastblock_request_dispatch_batch *batch)
{
	if (!batch)
		return;

	batch->nr_indexes = 0;
	memset(batch->indexes, 0, sizeof(batch->indexes));
}

void kfastblock_request_prepare_runtime(struct kfastblock_request *kf_req)
{
	unsigned long flags;
	unsigned int i;

	if (!kf_req || !kf_req->object_runtime)
		return;

	spin_lock_irqsave(&kf_req->object_state_lock, flags);
	kf_req->dispatch_cursor = 0;
	kf_req->queued_objects = 0;
	kf_req->inflight_objects = 0;
	kf_req->completed_objects = 0;
	kf_req->failed_objects = 0;
	kf_req->cancelled_objects = 0;
	kf_req->dispatch_generation++;
	for (i = 0; i < kf_req->nr_objects; ++i) {
		kf_req->object_runtime[i].state = KFASTBLOCK_OBJECT_READY;
		kf_req->object_runtime[i].last_error = 0;
		kf_req->object_runtime[i].dispatch_count = 0;
		kf_req->object_runtime[i].attempt_count = 0;
		kf_req->object_runtime[i].wire_seq = 0;
		kf_req->object_runtime[i].queued_jiffies = 0;
		kf_req->object_runtime[i].completed_jiffies = 0;
	}
	spin_unlock_irqrestore(&kf_req->object_state_lock, flags);
}

static bool kfastblock_request_object_dispatchable(
	const struct kfastblock_request_object_runtime *runtime)
{
	return runtime && runtime->state == KFASTBLOCK_OBJECT_READY;
}

int kfastblock_request_pick_dispatch_batch(
	struct kfastblock_request *kf_req,
	struct kfastblock_request_dispatch_batch *batch,
	unsigned int max_dispatch)
{
	unsigned long flags;
	unsigned int count = 0;
	unsigned int scanned = 0;
	unsigned int cursor;

	if (!kf_req || !batch || !max_dispatch)
		return -EINVAL;

	kfastblock_request_dispatch_batch_reset(batch);
	if (!kf_req->nr_objects || !kf_req->object_runtime)
		return 0;

	spin_lock_irqsave(&kf_req->object_state_lock, flags);
	cursor = kf_req->dispatch_cursor;
	while (scanned < kf_req->nr_objects && count < max_dispatch) {
		unsigned int index = (cursor + scanned) % kf_req->nr_objects;

		if (kfastblock_request_object_dispatchable(
			    &kf_req->object_runtime[index])) {
			batch->indexes[count++] = index;
			kf_req->object_runtime[index].state =
				KFASTBLOCK_OBJECT_QUEUED;
			kf_req->object_runtime[index].dispatch_count++;
			kf_req->object_runtime[index].queued_jiffies = jiffies;
			kf_req->queued_objects++;
		}
		scanned++;
	}
	if (count)
		kf_req->dispatch_cursor = (batch->indexes[count - 1] + 1) %
					  kf_req->nr_objects;
	batch->nr_indexes = count;
	spin_unlock_irqrestore(&kf_req->object_state_lock, flags);

	return count;
}

int kfastblock_request_mark_object_queued(
	struct kfastblock_request *kf_req,
	unsigned int object_index)
{
	unsigned long flags;
	int ret = 0;

	if (!kf_req || !kf_req->object_runtime || object_index >= kf_req->nr_objects)
		return -EINVAL;

	spin_lock_irqsave(&kf_req->object_state_lock, flags);
	if (kf_req->object_runtime[object_index].state !=
	    KFASTBLOCK_OBJECT_QUEUED) {
		ret = -EALREADY;
		goto out_unlock;
	}
	kf_req->object_runtime[object_index].attempt_count++;
out_unlock:
	spin_unlock_irqrestore(&kf_req->object_state_lock, flags);
	return ret;
}

void kfastblock_request_mark_object_inflight(
	struct kfastblock_request *kf_req,
	unsigned int object_index)
{
	unsigned long flags;

	if (!kf_req || !kf_req->object_runtime || object_index >= kf_req->nr_objects)
		return;

	spin_lock_irqsave(&kf_req->object_state_lock, flags);
	if (kf_req->object_runtime[object_index].state ==
	    KFASTBLOCK_OBJECT_QUEUED) {
		kf_req->object_runtime[object_index].state =
			KFASTBLOCK_OBJECT_IN_FLIGHT;
		if (kf_req->queued_objects)
			kf_req->queued_objects--;
		kf_req->inflight_objects++;
	}
	spin_unlock_irqrestore(&kf_req->object_state_lock, flags);
}

void kfastblock_request_mark_object_complete(
	struct kfastblock_request *kf_req,
	unsigned int object_index,
	int ret)
{
	unsigned long flags;
	struct kfastblock_request_object_runtime *runtime;

	if (!kf_req || !kf_req->object_runtime || object_index >= kf_req->nr_objects)
		return;

	spin_lock_irqsave(&kf_req->object_state_lock, flags);
	runtime = &kf_req->object_runtime[object_index];
	if (runtime->state == KFASTBLOCK_OBJECT_IN_FLIGHT &&
	    kf_req->inflight_objects)
		kf_req->inflight_objects--;
	runtime->last_error = ret;
	runtime->completed_jiffies = jiffies;
	if (ret) {
		runtime->state = KFASTBLOCK_OBJECT_FAILED;
		kf_req->failed_objects++;
	} else {
		runtime->state = KFASTBLOCK_OBJECT_DONE;
		kf_req->completed_objects++;
	}
	spin_unlock_irqrestore(&kf_req->object_state_lock, flags);
}

int kfastblock_request_cancel_unqueued(struct kfastblock_request *kf_req)
{
	unsigned long flags;
	unsigned int i;
	unsigned int cancelled = 0;

	if (!kf_req || !kf_req->object_runtime)
		return 0;

	spin_lock_irqsave(&kf_req->object_state_lock, flags);
	for (i = 0; i < kf_req->nr_objects; ++i) {
		struct kfastblock_request_object_runtime *runtime;

		runtime = &kf_req->object_runtime[i];
		if (runtime->state != KFASTBLOCK_OBJECT_READY &&
		    runtime->state != KFASTBLOCK_OBJECT_QUEUED)
			continue;
		if (runtime->state == KFASTBLOCK_OBJECT_QUEUED &&
		    kf_req->queued_objects)
			kf_req->queued_objects--;
		runtime->state = KFASTBLOCK_OBJECT_CANCELLED;
		runtime->last_error = -ECANCELED;
		runtime->completed_jiffies = jiffies;
		cancelled++;
	}
	kf_req->cancelled_objects += cancelled;
	spin_unlock_irqrestore(&kf_req->object_state_lock, flags);

	return cancelled;
}

unsigned int kfastblock_request_inflight_objects(
	const struct kfastblock_request *kf_req)
{
	unsigned int value = 0;
	unsigned long flags;

	if (!kf_req)
		return 0;

	spin_lock_irqsave((spinlock_t *)&kf_req->object_state_lock, flags);
	value = kf_req->inflight_objects;
	spin_unlock_irqrestore((spinlock_t *)&kf_req->object_state_lock, flags);
	return value;
}

unsigned int kfastblock_request_queued_objects(
	const struct kfastblock_request *kf_req)
{
	unsigned int value = 0;
	unsigned long flags;

	if (!kf_req)
		return 0;

	spin_lock_irqsave((spinlock_t *)&kf_req->object_state_lock, flags);
	value = kf_req->queued_objects;
	spin_unlock_irqrestore((spinlock_t *)&kf_req->object_state_lock, flags);
	return value;
}

unsigned int kfastblock_request_dispatch_credits(
	const struct kfastblock_request *kf_req)
{
	unsigned int dispatched;
	unsigned int window;
	unsigned int credits = 0;
	unsigned long flags;

	if (!kf_req)
		return 0;

	spin_lock_irqsave((spinlock_t *)&kf_req->object_state_lock, flags);
	window = kf_req->dispatch_window ? kf_req->dispatch_window : 1;
	dispatched = kf_req->queued_objects + kf_req->inflight_objects;
	credits = dispatched >= window ? 0 : window - dispatched;
	spin_unlock_irqrestore((spinlock_t *)&kf_req->object_state_lock, flags);
	return credits;
}

unsigned int kfastblock_request_completed_objects(
	const struct kfastblock_request *kf_req)
{
	unsigned int value = 0;
	unsigned long flags;

	if (!kf_req)
		return 0;

	spin_lock_irqsave((spinlock_t *)&kf_req->object_state_lock, flags);
	value = kf_req->completed_objects;
	spin_unlock_irqrestore((spinlock_t *)&kf_req->object_state_lock, flags);
	return value;
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
	object_size = kf_req->request_object_size;
	if (!object_size || !view->image.pg_count)
		return -EINVAL;

	current_offset = kf_req->byte_offset;
	remaining = kf_req->byte_length;
	while (remaining) {
		struct kfastblock_object_extent *extent;
		u64 object_seq;
		u32 object_offset;
		u32 object_len;

		if (kf_req->nr_objects >= kf_req->max_object_extents)
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
						    kf_req->request_pool_id,
						    view->image.image_name,
						    object_seq);
		extent->pg_id = kfastblock_request_calc_pg(extent->object_name,
						  view->image.pg_count);
		kfastblock_request_track_unique_pg(kf_req, extent->pg_id);

		remaining -= object_len;
			current_offset += object_len;
			++kf_req->nr_objects;
		}

	return kfastblock_request_capture_pg_hints(kf_req, view);
}

int kfastblock_request_get_pg_hint_leader(
	struct kfastblock_request_pg_hint *hint,
	struct kfastblock_leader_info *leader)
{
	unsigned long flags;
	int ret = 0;

	if (!hint || !leader)
		return -EINVAL;

	spin_lock_irqsave(&hint->lock, flags);
	if (!hint->leader_valid) {
		ret = -ENOENT;
	} else {
		*leader = hint->leader;
	}
	spin_unlock_irqrestore(&hint->lock, flags);
	return ret;
}

void kfastblock_request_set_pg_hint_leader(
	struct kfastblock_request_pg_hint *hint,
	const struct kfastblock_leader_info *leader)
{
	unsigned long flags;

	if (!hint || !leader)
		return;

	spin_lock_irqsave(&hint->lock, flags);
	hint->leader = *leader;
	hint->leader_valid = true;
	spin_unlock_irqrestore(&hint->lock, flags);
}

void kfastblock_request_invalidate_pg_hint_leader(
	struct kfastblock_request_pg_hint *hint)
{
	unsigned long flags;

	if (!hint)
		return;

	spin_lock_irqsave(&hint->lock, flags);
	hint->leader_valid = false;
	memset(&hint->leader, 0, sizeof(hint->leader));
	spin_unlock_irqrestore(&hint->lock, flags);
}
