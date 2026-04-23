#ifndef KFASTBLOCK_CONTROL_H
#define KFASTBLOCK_CONTROL_H

#include <linux/device.h>
#include <linux/types.h>

#include "kfastblock/common.h"

struct kfastblock_monitor_endpoint {
	char host[KFASTBLOCK_MAX_ADDR_LEN];
	u16 port;
};

struct kfastblock_attach_spec {
	char *monitor_addr;
	char *pool_name;
	char *image_name;
	char *token;
	char *snapshot_name;
	bool read_only;
	u64 debug_size_bytes;
	u32 debug_object_size;
	u32 debug_pool_id;
	u32 debug_pg_count;
	u32 nr_monitors;
	struct kfastblock_monitor_endpoint monitors[KFASTBLOCK_MAX_MONITORS];
};

int kfastblock_control_attach(const char *args, size_t count, int major,
			      struct bus_type *bus, struct device *parent_dev);
int kfastblock_control_detach(const char *args, size_t count);
void kfastblock_control_cleanup_attach_spec(struct kfastblock_attach_spec *spec);
const char *kfastblock_control_last_error(void);

#endif
