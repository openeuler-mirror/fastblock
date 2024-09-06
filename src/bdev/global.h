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

#include "monclient/client.h"
#include "osd/partition_manager.h"
#include "client/pg_client.h"
#include "client/bdev_block_client.h"
#include "client/fb_client.h"

// namespace global {
// extern std::shared_ptr<connect_cache> conn_cache;
// extern std::shared_ptr<::partition_manager> par_mgr;
// extern std::unique_ptr<monitor::client> mon_client;
// extern std::shared_ptr<::libblk_client> blk_client;
// }

namespace fastblock {
namespace global {
extern std::shared_ptr<connect_cache> conn_cache;
extern std::shared_ptr<::partition_manager> par_mgr;
extern std::unique_ptr<monitor::client> mon_client;
extern std::unique_ptr<client::pg_client> pg_cli;
extern std::unique_ptr<client::connection_pool> conn_pool;
extern std::unique_ptr<client::bdev_block_client> bdev_blk_cli;
extern std::unique_ptr<client::block_client_pool> blk_cli_pool;
}
}
