/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <sys/eventfd.h>
#include <sys/epoll.h>

#include <spdk/stdinc.h>
#include <spdk/env.h>
#include <spdk/bdev.h>
#include <spdk/thread.h>
#include <spdk/json.h>
#include <spdk/string.h>
#include <spdk/util.h>
#include <spdk/likely.h>
#include <spdk/bdev_module.h>
#include <spdk/log.h>

#include "bdev_fastblock.h"
#include "client/libfblock.h"
#include "global.h"

#define SPDK_FASTBLOCK_QUEUE_DEPTH 128
#define MAX_EVENTS_PER_POLL 128

static uint64_t future_id = 0;

static int bdev_fastblock_count = 0;

struct bdev_fastblock
{
	struct spdk_bdev disk;
	char *image_name;
	char *monitor_address;
	uint64_t pool_id;
	char *pool_name;
	uint64_t image_size;
	uint32_t block_size;
	uint64_t object_size;
	// fastblock_image_info_t info;
	TAILQ_ENTRY(bdev_fastblock)
	tailq;
	struct spdk_poller *reset_timer;
	struct spdk_bdev_io *reset_bdev_io;
};

struct bdev_fastblock_group_channel
{
	struct spdk_poller *poller;
	int epoll_fd;
};

struct bdev_fastblock_io_channel
{
	int pfd;
	// fastblock_image_t image;
	struct bdev_fastblock *disk;
	struct bdev_fastblock_group_channel *group_ch;
};

struct bdev_fastblock_io
{
	size_t total_len;
};

static void
bdev_fastblock_free(struct bdev_fastblock *fastblock)
{
	if (!fastblock)
	{
		return;
	}

	if (fastblock->disk.name)
		free(fastblock->disk.name);
	if (fastblock->image_name)
		free(fastblock->image_name);
	if (fastblock->monitor_address)
		free(fastblock->monitor_address);
	if (fastblock->pool_name)
		free(fastblock->pool_name);
	free(fastblock);
}

void bdev_fastblock_free_config(char **config)
{
	char **entry;

	if (config)
	{
		for (entry = config; *entry; entry++)
		{
			free(*entry);
		}
		free(config);
	}
}

char **
bdev_fastblock_dup_config(const char *const *config)
{
	size_t count;
	char **copy;

	if (!config)
	{
		return NULL;
	}
	for (count = 0; config[count]; count++)
	{
	}
	copy = (char **)calloc(count + 1, sizeof(*copy));
	if (!copy)
	{
		return NULL;
	}
	for (count = 0; config[count]; count++)
	{
		if (!(copy[count] = strdup(config[count])))
		{
			bdev_fastblock_free_config(copy);
			return NULL;
		}
	}
	return copy;
}

static int
bdev_create_image(struct bdev_fastblock *fastblock)
{
	if (fastblock->object_size > 0)
    {
        global::blk_client->create_image(
          fastblock->pool_name,
          fastblock->image_name,
		  fastblock->image_size,
          fastblock->object_size);
    }
	else
    {
        global::blk_client->create_image(
          fastblock->pool_name,
          fastblock->image_name,
		  fastblock->image_size);

    }

	return 0;
}

static void
bdev_delete_image(struct bdev_fastblock *fastblock)
{
	global::blk_client->remove_image(fastblock->pool_name, fastblock->image_name);
}

void bdev_resize_image(struct bdev_fastblock *fastblock, uint64_t new_size_in_byte)
{
	global::blk_client->resize_image(fastblock->pool_name, std::string(fastblock->image_name), new_size_in_byte);
}

static int bdev_fastblock_library_init(void);

static void bdev_fastblock_library_fini(void);

static int
bdev_fastblock_get_ctx_size(void)
{
	return sizeof(struct bdev_fastblock_io);
}

struct spdk_bdev_module fastblock_if = {
	.module_init = bdev_fastblock_library_init,
	.module_fini = bdev_fastblock_library_fini,
	.name = "fastblock",
	.get_ctx_size = bdev_fastblock_get_ctx_size,

};
SPDK_BDEV_MODULE_REGISTER(fastblock, &fastblock_if)

static int
bdev_fastblock_reset_timer(void *arg)
{
	struct bdev_fastblock *disk = (struct bdev_fastblock *)arg;

	spdk_bdev_io_complete(disk->reset_bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	spdk_poller_unregister(&disk->reset_timer);
	disk->reset_bdev_io = NULL;

	return SPDK_POLLER_BUSY;
}

static int
bdev_fastblock_reset(struct bdev_fastblock *disk, struct spdk_bdev_io *bdev_io)
{
	assert(disk->reset_bdev_io == NULL);
	disk->reset_bdev_io = bdev_io;
	disk->reset_timer = SPDK_POLLER_REGISTER(bdev_fastblock_reset_timer, disk, 1 * 1000 * 1000);

	return 0;
}

static int
bdev_fastblock_destruct(void *ctx)
{
	struct bdev_fastblock *fastblock = (struct bdev_fastblock *)ctx;

	spdk_io_device_unregister(fastblock, NULL);

	bdev_fastblock_free(fastblock);
	return 0;
}

/*
 * read 回调函数
 */
static void bdev_fastblock_read_callback(struct spdk_bdev_io *bdev_io, char* data, uint64_t len, int32_t res)
{
	if (res == errc::success)
	{
		if(!data){
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
			return;
		}
	    struct iovec *iovs = bdev_io->u.bdev.iovs;
	    int iovcnt = bdev_io->u.bdev.iovcnt;
		char *ptr = data;
        uint64_t length = 0;
		if (iovcnt == 1){
		    length = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	    }else{
		    for (int i = 0; i < iovcnt; i++)
		    {
			    length += iovs[i].iov_len;
		    }
	    }
		if(length != len){
			SPDK_ERRLOG_EX("length: %lu len: %lu\n", length, len);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		for (int i = 0; i < iovcnt; i++){
            memcpy(iovs[i].iov_base, ptr, iovs[i].iov_len);
			ptr += iovs[i].iov_len;
		}

		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}
	else
	{
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	};
	return;
}

/*
 * write 回调函数
 */
static void bdev_fastblock_write_callback(struct spdk_bdev_io *bdev_io, int32_t res)
{
	if (res == errc::success)
	{
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}
	else
	{
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	};
	return;
}

/*
 * bdev_fastblock_write
 * 写可以直接写
 * 返回 void
 */
static void
bdev_fastblock_write(struct spdk_bdev_io *bdev_io,
					 struct iovec *iovs,
					 int iovcnt,
					 uint64_t offset,
					 size_t len)
{

	uint64_t read_id = future_id++;
	struct bdev_fastblock *fastblock = (struct bdev_fastblock *)bdev_io->bdev->ctxt;

	size_t total_len = 0;
	for (int i = 0; i < iovcnt; i++)
	{
		total_len += iovs[i].iov_len;
	}
	bool aligned = !(total_len % 4096);
	global::blk_client->write(fastblock->pool_id,
								   std::string(fastblock->image_name),
								   bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
								   bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
								   bdev_io,
								   bdev_fastblock_write_callback);
}

/*
 * dev_fastblock_get_buf_cb 在这里调用fastblock的read
 * 返回 void
 */
static void
bdev_fastblock_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
						  bool success)
{
	if (!success)
	{
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	struct iovec *iovs = bdev_io->u.bdev.iovs;
	int iovcnt = bdev_io->u.bdev.iovcnt;

	struct bdev_fastblock *fastblock = (struct bdev_fastblock *)bdev_io->bdev->ctxt;

	uint64_t offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	uint64_t len = 0;
	if (iovcnt == 1)
	{
		len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	}
	else
	{
		for (int i = 0; i < iovcnt; i++)
		{
			len += iovs[i].iov_len;
		}
	}
	SPDK_INFOLOG_EX(libblk, "start read: offset:{%lu} iovs len:{%lu} total len:{%lu}\n", offset, len, bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);

	uint64_t read_id = future_id++;

	return (void)global::blk_client->read(fastblock->pool_id,
								  std::string(fastblock->image_name),
								  offset, len, bdev_io, bdev_fastblock_read_callback);
}

/*
 * 改成直接调用_bdev_fastblock_submit_request(bdev_io)
 * 返回 void
 */
static void _bdev_fastblock_submit_request(struct spdk_bdev_io *bdev_io)
{
	struct bdev_fastblock *fastblock = (struct bdev_fastblock *)bdev_io->bdev->ctxt;

	switch (bdev_io->type)
	{
	case SPDK_BDEV_IO_TYPE_READ:
		// return;
		spdk_bdev_io_get_buf(bdev_io, bdev_fastblock_get_buf_cb,
							 bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return;

	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_fastblock_write(bdev_io,
							 bdev_io->u.bdev.iovs,
							 bdev_io->u.bdev.iovcnt,
							 bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
							 bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return;

	case SPDK_BDEV_IO_TYPE_RESET:
		// return bdev_fastblock_reset((struct bdev_rbd *)bdev_io->bdev->ctxt,
		//		      bdev_io);

	default:
		return;
	}
	return;
}

/*
 * 改成直接调用_bdev_fastblock_submit_request(bdev_io)
 */
static void bdev_fastblock_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	_bdev_fastblock_submit_request(bdev_io);
}

static bool
bdev_fastblock_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type)
	{
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;

	default:
		return false;
	}
}

static void
bdev_fastblock_io_poll(struct bdev_fastblock_io_channel *ch)
{
}

static void
bdev_fastblock_free_channel(struct bdev_fastblock_io_channel *ch)
{
	if (!ch)
	{
		return;
	}

	if (ch->pfd >= 0)
	{
		close(ch->pfd);
	}

	if (ch->group_ch)
	{
		spdk_put_io_channel(spdk_io_channel_from_ctx(ch->group_ch));
	}
}

static void *
bdev_fastblock_handle(void *arg)
{
	struct bdev_fastblock_io_channel *ch = (struct bdev_fastblock_io_channel *)arg;
	void *ret = arg;

	return ret;
}

static int
bdev_fastblock_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_fastblock_io_channel *ch = (struct bdev_fastblock_io_channel *)ctx_buf;
	int ret;
	struct epoll_event event;

	ch->disk = (struct bdev_fastblock *)io_device;
	// ch->image = NULL;
	// ch->io_ctx = NULL;
	ch->pfd = -1;

	if (spdk_call_unaffinitized(bdev_fastblock_handle, ch) == NULL)
	{
		goto err;
	}

	ch->pfd = eventfd(0, EFD_NONBLOCK);
	if (ch->pfd < 0)
	{
		SPDK_ERRLOG_EX("Failed to get eventfd\n");
		goto err;
	}

	ch->group_ch = (struct bdev_fastblock_group_channel *)spdk_io_channel_get_ctx(spdk_get_io_channel(&fastblock_if));
	assert(ch->group_ch != NULL);
	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN;
	event.data.ptr = ch;

	ret = epoll_ctl(ch->group_ch->epoll_fd, EPOLL_CTL_ADD, ch->pfd, &event);
	if (ret < 0)
	{
		SPDK_ERRLOG_EX("Failed to add the fd of ch(%p) to the epoll group from group_ch=%p\n", ch,
					ch->group_ch);
		goto err;
	}

	return 0;

err:
	bdev_fastblock_free_channel(ch);
	return -1;
}

static void
bdev_fastblock_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_fastblock_io_channel *io_channel = (struct bdev_fastblock_io_channel *)ctx_buf;
	int rc;

	rc = epoll_ctl(io_channel->group_ch->epoll_fd, EPOLL_CTL_DEL,
				   io_channel->pfd, NULL);
	if (rc < 0)
	{
		SPDK_ERRLOG_EX("Failed to remove fd on io_channel=%p from the polling group=%p\n",
					io_channel, io_channel->group_ch);
	}

	bdev_fastblock_free_channel(io_channel);
}

static struct spdk_io_channel *
bdev_fastblock_get_io_channel(void *ctx)
{
	struct bdev_fastblock *fastblock_bdev = (struct bdev_fastblock *)ctx;

	return spdk_get_io_channel(fastblock_bdev);
}

static int
bdev_fastblock_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_fastblock *fastblock_bdev = (struct bdev_fastblock *)ctx;

	spdk_json_write_named_object_begin(w, "fastblock");

	spdk_json_write_named_uint64(w, "pool_id", fastblock_bdev->pool_id);

	spdk_json_write_named_string(w, "image_name", fastblock_bdev->image_name);

	/*
		if (fastblock_bdev->config) {
			char **entry = fastblock_bdev->config;

			spdk_json_write_named_object_begin(w, "config");
			while (*entry) {
				spdk_json_write_named_string(w, entry[0], entry[1]);
				entry += 2;
			}
			spdk_json_write_object_end(w);
		}
	*/

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_fastblock_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct bdev_fastblock *fastblock = (struct bdev_fastblock *)bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_fastblock_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint64(w, "pool_id", fastblock->pool_id);
	spdk_json_write_named_string(w, "pool_name", fastblock->pool_name);
	spdk_json_write_named_string(w, "image_name", fastblock->image_name);
	spdk_json_write_named_uint64(w, "image_size", fastblock->image_size);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	spdk_json_write_named_uint64(w, "object_size", fastblock->object_size);
	spdk_json_write_named_string(w, "monitor_address", fastblock->monitor_address);

	/*
		if (fastblock->config) {
			char **entry = fastblock->config;

			spdk_json_write_named_object_begin(w, "config");
			while (*entry) {
				spdk_json_write_named_string(w, entry[0], entry[1]);
				entry += 2;
			}
			spdk_json_write_object_end(w);
		}
	*/

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table fastblock_fn_table = {
	.destruct = bdev_fastblock_destruct,
	.submit_request = bdev_fastblock_submit_request,
	.io_type_supported = bdev_fastblock_io_type_supported,
	.get_io_channel = bdev_fastblock_get_io_channel,
	.dump_info_json = bdev_fastblock_dump_info_json,
	.write_config_json = bdev_fastblock_write_config_json,
};

int bdev_fastblock_create(struct spdk_bdev **bdev, const char *name,
						  uint64_t pool_id,
						  const char *pool_name,
						  const char *image_name,
						  uint64_t image_size,
						  uint32_t block_size,
						  uint64_t object_size,
						  const char *monitor_address)
{
	struct bdev_fastblock *fastblock = NULL;
	int ret;
	if (image_name == NULL)
	{
		return -EINVAL;
	}

	SPDK_DEBUGLOG_EX(bdev_fastblock, "bdev_fastblock_create 11\n");
	fastblock = (struct bdev_fastblock *)calloc(1, sizeof(struct bdev_fastblock));
	if (fastblock == NULL)
	{
		SPDK_ERRLOG_EX("Failed to allocate bdev_fastblock struct\n");
		return -ENOMEM;
	}

	fastblock->image_name = strdup(image_name);
	if (!fastblock->image_name)
	{
		bdev_fastblock_free(fastblock);
		return -ENOMEM;
	}
	fastblock->image_size = image_size;
	fastblock->pool_id = pool_id;
    fastblock->pool_name = ::strdup(pool_name);
	fastblock->block_size = block_size;
	if (object_size == 0)
		fastblock->object_size = default_object_size;
	else
		fastblock->object_size = object_size;

	fastblock->monitor_address = strdup(monitor_address);
	if (!fastblock->monitor_address)
	{
		bdev_fastblock_free(fastblock);
		return -ENOMEM;
	}
	SPDK_NOTICELOG_EX("image_name %s, monitor_address %s\n", fastblock->image_name, fastblock->monitor_address);

	ret = bdev_create_image(fastblock);
	if (ret != 0)
	{
		bdev_fastblock_free(fastblock);
		SPDK_ERRLOG_EX("Failed to init fastblock device\n");
		return ret;
	}

	SPDK_NOTICELOG_EX("after bdev_create_image\n");
	if (name)
	{
		fastblock->disk.name = strdup(name);
	}
	else
	{
		fastblock->disk.name = spdk_sprintf_alloc("Fastblock%d", bdev_fastblock_count);
	}
	if (!fastblock->disk.name)
	{
		bdev_fastblock_free(fastblock);
		return -ENOMEM;
	}
	fastblock->disk.product_name = (char*)"Fastblock Disk";
	bdev_fastblock_count++;

	fastblock->disk.write_cache = 0;
	fastblock->disk.blocklen = block_size;
	fastblock->disk.blockcnt = fastblock->image_size / fastblock->disk.blocklen;
	fastblock->disk.ctxt = fastblock;
	fastblock->disk.fn_table = &fastblock_fn_table;
	fastblock->disk.module = &fastblock_if;

	SPDK_NOTICELOG_EX("Add %s fastblock disk to lun\n", fastblock->disk.name);

	spdk_io_device_register(fastblock, bdev_fastblock_create_cb,
							bdev_fastblock_destroy_cb,
							sizeof(struct bdev_fastblock_io_channel),
							image_name);
	ret = spdk_bdev_register(&fastblock->disk);
	if (ret)
	{
		spdk_io_device_unregister(fastblock, NULL);
		bdev_fastblock_free(fastblock);
		return ret;
	}

	*bdev = &(fastblock->disk);

	return ret;
}

void bdev_fastblock_delete(struct spdk_bdev *bdev, spdk_delete_fastblock_complete cb_fn, void *cb_arg)
{
	if (!bdev || bdev->module != &fastblock_if)
	{
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	bdev_delete_image((struct bdev_fastblock *)bdev->ctxt);
	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

int bdev_fastblock_resize(struct spdk_bdev *bdev, const uint64_t new_size_in_mb)
{
	struct spdk_io_channel *ch;
	struct bdev_fastblock_io_channel *fastblock_io_ch;
	int rc;
	uint64_t new_size_in_byte;
	uint64_t current_size_in_mb;

	if (bdev->module != &fastblock_if)
	{
		return -EINVAL;
	}

	current_size_in_mb = bdev->blocklen * bdev->blockcnt / (1024 * 1024);
	if (current_size_in_mb > new_size_in_mb)
	{
		SPDK_ERRLOG_EX("The new bdev size must be lager than current bdev size.\n");
		return -EINVAL;
	}

	ch = bdev_fastblock_get_io_channel(bdev);
	fastblock_io_ch = (struct bdev_fastblock_io_channel *)spdk_io_channel_get_ctx(ch);
	new_size_in_byte = new_size_in_mb * 1024 * 1024;

	bdev_resize_image(fastblock_io_ch->disk, new_size_in_byte);

	rc = spdk_bdev_notify_blockcnt_change(bdev, new_size_in_byte / bdev->blocklen);
	if (rc != 0)
	{
		SPDK_ERRLOG_EX("failed to notify block cnt change.\n");
		return rc;
	}

	return rc;
}

static int
bdev_fastblock_group_poll(void *arg)
{
	struct bdev_fastblock_group_channel *group_ch = (struct bdev_fastblock_group_channel *)arg;
	struct epoll_event events[MAX_EVENTS_PER_POLL];
	int num_events, i;

	num_events = epoll_wait(group_ch->epoll_fd, events, MAX_EVENTS_PER_POLL, 0);

	if (num_events <= 0)
	{
		return SPDK_POLLER_IDLE;
	}

	for (i = 0; i < num_events; i++)
	{
		bdev_fastblock_io_poll((struct bdev_fastblock_io_channel *)events[i].data.ptr);
	}

	return SPDK_POLLER_BUSY;
}

static int
bdev_fastblock_group_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_fastblock_group_channel *ch = (struct bdev_fastblock_group_channel *)ctx_buf;

	ch->epoll_fd = epoll_create1(0);
	if (ch->epoll_fd < 0)
	{
		SPDK_ERRLOG_EX("Could not create epoll fd on io device=%p\n", io_device);
		return -1;
	}

	ch->poller = SPDK_POLLER_REGISTER(bdev_fastblock_group_poll, ch, 0);

	return 0;
}

static void
bdev_fastblock_group_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_fastblock_group_channel *ch = (struct bdev_fastblock_group_channel *)ctx_buf;

	if (ch->epoll_fd >= 0)
	{
		close(ch->epoll_fd);
	}

	spdk_poller_unregister(&ch->poller);
}

static int
bdev_fastblock_library_init(void)
{
	spdk_io_device_register(&fastblock_if, bdev_fastblock_group_create_cb, bdev_fastblock_group_destroy_cb,
							sizeof(struct bdev_fastblock_group_channel), "bdev_fastblock_poll_groups");

	return 0;
}

static void bdev_fastblock_library_fini(void)
{
	spdk_io_device_unregister(&fastblock_if, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_fastblock)

uint64_t get_obj_size_of_image(struct spdk_bdev *bdev)
{
	if (!bdev || bdev->module != &fastblock_if)
	{
		return 0;
	}

	struct bdev_fastblock *fastblock = (struct bdev_fastblock *)bdev->ctxt;
	return fastblock->object_size;
}

uint64_t get_image_size(struct spdk_bdev *bdev)
{
	if (!bdev || bdev->module != &fastblock_if)
	{
		return 0;
	}

	struct bdev_fastblock *fastblock = (struct bdev_fastblock *)bdev->ctxt;
	return fastblock->image_size;
}