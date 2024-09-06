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

#include "global.h"

// namespace global {
// std::shared_ptr<connect_cache> conn_cache;
// std::shared_ptr<::partition_manager> par_mgr;
// std::unique_ptr<monitor::client> mon_client;
// std::shared_ptr<::libblk_client> blk_client;
// }

namespace fastblock {
namespace global {
std::shared_ptr<connect_cache> conn_cache;
std::shared_ptr<::partition_manager> par_mgr;
std::unique_ptr<monitor::client> mon_client;
std::unique_ptr<client::pg_client> pg_cli;
std::unique_ptr<client::connection_pool> conn_pool;
std::unique_ptr<client::bdev_block_client> bdev_blk_cli;
std::unique_ptr<client::block_client_pool> blk_cli_pool;
}
}
