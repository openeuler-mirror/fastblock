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

#include "fastblock/client/libfblock.h"
#include "fastblock/client/libfblock_c.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

extern "C" void libfblock_create_context(char* json_conf) {
    boost::property_tree::ptree g_pt{};
    boost::property_tree::read_json(std::string(json_conf), g_pt);
    ::libblk_client_create_context(g_pt);
}

extern "C" void libfblock_create_sharded() {
    auto hold_index = utils::get_current_shard_id();
    auto* blk_ctx = ::libblk_client_get_context();
    auto* current_thread = ::spdk_get_thread();
    blk_ctx->vhost_worker_threads.at(hold_index) = current_thread;
    auto blk_cli = std::make_shared<::libblk_client>(blk_ctx->mon_client.get(), current_thread, blk_ctx->rpc_cli_opts);
    auto* blk_cli_ptr = blk_cli.get();
    blk_ctx->blk_clients.at(hold_index) = std::move(blk_cli);
    blk_cli_ptr->start([]() {
        SPDK_NOTICELOG("libblk_client start\n");
    });
}

extern "C" void libfblock_destroy_sharded() {
    auto* blk_ctx = ::libblk_client_get_context();
    auto hold_index = utils::get_current_shard_id();
    auto blk_cli = std::move(blk_ctx->blk_clients.at(hold_index));
    auto blk_cli_ptr = blk_cli.get();
    blk_cli_ptr->stop([blk_cli]() {
        SPDK_NOTICELOG("libblk_client stop\n");
    });
}

extern "C" bool libfblock_created_sharded() {
    auto* blk_ctx = ::libblk_client_get_context();
    auto hold_index = utils::get_current_shard_id();
    return !!(blk_ctx->blk_clients.at(hold_index));
}

extern "C" void
libfblock_create_image(char* pool_name, char* image_name, uint64_t image_size, uint64_t object_size) {
    auto* blk_ctx = ::libblk_client_get_context();
    blk_ctx->mon_client->emplace_create_image_request(
      pool_name,
      image_name,
      image_size,
      object_size,
      [] (const monitor::client::response_status s, [[maybe_unused]] auto _) {
          SPDK_INFOLOG(libblk, "create_image image status %d\n", s);
      }
    );
}

extern "C" void libfblock_remove_image(char* pool_name, char* image_name) {
    auto* blk_ctx = ::libblk_client_get_context();
    blk_ctx->mon_client->emplace_remove_image_request(
      pool_name,
      image_name,
      [] (const monitor::client::response_status s, [[maybe_unused]] auto _) {
          SPDK_INFOLOG(libblk, "remove_image image status %d\n", s);
      }
    );
}

extern "C" void libfblock_resize_image(char* pool_name, char* image_name, uint64_t new_size) {
    auto* blk_ctx = ::libblk_client_get_context();
    blk_ctx->mon_client->emplace_resize_image_request(
      pool_name,
      image_name,
      new_size,
      [] (const monitor::client::response_status s, [[maybe_unused]] auto _) {
          SPDK_INFOLOG(libblk, "resize_image image status %d\n", s);
      }
    );
}

extern "C" void libfblock_write_block(
  uint64_t pool_id,
  char* image_name,
  uint64_t offset,
  uint64_t length,
  struct spdk_bdev_io* bdev_io,
  void(*cb)(struct spdk_bdev_io*, int32_t)) {
    auto* blk_ctx = ::libblk_client_get_context();
    auto worker_index = utils::get_current_shard_id();
    SPDK_DEBUGLOG(libblk, "worker index: %d\n", worker_index);
    auto& blk_cli = blk_ctx->blk_clients.at(worker_index);

    blk_cli->write(
      pool_id,
      std::string(image_name),
      bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
      bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
      bdev_io,
      cb);
}

extern "C" void libfblock_read_block(
  uint64_t pool_id,
  char* image_name,
  uint64_t offset,
  uint64_t length,
  struct spdk_bdev_io* bdev_io,
  void(*cb)(struct spdk_bdev_io*, char*, uint64_t, int32_t)) {
    auto* blk_ctx = ::libblk_client_get_context();
    auto worker_index = utils::get_current_shard_id();
    SPDK_DEBUGLOG(libblk, "worker index: %d\n", worker_index);
    auto& blk_cli = blk_ctx->blk_clients.at(worker_index);

    blk_cli->read(
      pool_id,
      std::string(image_name),
      offset,
      length,
      bdev_io,
      cb);
}
