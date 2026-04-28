#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "kfastblock/common.h"
#include "kfastblock/meta.h"
#include "kfastblock/request.h"
#include "kfastblock/transport.h"
#include "kfastblock/volume.h"

static LIST_HEAD(g_kfastblock_volumes);
static DEFINE_MUTEX(g_kfastblock_volumes_lock);
static DEFINE_IDA(g_kfastblock_dev_ida);

static int kfastblock_blk_open(struct gendisk *disk, blk_mode_t mode)
{
	struct kfastblock_volume *vol = disk->private_data;

	if (!vol)
		return -ENXIO;

	if (!atomic_read(&vol->ready))
		return -ENOENT;

	atomic_inc(&vol->open_count);
	get_device(&vol->dev);
	return 0;
}

static void kfastblock_blk_release(struct gendisk *disk)
{
	struct kfastblock_volume *vol = disk->private_data;

	put_device(&vol->dev);
	atomic_dec(&vol->open_count);
}

static blk_status_t kfastblock_queue_rq(struct blk_mq_hw_ctx *hctx,
					const struct blk_mq_queue_data *bd)
{
	struct kfastblock_volume *vol = hctx->queue->queuedata;
	struct request *rq = bd->rq;
	struct kfastblock_request *kf_req = blk_mq_rq_to_pdu(rq);
	int ret;

	blk_mq_start_request(rq);

	if (!vol || !atomic_read(&vol->ready)) {
		blk_mq_end_request(rq, BLK_STS_RESOURCE);
		return BLK_STS_RESOURCE;
	}

	if (blk_rq_is_passthrough(rq)) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}

	kfastblock_request_init(kf_req, vol, rq);
	ret = kfastblock_request_split(kf_req, vol->view.image.object_size);
	if (ret) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}

	ret = kfastblock_transport_submit(kf_req);
	if (ret) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_IOERR;
	}

	return BLK_STS_OK;
}

static const struct block_device_operations kfastblock_bd_ops = {
	.owner = THIS_MODULE,
	.open = kfastblock_blk_open,
	.release = kfastblock_blk_release,
};

static const struct blk_mq_ops kfastblock_mq_ops = {
	.queue_rq = kfastblock_queue_rq,
};

static void kfastblock_volume_free(struct kfastblock_volume *vol)
{
	if (!vol)
		return;

	atomic_set(&vol->ready, 0);

	mutex_lock(&g_kfastblock_volumes_lock);
	if (!list_empty(&vol->node))
		list_del_init(&vol->node);
	mutex_unlock(&g_kfastblock_volumes_lock);

	if (vol->disk) {
		del_gendisk(vol->disk);
		put_disk(vol->disk);
		vol->disk = NULL;
	}

	blk_mq_free_tag_set(&vol->tag_set);

	if (vol->dev_id >= 0)
		ida_free(&g_kfastblock_dev_ida, vol->dev_id);

	kfree(vol);
	module_put(THIS_MODULE);
}

static void kfastblock_volume_dev_release(struct device *dev)
{
	struct kfastblock_volume *vol;

	vol = container_of(dev, struct kfastblock_volume, dev);
	kfastblock_volume_free(vol);
}

static const struct device_type kfastblock_volume_type = {
	.name = "kfastblock_volume",
	.release = kfastblock_volume_dev_release,
};

static int kfastblock_volume_add_disk(struct kfastblock_volume *vol,
				      struct bus_type *bus,
				      struct device *parent_dev)
{
	int ret;

	memset(&vol->tag_set, 0, sizeof(vol->tag_set));
	vol->tag_set.ops = &kfastblock_mq_ops;
	vol->tag_set.queue_depth = KFASTBLOCK_DEFAULT_QUEUE_DEPTH;
	vol->tag_set.numa_node = NUMA_NO_NODE;
	vol->tag_set.nr_hw_queues = 1;
	vol->tag_set.cmd_size = sizeof(struct kfastblock_request);
	vol->tag_set.driver_data = vol;
	vol->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;

	ret = blk_mq_alloc_tag_set(&vol->tag_set);
	if (ret)
		return ret;

	vol->dev_id = ida_alloc_range(&g_kfastblock_dev_ida, 0,
				      (1 << (MINORBITS -
				       KFASTBLOCK_DEVICE_PART_SHIFT)) - 1,
				      GFP_KERNEL);
	if (vol->dev_id < 0)
		return vol->dev_id;

	vol->minor = vol->dev_id << KFASTBLOCK_DEVICE_PART_SHIFT;
	vol->disk = blk_mq_alloc_disk(&vol->tag_set, vol);
	if (!vol->disk)
		return -ENOMEM;

	vol->disk->private_data = vol;
	vol->disk->major = vol->major;
	vol->disk->first_minor = vol->minor;
	vol->disk->fops = &kfastblock_bd_ops;
	vol->disk->minors = 1 << KFASTBLOCK_DEVICE_PART_SHIFT;
	snprintf(vol->disk->disk_name, sizeof(vol->disk->disk_name), "%s%d",
		 KFASTBLOCK_DRV_NAME_PREFIX, vol->dev_id);
	set_capacity(vol->disk, vol->view.image.size_bytes >> SECTOR_SHIFT);
	if (vol->view.image.block_size)
		blk_queue_logical_block_size(vol->disk->queue,
					     vol->view.image.block_size);
	if (vol->view.image.object_size) {
		blk_queue_io_opt(vol->disk->queue, vol->view.image.object_size);
		blk_queue_chunk_sectors(vol->disk->queue,
					vol->view.image.object_size >>
					SECTOR_SHIFT);
	}
	blk_queue_max_hw_sectors(vol->disk->queue,
				 KFASTBLOCK_MAX_IO_BYTES >> SECTOR_SHIFT);
	blk_queue_max_segments(vol->disk->queue, USHRT_MAX);
	blk_queue_max_segment_size(vol->disk->queue, KFASTBLOCK_MAX_IO_BYTES);
	vol->disk->queue->queuedata = vol;

	device_initialize(&vol->dev);
	vol->dev.parent = parent_dev;
	vol->dev.bus = bus;
	vol->dev.type = &kfastblock_volume_type;
	dev_set_name(&vol->dev, "%s%s-%s",
		     KFASTBLOCK_SYSFS_DEV_PREFIX,
		     vol->view.image.pool_name,
		     vol->view.image.image_name);

	ret = device_add(&vol->dev);
	if (ret)
		return ret;

	ret = device_add_disk(&vol->dev, vol->disk, NULL);
	if (ret) {
		device_del(&vol->dev);
		return ret;
	}

	return 0;
}

int kfastblock_volume_attach(const struct kfastblock_attach_spec *spec, int major,
			     struct bus_type *bus, struct device *parent_dev)
{
	struct kfastblock_volume *vol;
	int ret;

	if (!spec || !bus || !parent_dev)
		return -EINVAL;

	vol = kzalloc(sizeof(*vol), GFP_KERNEL);
	if (!vol)
		return -ENOMEM;

	vol->dev_id = -1;
	vol->major = major;
	atomic_set(&vol->open_count, 0);
	atomic_set(&vol->ready, 0);
	mutex_init(&vol->inflight_lock);
	INIT_LIST_HEAD(&vol->node);

	ret = kfastblock_meta_bootstrap(&vol->view, spec);
	if (ret)
		goto err_free;

	if (!kfastblock_meta_ready(&vol->view)) {
		ret = -EOPNOTSUPP;
		goto err_free;
	}

	ret = kfastblock_volume_add_disk(vol, bus, parent_dev);
	if (ret)
		goto err_free;

	mutex_lock(&g_kfastblock_volumes_lock);
	list_add_tail(&vol->node, &g_kfastblock_volumes);
	mutex_unlock(&g_kfastblock_volumes_lock);

	atomic_set(&vol->ready, 1);
	return 0;

err_free:
	kfree(vol);
	return ret;
}

int kfastblock_volume_detach(const struct kfastblock_attach_spec *spec)
{
	struct kfastblock_volume *vol;
	struct kfastblock_volume *found = NULL;

	if (!spec)
		return -EINVAL;

	mutex_lock(&g_kfastblock_volumes_lock);
	list_for_each_entry(vol, &g_kfastblock_volumes, node) {
		if (strcmp(vol->view.image.pool_name, spec->pool_name))
			continue;
		if (strcmp(vol->view.image.image_name, spec->image_name))
			continue;
		found = vol;
		break;
	}
	mutex_unlock(&g_kfastblock_volumes_lock);

	if (!found)
		return -ENOENT;

	atomic_set(&found->ready, 0);
	if (found->disk) {
		del_gendisk(found->disk);
		put_disk(found->disk);
		found->disk = NULL;
	}
	device_unregister(&found->dev);
	return 0;
}

int kfastblock_volume_init(void)
{
	ida_init(&g_kfastblock_dev_ida);
	return 0;
}

void kfastblock_volume_exit(void)
{
	struct kfastblock_volume *vol;
	struct kfastblock_volume *tmp;

	mutex_lock(&g_kfastblock_volumes_lock);
	list_for_each_entry_safe(vol, tmp, &g_kfastblock_volumes, node) {
		list_del_init(&vol->node);
		atomic_set(&vol->ready, 0);
		if (vol->disk) {
			del_gendisk(vol->disk);
			put_disk(vol->disk);
			vol->disk = NULL;
		}
		device_unregister(&vol->dev);
	}
	mutex_unlock(&g_kfastblock_volumes_lock);

	ida_destroy(&g_kfastblock_dev_ida);
}
