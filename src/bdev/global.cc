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

#include "fastblock/bdev/global.h"

namespace global {
std::shared_ptr<msg::rdma::client::options> rpc_cli_opts;
std::shared_ptr<connect_cache> conn_cache;
std::unique_ptr<monitor::client> mon_client;
std::shared_ptr<::libblk_client> blk_client;
std::vector<std::shared_ptr<::libblk_client>> blk_clients;
std::vector<::spdk_thread*> vhost_worker_threads;
uint32_t  app_thread_shard_id;
}
