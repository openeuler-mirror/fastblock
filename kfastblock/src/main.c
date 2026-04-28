#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include "kfastblock/common.h"
#include "kfastblock/control.h"
#include "kfastblock/meta.h"
#include "kfastblock/transport.h"
#include "kfastblock/volume.h"

static int kfastblock_major;

static void kfastblock_root_dev_release(struct device *dev)
{
}

static struct device kfastblock_root_dev = {
	.init_name = KFASTBLOCK_DRV_NAME,
	.release = kfastblock_root_dev_release,
};

static ssize_t attach_store(const struct bus_type *bus, const char *args,
			    size_t count);
static ssize_t detach_store(const struct bus_type *bus, const char *args,
			    size_t count);
static ssize_t last_error_show(const struct bus_type *bus, char *buf);

static BUS_ATTR_WO(attach);
static BUS_ATTR_WO(detach);
static BUS_ATTR_RO(last_error);

static struct attribute *kfastblock_bus_attrs[] = {
	&bus_attr_attach.attr,
	&bus_attr_detach.attr,
	&bus_attr_last_error.attr,
	NULL,
};

static const struct attribute_group kfastblock_bus_attr_group = {
	.attrs = kfastblock_bus_attrs,
};

static const struct attribute_group *kfastblock_bus_attr_groups[] = {
	&kfastblock_bus_attr_group,
	NULL,
};

static struct bus_type kfastblock_bus_type = {
	.name = KFASTBLOCK_DRV_NAME,
	.bus_groups = kfastblock_bus_attr_groups,
};

static ssize_t attach_store(const struct bus_type *bus, const char *args,
			    size_t count)
{
	int ret;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	ret = kfastblock_control_attach(args, count, kfastblock_major,
					&kfastblock_bus_type,
					&kfastblock_root_dev);
	if (ret) {
		module_put(THIS_MODULE);
		return ret;
	}

	return count;
}

static ssize_t detach_store(const struct bus_type *bus, const char *args,
			    size_t count)
{
	int ret = kfastblock_control_detach(args, count);

	if (ret)
		return ret;

	return count;
}

static ssize_t last_error_show(const struct bus_type *bus, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", kfastblock_control_last_error());
}

static int kfastblock_sysfs_init(void)
{
	int ret;

	ret = bus_register(&kfastblock_bus_type);
	if (ret)
		return ret;

	ret = device_register(&kfastblock_root_dev);
	if (ret) {
		bus_unregister(&kfastblock_bus_type);
		return ret;
	}

	return 0;
}

static void kfastblock_sysfs_exit(void)
{
	device_unregister(&kfastblock_root_dev);
	bus_unregister(&kfastblock_bus_type);
}

static int __init kfastblock_init(void)
{
	int ret;

	kfastblock_major = register_blkdev(0, KFASTBLOCK_DRV_NAME);
	if (kfastblock_major < 0)
		return kfastblock_major;

	ret = kfastblock_meta_init();
	if (ret)
		goto err_unreg_blkdev;

	ret = kfastblock_transport_init();
	if (ret)
		goto err_meta_exit;

	ret = kfastblock_volume_init();
	if (ret)
		goto err_transport_exit;

	ret = kfastblock_sysfs_init();
	if (ret)
		goto err_volume_exit;

	return 0;

err_volume_exit:
	kfastblock_volume_exit();
err_transport_exit:
	kfastblock_transport_exit();
err_meta_exit:
	kfastblock_meta_exit();
err_unreg_blkdev:
	unregister_blkdev(kfastblock_major, KFASTBLOCK_DRV_NAME);
	return ret;
}

static void __exit kfastblock_exit(void)
{
	kfastblock_sysfs_exit();
	kfastblock_volume_exit();
	kfastblock_transport_exit();
	kfastblock_meta_exit();
	unregister_blkdev(kfastblock_major, KFASTBLOCK_DRV_NAME);
}

module_init(kfastblock_init);
module_exit(kfastblock_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("kfastblock kernel client scaffold");
