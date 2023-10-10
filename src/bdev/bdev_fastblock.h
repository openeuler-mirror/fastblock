/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef _BDEV_FASTBLOCK_H__
#define _BDEV_FASTBLOCK_H__

#include <spdk/stdinc.h>
#include <spdk/bdev.h>

void bdev_fastblock_free_config(char **config);

typedef void (*spdk_delete_fastblock_complete)(void *cb_arg, int bdeverrno);

int bdev_fastblock_create(struct spdk_bdev **bdev, const char *name,
						  uint64_t pool_id,
						  const char *pool_name,
						  const char *image_name,
						  uint64_t image_size,
						  uint32_t block_size,
						  uint64_t object_size,
						  const char *monitor_address);

/**
 * Delete fastblock bdev.
 *
 * \param bdev Pointer to fastblock bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void bdev_fastblock_delete(struct spdk_bdev *bdev, spdk_delete_fastblock_complete cb_fn,
						   void *cb_arg);

/**
 * Resize fastblock bdev.
 *
 * \param bdev Pointer to fastblock bdev.
 * \param new_size_in_mb The new size in MiB for this bdev.
 */
int bdev_fastblock_resize(struct spdk_bdev *bdev, const uint64_t new_size_in_mb);

extern struct spdk_bdev_module fastblock_if;

uint64_t get_obj_size_of_image(struct spdk_bdev *bdev);

uint64_t get_image_size(struct spdk_bdev *bdev);

#endif
