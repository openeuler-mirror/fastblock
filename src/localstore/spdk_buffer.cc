/* Copyright (c) 2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "spdk_buffer.h"
#include "buffer_pool.h"

buffer_list make_buffer_list(size_t n) {
  buffer_list bl;
  for (size_t i = 0; i < n; i++) {
    bl.append_buffer(buffer_pool_get());
  }
  return bl;
};

void free_buffer_list(buffer_list& bl) {
  while(!bl.empty()) {
    auto sbuf = bl.front();
    buffer_pool_put(sbuf);
    bl.pop_front();
  }
};