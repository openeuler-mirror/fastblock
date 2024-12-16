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

#ifndef LIBFBLOCK_C_H
#define LIBFBLOCK_C_H

#include <stdint.h>
#include <spdk/bdev_module.h>

#ifdef __cplusplus
extern "C" {
#endif

void libfblock_create_context(char*);

void libfblock_create_sharded();
void libfblock_destroy_sharded();
bool libfblock_created_sharded();

void libfblock_create_image(char*, char*, uint64_t, uint64_t);
void libfblock_remove_image(char*, char*);
void libfblock_resize_image(char*, char*, uint64_t);

void libfblock_write_block(
  uint64_t,
  char*,
  uint64_t,
  uint64_t,
  struct spdk_bdev_io*,
  void(*)(struct spdk_bdev_io*, int32_t));

void libfblock_read_block(
  uint64_t,
  char*,
  uint64_t,
  uint64_t,
  struct spdk_bdev_io*,
  void(*)(struct spdk_bdev_io*, char*, uint64_t, int32_t));

#ifdef __cplusplus
}
#endif

#endif
