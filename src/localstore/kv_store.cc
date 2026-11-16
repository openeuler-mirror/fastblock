/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "kv_store.h"

#include <cerrno>

SPDK_LOG_REGISTER_COMPONENT(kvlog)

void make_aos_kvstore(fastblock::aos::kv_engine* eng, make_kvs_complete cb_fn,
                      void* arg) {
  if (eng == nullptr) {
    if (cb_fn) {
      cb_fn(arg, nullptr, -EINVAL);
    }
    return;
  }
  auto* kvs = new kvstore(eng);
  kvs->start();
  if (cb_fn) {
    cb_fn(arg, kvs, 0);
  }
}
