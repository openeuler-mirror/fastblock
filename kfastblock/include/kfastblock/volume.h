#ifndef KFASTBLOCK_VOLUME_H
#define KFASTBLOCK_VOLUME_H

#include <linux/blk-mq.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "kfastblock/control.h"
#include "kfastblock/meta.h"

struct kfastblock_volume {
	int dev_id;
	int major;
	int minor;

	struct gendisk *disk;
	struct blk_mq_tag_set tag_set;

	atomic_t open_count;
	atomic_t ready;

	struct kfastblock_cluster_view view;

	struct list_head node;
	struct device dev;
	struct mutex inflight_lock;
};

int kfastblock_volume_init(void);
void kfastblock_volume_exit(void);
int kfastblock_volume_attach(const struct kfastblock_attach_spec *spec, int major,
			     struct bus_type *bus, struct device *parent_dev);
int kfastblock_volume_detach(const struct kfastblock_attach_spec *spec);

#endif
