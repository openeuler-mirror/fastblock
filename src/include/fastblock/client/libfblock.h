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

#include "fastblock/msg/rpc_controller.h"
#include "fastblock/rpc/connect_cache.h"
#include "fastblock/client/fb_client.h"
#include "fastblock/monclient/client.h"

#include <optional>
#include <unordered_map>
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

    void start(auto&&... args) {
        _client->start(std::forward<decltype(args)>(args)...);
    }

    void stop(std::optional<std::function<void()>>&& cb = std::nullopt) {
        _client->stop(std::move(cb));
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
    void get_image_metadata_by_name(const std::string pool_name, const std::string image_name);
    void get_snapshot_metadata_by_id(const std::string snapshot_id);
    void get_snapshot_id_by_name(const std::string pool_name, const std::string image_name, const std::string snapshot_name);
    void create_image_snapshot(const std::string pool_name, const std::string image_name, const std::string snapshot_name);
    void create_clone_from_snapshot_name(const std::string pool_name, const std::string image_name, const std::string snapshot_name, const std::string clone_image_name);
    void create_clone_from_snapshot(const std::string snapshot_id, const std::string clone_image_name);
    void protect_snapshot_by_name(const std::string pool_name, const std::string image_name, const std::string snapshot_name);
    void protect_snapshot(const std::string snapshot_id);
    void unprotect_snapshot_by_name(const std::string pool_name, const std::string image_name, const std::string snapshot_name);
    void unprotect_snapshot(const std::string snapshot_id);
    void delete_image_snapshot_by_name(const std::string pool_name, const std::string image_name, const std::string snapshot_name);
    void delete_image_snapshot(const std::string snapshot_id);
    void finalize_flatten_image(const std::string image_id);
    void flatten_image(const std::string pool_name, const std::string image_name);

    fblock_client* data_client() {
      return _client.get();
    }

    monitor::client* monitor_client() {
      return _mon_cli;
    }

    void refresh_cached_image_metadata(const monitor::client::image_metadata& metadata) {
      warm_image_lineage_by_metadata(metadata);
    }

    std::vector<monitor::client::snapshot_metadata> get_fallback_chain(
      const std::optional<monitor::client::image_metadata>& image_metadata) const {
      return build_fallback_chain(image_metadata);
    }

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

    spdk_thread* get_blk_thread() {
      return _client->get_current_thread();
    }
private:

    static std::string make_image_cache_key(const int32_t pool_id, const std::string& image_name) {
      return std::to_string(pool_id) + "/" + image_name;
    }

    std::vector<monitor::client::snapshot_metadata> build_fallback_chain(
      const std::optional<monitor::client::image_metadata>& image_metadata) const;

    void warm_image_lineage_by_metadata(const monitor::client::image_metadata& metadata);
    void warm_image_lineage_by_id(const std::string& image_id);
    void warm_snapshot_lineage(const std::string& snapshot_id);

    void cache_image_metadata(const monitor::client::image_metadata& metadata) {
      _image_metadata_cache[make_image_cache_key(metadata.pool_id, metadata.image_name)] = metadata;
    }

    void cache_snapshot_metadata(const monitor::client::snapshot_metadata& metadata) {
      _snapshot_metadata_cache[metadata.snapshot_id] = metadata;
    }

    std::optional<monitor::client::image_metadata> find_cached_image_metadata(const int32_t pool_id, const std::string& image_name) const {
      auto it = _image_metadata_cache.find(make_image_cache_key(pool_id, image_name));
      if (it == _image_metadata_cache.end()) {
        return std::nullopt;
      }
      return it->second;
    }

    std::optional<monitor::client::snapshot_metadata> find_cached_snapshot_metadata(const std::string& snapshot_id) const {
      auto it = _snapshot_metadata_cache.find(snapshot_id);
      if (it == _snapshot_metadata_cache.end()) {
        return std::nullopt;
      }
      return it->second;
    }

    bool is_image_lineage_ready(const std::optional<monitor::client::image_metadata>& image_metadata) const;

    monitor::client* _mon_cli{nullptr};
    std::unordered_map<std::string, monitor::client::image_metadata> _image_metadata_cache{};
    std::unordered_map<std::string, monitor::client::snapshot_metadata> _snapshot_metadata_cache{};
};
