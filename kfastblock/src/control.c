#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kfastblock/common.h"
#include "kfastblock/control.h"
#include "kfastblock/volume.h"

static char g_kfastblock_last_error[KFASTBLOCK_LAST_ERROR_LEN] =
	"kfastblock scaffold not initialized";

static void kfastblock_set_last_error(const char *msg)
{
	strscpy(g_kfastblock_last_error, msg, sizeof(g_kfastblock_last_error));
}

static bool kfastblock_parse_bool(const char *value, bool *out)
{
	if (sysfs_streq(value, "1") || sysfs_streq(value, "true") ||
	    sysfs_streq(value, "yes")) {
		*out = true;
		return true;
	}

	if (sysfs_streq(value, "0") || sysfs_streq(value, "false") ||
	    sysfs_streq(value, "no")) {
		*out = false;
		return true;
	}

	return false;
}

static int kfastblock_dup_field(char **dst, const char *value)
{
	kfree(*dst);
	*dst = kstrdup(value, GFP_KERNEL);
	return *dst ? 0 : -ENOMEM;
}

static int kfastblock_parse_attach_spec(const char *args, size_t count,
					struct kfastblock_attach_spec *spec)
{
	char *cursor;
	char *scratch;
	char *token;
	int ret = 0;

	if (!args || !count || !spec)
		return -EINVAL;

	memset(spec, 0, sizeof(*spec));
	scratch = kmemdup_nul(args, count, GFP_KERNEL);
	if (!scratch)
		return -ENOMEM;

	cursor = scratch;
	while ((token = strsep(&cursor, " \t\r\n")) != NULL) {
		char *key;
		char *value;
		bool bool_val;

		if (!*token)
			continue;

		key = strsep(&token, "=");
		value = token;
		if (!key || !value || !*value) {
			ret = -EINVAL;
			goto out;
		}

		if (!strcmp(key, "monitor_addr")) {
			ret = kfastblock_dup_field(&spec->monitor_addr, value);
		} else if (!strcmp(key, "pool_name")) {
			ret = kfastblock_dup_field(&spec->pool_name, value);
		} else if (!strcmp(key, "image_name")) {
			ret = kfastblock_dup_field(&spec->image_name, value);
		} else if (!strcmp(key, "token")) {
			ret = kfastblock_dup_field(&spec->token, value);
		} else if (!strcmp(key, "snapshot_name")) {
			ret = kfastblock_dup_field(&spec->snapshot_name, value);
		} else if (!strcmp(key, "read_only")) {
			if (!kfastblock_parse_bool(value, &bool_val)) {
				ret = -EINVAL;
				goto out;
			}
			spec->read_only = bool_val;
		} else if (!strcmp(key, "debug_size_bytes")) {
			ret = kstrtou64(value, 10, &spec->debug_size_bytes);
		} else if (!strcmp(key, "debug_object_size")) {
			ret = kstrtou32(value, 10, &spec->debug_object_size);
		} else if (!strcmp(key, "debug_pool_id")) {
			ret = kstrtou32(value, 10, &spec->debug_pool_id);
		} else if (!strcmp(key, "debug_pg_count")) {
			ret = kstrtou32(value, 10, &spec->debug_pg_count);
		} else {
			ret = -EINVAL;
			goto out;
		}

		if (ret)
			goto out;
	}

	if (!spec->monitor_addr || !spec->pool_name || !spec->image_name) {
		ret = -EINVAL;
		goto out;
	}

out:
	kfree(scratch);
	if (ret)
		kfastblock_control_cleanup_attach_spec(spec);
	return ret;
}

int kfastblock_control_attach(const char *args, size_t count, int major,
			      struct bus_type *bus, struct device *parent_dev)
{
	struct kfastblock_attach_spec spec;
	int ret;

	ret = kfastblock_parse_attach_spec(args, count, &spec);
	if (ret) {
		kfastblock_set_last_error("invalid attach arguments");
		return ret;
	}

	ret = kfastblock_volume_attach(&spec, major, bus, parent_dev);
	if (ret == -EOPNOTSUPP) {
		kfastblock_set_last_error("monitor bootstrap and data transport are not implemented yet");
	} else if (ret) {
		kfastblock_set_last_error("failed to attach volume");
	} else {
		kfastblock_set_last_error("ok");
	}

	kfastblock_control_cleanup_attach_spec(&spec);
	return ret;
}

int kfastblock_control_detach(const char *args, size_t count)
{
	struct kfastblock_attach_spec spec;
	int ret;

	ret = kfastblock_parse_attach_spec(args, count, &spec);
	if (ret) {
		kfastblock_set_last_error("invalid detach arguments");
		return ret;
	}

	ret = kfastblock_volume_detach(&spec);
	if (ret) {
		kfastblock_set_last_error("failed to detach volume");
	} else {
		kfastblock_set_last_error("ok");
	}

	kfastblock_control_cleanup_attach_spec(&spec);
	return ret;
}

void kfastblock_control_cleanup_attach_spec(struct kfastblock_attach_spec *spec)
{
	if (!spec)
		return;

	kfree(spec->monitor_addr);
	kfree(spec->pool_name);
	kfree(spec->image_name);
	kfree(spec->token);
	kfree(spec->snapshot_name);
	memset(spec, 0, sizeof(*spec));
}

const char *kfastblock_control_last_error(void)
{
	return g_kfastblock_last_error;
}
