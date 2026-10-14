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
#pragma once

#include <spdk/stdinc.h>
#include <spdk/bdev.h>

void bdev_fastblock_free_config(char **config);

typedef void (*spdk_delete_fastblock_complete)(void *cb_arg, int bdeverrno);
typedef void (*spdk_fastblock_qos_update_complete)(void *cb_arg, int status);

int bdev_fastblock_create(struct spdk_bdev **bdev, const char *name,
						  const char *pool_name,
						  const char *image_name,
						  uint64_t image_size,
						  uint32_t block_size,
						  uint64_t object_size,
						  const char *monitor_address,
						  uint64_t iops_limit = 0,
						  uint64_t bw_limit_mib_per_sec = 0);

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

int bdev_fastblock_update_qos(struct spdk_bdev *bdev, uint64_t iops_limit, uint64_t bw_limit_mib_per_sec,
							  spdk_fastblock_qos_update_complete cb_fn = nullptr, void *cb_arg = nullptr);
int bdev_fastblock_get_qos(struct spdk_bdev *bdev,
						   uint64_t *iops_limit,
						   uint64_t *bw_limit_mib_per_sec,
						   uint64_t *spdk_iops_limit,
						   uint64_t *spdk_bw_limit_mib_per_sec,
						   uint64_t *spdk_read_bw_limit_mib_per_sec,
						   uint64_t *spdk_write_bw_limit_mib_per_sec);

extern struct spdk_bdev_module fastblock_if;

uint64_t get_obj_size_of_image(struct spdk_bdev *bdev);

uint64_t get_image_size(struct spdk_bdev *bdev);
