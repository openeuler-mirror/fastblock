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

#include "msg/rpc_controller.h"
#include "rpc/osd_msg.pb.h"
#include "rpc/connect_cache.h"
#include "fb_client.h"
#include "monclient/client.h"

#include <google/protobuf/stubs/callback.h>

#include <vector>

constexpr size_t KiB = 1024;
constexpr size_t MiB = 1024 * KiB;
constexpr size_t GiB = 1024 * MiB;

constexpr size_t operator"" _KiB(unsigned long long val) { return val * KiB; }
constexpr size_t operator"" _MiB(unsigned long long val) { return val * MiB; }
constexpr size_t operator"" _GiB(unsigned long long val) { return val * GiB; }

constexpr size_t default_object_size = 4_MiB;

enum errc
{
    success = 0, // must be 0
    image_not_exist,
    image_already_exist,
    etcd_cmd_failed,
    etcd_image_format_invalid,
    size_is_less_than_current_size,
    put_image_state_flag_failed,
    image_in_deleting,
    not_supported,
    closed,
    invalid_read_data_size,
    invalid_write_data_size
};

static monitor::client::endpoint parse_endpoint(const char *address)
{
    auto add_str = std::string(address);
    auto p = add_str.find(':');
    monitor::client::endpoint ep;
    ep.host = add_str.substr(0, p);
    auto port = stoi(add_str.substr(p + 1, add_str.length() - p - 1));
    return ep;
}

class libblk_client
{
public:

    libblk_client(monitor::client* cli, auto&&... args)
      : _client{std::make_unique<fblock_client>(cli, std::forward<decltype(args)>(args)...)}
      , _mon_cli{cli} {}

public:

    std::unique_ptr<fblock_client> _client{};

public:

    void start() {
        _client->start();
    }

    void stop() {
        _client->stop();
    }

    void create_image(
      const std::string pool_name,
      const std::string image_name,
      const size_t size,
      const size_t object_size = default_object_size);

    void open_image(const std::string pool_name, const std::string image_name);
    void remove_image(const std::string pool_name, const std::string image_name);
    void resize_image(const std::string pool_name, const std::string image_name, const size_t size);
    void get_image_info(const std::string pool_name, const std::string image_name);

    int write(
      const uint64_t pool_id,
      const std::string image_name,
      const uint64_t offset,
      uint64_t length,
      struct spdk_bdev_io *bdev_io,
      write_callback cb);

    int write(
      const uint64_t pool_id,
      const std::string image_name,
      const uint64_t offset,
      struct spdk_bdev_io *bdev_io,
      std::string& buf,
      write_callback cb);

    int read(
      const uint64_t pool_id,
      const std::string image_name,
      const uint64_t offset,
      const uint64_t length,
      struct spdk_bdev_io *bdev_io,
      read_callback cb);

    std::string calc_image_object_prefix(const uint64_t pool_id, const std::string &image_name);

    std::tuple<size_t, uint64_t, uint64_t>
    calc_first_object_position(const uint64_t offset, const uint64_t length, const size_t object_size);

    // TODO 添加 pg 到 master osd 的映射
    std::string get_image_object_name(std::string &prefix, uint64_t seq);

private:

    monitor::client* _mon_cli{nullptr};
};