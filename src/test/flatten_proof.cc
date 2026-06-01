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
#include "fastblock/bdev/global.h"
#include "fastblock/monclient/client.h"
#include "fastblock/msg/rdma/client.h"
#include "fastblock/utils/err_num.h"
#include "fastblock/utils/utils.h"

#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/string.h>
#include <spdk/thread.h>
#include <spdk/bdev.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

SPDK_LOG_REGISTER_COMPONENT(flatten_proof)

namespace global {
std::shared_ptr<msg::rdma::client::options> rpc_cli_opts{};
std::shared_ptr<connect_cache> conn_cache{};
std::unique_ptr<monitor::client> mon_client{};
std::shared_ptr<::libblk_client> blk_client{};
std::vector<std::shared_ptr<::libblk_client>> blk_clients{};
std::vector<::spdk_thread*> vhost_worker_threads{};
uint32_t app_thread_shard_id{0};
} // namespace global

namespace {

constexpr uint64_t default_image_size = 16 * 1024 * 1024;
constexpr uint64_t seg0_offset = 0;
constexpr uint64_t seg1_offset = default_object_size;
constexpr uint64_t seg2_offset = default_object_size * 2;
constexpr uint64_t io_size = 4096;

static char* g_conf_path{nullptr};
static std::string g_pool_name{"fb"};
static std::string g_image_name{"flatten-proof-base"};
static std::string g_snapshot_name{"snap-flatten-proof"};
static std::string g_clone_image_name{"flatten-proof-clone"};

enum class phase {
    init = 0,
    write_base_seg0,
    write_base_seg1,
    write_base_seg2,
    create_snapshot,
    protect_snapshot,
    create_clone,
    warm_clone_lineage,
    write_clone_seg0,
    read_clone_before_flatten_seg0,
    read_clone_before_flatten_seg1,
    read_clone_before_flatten_seg2,
    flatten_clone,
    read_clone_after_flatten_seg0,
    read_clone_after_flatten_seg1,
    read_clone_after_flatten_seg2,
    verify_metadata,
    done,
};

struct app_ctx {
    boost::property_tree::ptree pt{};
    std::unique_ptr<monitor::client> mon_client{};
    std::unique_ptr<libblk_client> blk_client{};
    spdk_thread* blk_thread{nullptr};
    std::string pool_name{g_pool_name};
    std::string image_name{g_image_name};
    std::string snapshot_name{g_snapshot_name};
    std::string clone_image_name{g_clone_image_name};
    int32_t pool_id{-1};
    int32_t clone_pool_id{-1};
    std::string snapshot_id{};
    std::string clone_image_id{};
    std::string seg0_base{};   // @@BASE_0\n
    std::string seg1_base{};   // @@BASE_1\n
    std::string seg2_base{};   // @@BASE_2\n
    std::string seg0_clone{};  // @@CLONE0\n
    phase current_phase{phase::init};
    std::atomic<bool> stopping{false};
    int rc{0};
    uint32_t clone_retry_count{0};
};

constexpr uint32_t clone_retry_limit = 30;
constexpr uint64_t clone_retry_delay_us = 100'000;

static app_ctx g_ctx{};

struct thread_msg {
    std::function<void()> fn{};
};

static std::string make_pattern_block(const std::string& prefix, size_t size = io_size)
{
    auto data = std::string(size, '\0');
    memcpy(data.data(), prefix.data(), std::min(prefix.size(), size));
    return data;
}

static void app_stop_with_rc(int rc)
{
    g_ctx.rc = rc;
    if (g_ctx.stopping.exchange(true)) {
        return;
    }

    auto stop_monitor = [rc]() mutable {
        if (g_ctx.mon_client) {
            g_ctx.mon_client->stop([rc]() mutable {
                spdk_app_stop(rc);
            });
        } else {
            spdk_app_stop(rc);
        }
    };

    if (g_ctx.blk_client) {
        auto* blk_thread = g_ctx.blk_thread;
        g_ctx.blk_client->stop([blk_thread, stop_monitor = std::move(stop_monitor)]() mutable {
            if (blk_thread) {
                spdk_thread_exit(blk_thread);
            }
            stop_monitor();
        });
    } else {
        stop_monitor();
    }
}

static void fail_with_state(const char* what, int32_t state)
{
    SPDK_ERRLOG("%s failed: %s(%d)\n", what, err::string_status(state), state);
    app_stop_with_rc(EIO);
}

static void run_on_blk_thread(std::function<void()> fn)
{
    if (g_ctx.blk_thread == nullptr || spdk_get_thread() == g_ctx.blk_thread) {
        fn();
        return;
    }
    auto* msg = new thread_msg{.fn = std::move(fn)};
    spdk_thread_send_msg(
      g_ctx.blk_thread,
      [](void* arg) {
          auto* msg = reinterpret_cast<thread_msg*>(arg);
          auto fn = std::move(msg->fn);
          delete msg;
          fn();
      },
      msg);
}

// Forward declarations
static void issue_write_base_seg0();
static void issue_write_base_seg1();
static void issue_write_base_seg2();
static void issue_write_clone_seg0();
static void schedule_clone_write_retry();
static void issue_read_clone_before_flatten_seg0();
static void issue_read_clone_before_flatten_seg1();
static void issue_read_clone_before_flatten_seg2();
static void issue_flatten_clone();
static void issue_read_clone_after_flatten_seg0();
static void issue_read_clone_after_flatten_seg1();
static void issue_read_clone_after_flatten_seg2();
static void issue_verify_metadata();
static void ensure_image_ready();
static void on_snapshot_ready(const std::string& snapshot_id, uint64_t snap_seq);
static void on_clone_ready(const monitor::client::image_metadata& metadata);

// ---------- shared helpers for snapshot/clone flow ----------

static void on_clone_ready(const monitor::client::image_metadata& metadata)
{
    g_ctx.clone_pool_id = metadata.pool_id;
    g_ctx.clone_image_id = metadata.image_id;
    SPDK_NOTICELOG(
      "clone ready clone_image=%s pool_id=%d image_id=%s parent_snapshot_id=%s\n",
      metadata.image_name.c_str(),
      metadata.pool_id,
      metadata.image_id.c_str(),
      metadata.parent_snapshot_id.c_str());

    run_on_blk_thread([metadata]() {
        g_ctx.blk_client->refresh_cached_image_metadata(metadata);
        g_ctx.current_phase = phase::warm_clone_lineage;
        g_ctx.blk_client->open_image(g_ctx.pool_name, g_ctx.clone_image_name);
        issue_write_clone_seg0();
    });
}

static void issue_check_or_create_clone()
{
    g_ctx.current_phase = phase::create_clone;
    g_ctx.mon_client->emplace_get_image_metadata_by_name_request(
      g_ctx.pool_name,
      g_ctx.clone_image_name,
      [](const monitor::client::response_status check_status, monitor::client::request_context* check_ctx)
      {
          if (check_status == monitor::client::response_status::image_not_found) {
              // Clone doesn't exist, create it
              g_ctx.mon_client->emplace_create_clone_from_snapshot_request(
                g_ctx.snapshot_id,
                g_ctx.clone_image_name,
                [](const monitor::client::response_status create_status, monitor::client::request_context* create_ctx)
                {
                    if (create_status != monitor::client::response_status::ok) {
                        SPDK_ERRLOG("create clone failed with status %d\n", create_status);
                        app_stop_with_rc(EIO);
                        return;
                    }
                    auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(
                      create_ctx->response_data);
                    if (!metadata) {
                        SPDK_ERRLOG("create clone response missing metadata\n");
                        app_stop_with_rc(EIO);
                        return;
                    }
                    on_clone_ready(*metadata);
                });
              return;
          }

          if (check_status != monitor::client::response_status::ok) {
              SPDK_ERRLOG("check clone existence failed with status %d\n", check_status);
              app_stop_with_rc(EIO);
              return;
          }

          // Clone exists, use existing metadata
          SPDK_NOTICELOG("clone already exists, using existing metadata\n");
          auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(
            check_ctx->response_data);
          if (!metadata) {
              SPDK_ERRLOG("missing existing clone metadata\n");
              app_stop_with_rc(EIO);
              return;
          }
          on_clone_ready(*metadata);
      });
}

static void on_snapshot_ready(const std::string& snapshot_id, uint64_t snap_seq)
{
    g_ctx.snapshot_id = snapshot_id;
    SPDK_NOTICELOG(
      "snapshot ready image=%s snapshot=%s snap_seq=%lu snapshot_id=%s\n",
      g_ctx.image_name.c_str(),
      g_ctx.snapshot_name.c_str(),
      snap_seq,
      snapshot_id.c_str());

    run_on_blk_thread([snap_seq]() {
        g_ctx.blk_client->advance_cached_image_snap_seq(g_ctx.pool_id, g_ctx.image_name, snap_seq);
        g_ctx.current_phase = phase::protect_snapshot;
        g_ctx.mon_client->emplace_protect_snapshot_request(
          g_ctx.snapshot_id,
          [](const monitor::client::response_status protect_status, monitor::client::request_context*)
          {
              if (protect_status != monitor::client::response_status::ok) {
                  SPDK_ERRLOG("protect snapshot failed with status %d\n", protect_status);
                  app_stop_with_rc(EIO);
                  return;
              }
              SPDK_NOTICELOG("snapshot protected snapshot_id=%s\n", g_ctx.snapshot_id.c_str());
              issue_check_or_create_clone();
          });
    });
}

// ---------- read callback ----------

static void on_read_done(struct spdk_bdev_io*, char* buf, uint64_t len, int32_t state)
{
    if (state != err::E_SUCCESS) {
        fail_with_state("image read", state);
        return;
    }

    auto data = std::string(buf, buf + len);

    switch (g_ctx.current_phase) {
    case phase::read_clone_before_flatten_seg0:
        if (data.rfind("@@CLONE0\n", 0) != 0) {
            SPDK_ERRLOG("clone SEG0 verify failed before flatten (expected @@CLONE0\\n)\n");
            app_stop_with_rc(EIO);
            return;
        }
        SPDK_NOTICELOG("clone SEG0 verify success before flatten\n");
        issue_read_clone_before_flatten_seg1();
        return;

    case phase::read_clone_before_flatten_seg1:
        if (data.rfind("@@BASE_1\n", 0) != 0) {
            SPDK_ERRLOG("clone SEG1 verify failed before flatten (expected @@BASE_1\\n from parent)\n");
            app_stop_with_rc(EIO);
            return;
        }
        SPDK_NOTICELOG("clone SEG1 verify success before flatten (inherited from parent)\n");
        issue_read_clone_before_flatten_seg2();
        return;

    case phase::read_clone_before_flatten_seg2:
        if (data.rfind("@@BASE_2\n", 0) != 0) {
            SPDK_ERRLOG("clone SEG2 verify failed before flatten (expected @@BASE_2\\n from parent)\n");
            app_stop_with_rc(EIO);
            return;
        }
        SPDK_NOTICELOG("clone SEG2 verify success before flatten (inherited from parent)\n");
        issue_flatten_clone();
        return;

    case phase::read_clone_after_flatten_seg0:
        if (data.rfind("@@CLONE0\n", 0) != 0) {
            SPDK_ERRLOG("clone SEG0 verify failed after flatten (expected @@CLONE0\\n)\n");
            app_stop_with_rc(EIO);
            return;
        }
        SPDK_NOTICELOG("clone SEG0 verify success after flatten\n");
        issue_read_clone_after_flatten_seg1();
        return;

    case phase::read_clone_after_flatten_seg1:
        if (data.rfind("@@BASE_1\n", 0) != 0) {
            SPDK_ERRLOG("clone SEG1 verify failed after flatten (expected @@BASE_1\\n from parent)\n");
            app_stop_with_rc(EIO);
            return;
        }
        SPDK_NOTICELOG("clone SEG1 verify success after flatten (materialized from parent)\n");
        issue_read_clone_after_flatten_seg2();
        return;

    case phase::read_clone_after_flatten_seg2:
        if (data.rfind("@@BASE_2\n", 0) != 0) {
            SPDK_ERRLOG("clone SEG2 verify failed after flatten (expected @@BASE_2\\n from parent)\n");
            app_stop_with_rc(EIO);
            return;
        }
        SPDK_NOTICELOG("clone SEG2 verify success after flatten (materialized from parent)\n");
        issue_verify_metadata();
        return;

    default:
        SPDK_ERRLOG("unexpected read callback phase %d\n", static_cast<int>(g_ctx.current_phase));
        app_stop_with_rc(EIO);
        return;
    }
}

// ---------- issue read helpers ----------

static void issue_read_clone_before_flatten_seg0()
{
    g_ctx.current_phase = phase::read_clone_before_flatten_seg0;
    g_ctx.blk_client->read(g_ctx.clone_pool_id, g_ctx.clone_image_name, seg0_offset, io_size, nullptr, &on_read_done);
}

static void issue_read_clone_before_flatten_seg1()
{
    g_ctx.current_phase = phase::read_clone_before_flatten_seg1;
    g_ctx.blk_client->read(g_ctx.clone_pool_id, g_ctx.clone_image_name, seg1_offset, io_size, nullptr, &on_read_done);
}

static void issue_read_clone_before_flatten_seg2()
{
    g_ctx.current_phase = phase::read_clone_before_flatten_seg2;
    g_ctx.blk_client->read(g_ctx.clone_pool_id, g_ctx.clone_image_name, seg2_offset, io_size, nullptr, &on_read_done);
}

static void issue_read_clone_after_flatten_seg0()
{
    g_ctx.current_phase = phase::read_clone_after_flatten_seg0;
    g_ctx.blk_client->read(g_ctx.clone_pool_id, g_ctx.clone_image_name, seg0_offset, io_size, nullptr, &on_read_done);
}

static void issue_read_clone_after_flatten_seg1()
{
    g_ctx.current_phase = phase::read_clone_after_flatten_seg1;
    g_ctx.blk_client->read(g_ctx.clone_pool_id, g_ctx.clone_image_name, seg1_offset, io_size, nullptr, &on_read_done);
}

static void issue_read_clone_after_flatten_seg2()
{
    g_ctx.current_phase = phase::read_clone_after_flatten_seg2;
    g_ctx.blk_client->read(g_ctx.clone_pool_id, g_ctx.clone_image_name, seg2_offset, io_size, nullptr, &on_read_done);
}

// ---------- write callback ----------

static void on_base_write_done(struct spdk_bdev_io*, int32_t state)
{
    if (state != err::E_SUCCESS) {
        fail_with_state("base image write", state);
        return;
    }

    switch (g_ctx.current_phase) {
    case phase::write_base_seg0:
        SPDK_NOTICELOG("base SEG0 written\n");
        issue_write_base_seg1();
        return;
    case phase::write_base_seg1:
        SPDK_NOTICELOG("base SEG1 written\n");
        issue_write_base_seg2();
        return;
    case phase::write_base_seg2:
        SPDK_NOTICELOG("base SEG2 written\n");
        g_ctx.current_phase = phase::create_snapshot;
        g_ctx.mon_client->emplace_get_snapshot_id_by_name_request(
          g_ctx.pool_name,
          g_ctx.image_name,
          g_ctx.snapshot_name,
          [](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
          {
              if (status == monitor::client::response_status::image_not_found) {
                  // Snapshot does not exist, create it
                  g_ctx.mon_client->emplace_create_image_snapshot_request(
                    g_ctx.pool_name,
                    g_ctx.image_name,
                    g_ctx.snapshot_name,
                    [](const monitor::client::response_status create_status, monitor::client::request_context* create_req_ctx)
                    {
                        if (create_status != monitor::client::response_status::ok) {
                            SPDK_ERRLOG("create snapshot failed with status %d\n", create_status);
                            app_stop_with_rc(EIO);
                            return;
                        }
                        auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(
                          create_req_ctx->response_data);
                        if (!metadata) {
                            SPDK_ERRLOG("create snapshot response missing metadata\n");
                            app_stop_with_rc(EIO);
                            return;
                        }
                        on_snapshot_ready(metadata->snapshot_id, metadata->snap_seq);
                    });
                  return;
              }

              if (status != monitor::client::response_status::ok) {
                  SPDK_ERRLOG("check snapshot existence failed with status %d\n", status);
                  app_stop_with_rc(EIO);
                  return;
              }

              // Snapshot exists, get snapshot_id and fetch full metadata
              SPDK_NOTICELOG("snapshot already exists, fetching metadata\n");
              auto& snap_id_ptr = std::get<std::unique_ptr<std::string>>(req_ctx->response_data);
              if (!snap_id_ptr) {
                  SPDK_ERRLOG("missing snapshot id from lookup\n");
                  app_stop_with_rc(EIO);
                  return;
              }
              auto existing_snapshot_id = *snap_id_ptr;

              g_ctx.mon_client->emplace_get_snapshot_metadata_by_id_request(
                existing_snapshot_id,
                [existing_snapshot_id](const monitor::client::response_status get_status, monitor::client::request_context* get_req_ctx)
                {
                    if (get_status != monitor::client::response_status::ok) {
                        SPDK_ERRLOG("get existing snapshot metadata failed with status %d\n", get_status);
                        app_stop_with_rc(EIO);
                        return;
                    }
                    auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(
                      get_req_ctx->response_data);
                    if (!metadata) {
                        SPDK_ERRLOG("missing existing snapshot metadata\n");
                        app_stop_with_rc(EIO);
                        return;
                    }
                    on_snapshot_ready(existing_snapshot_id, metadata->snap_seq);
                });
          });
        return;
    default:
        SPDK_ERRLOG("unexpected base write callback phase %d\n", static_cast<int>(g_ctx.current_phase));
        app_stop_with_rc(EIO);
        return;
    }
}

static void on_clone_write_done(struct spdk_bdev_io*, int32_t state)
{
    if (state == err::E_BUSY) {
        SPDK_NOTICELOG(
          "clone write hit E_BUSY in phase %d, wait for lineage warmup and retry (%u/%u)\n",
          static_cast<int>(g_ctx.current_phase),
          g_ctx.clone_retry_count + 1,
          clone_retry_limit);
        schedule_clone_write_retry();
        return;
    }
    if (state != err::E_SUCCESS) {
        fail_with_state("clone image write", state);
        return;
    }
    g_ctx.clone_retry_count = 0;

    switch (g_ctx.current_phase) {
    case phase::write_clone_seg0:
        SPDK_NOTICELOG("clone SEG0 written\n");
        issue_read_clone_before_flatten_seg0();
        return;
    default:
        SPDK_ERRLOG("unexpected clone write callback phase %d\n", static_cast<int>(g_ctx.current_phase));
        app_stop_with_rc(EIO);
        return;
    }
}

// ---------- issue write helpers ----------

static void issue_write_base_seg0()
{
    g_ctx.current_phase = phase::write_base_seg0;
    auto buf = g_ctx.seg0_base;
    g_ctx.blk_client->write(g_ctx.pool_id, g_ctx.image_name, seg0_offset, nullptr, buf, &on_base_write_done);
}

static void issue_write_base_seg1()
{
    g_ctx.current_phase = phase::write_base_seg1;
    auto buf = g_ctx.seg1_base;
    g_ctx.blk_client->write(g_ctx.pool_id, g_ctx.image_name, seg1_offset, nullptr, buf, &on_base_write_done);
}

static void issue_write_base_seg2()
{
    g_ctx.current_phase = phase::write_base_seg2;
    auto buf = g_ctx.seg2_base;
    g_ctx.blk_client->write(g_ctx.pool_id, g_ctx.image_name, seg2_offset, nullptr, buf, &on_base_write_done);
}

static void issue_write_clone_seg0()
{
    g_ctx.current_phase = phase::write_clone_seg0;
    auto buf = g_ctx.seg0_clone;
    g_ctx.blk_client->write(g_ctx.clone_pool_id, g_ctx.clone_image_name, seg0_offset, nullptr, buf, &on_clone_write_done);
}

static void schedule_clone_write_retry()
{
    if (g_ctx.clone_retry_count >= clone_retry_limit) {
        SPDK_ERRLOG("clone write retry limit reached (%u), giving up\n", clone_retry_limit);
        app_stop_with_rc(ETIMEDOUT);
        return;
    }
    g_ctx.clone_retry_count++;
    spdk_thread_send_msg(
      g_ctx.blk_thread,
      [](void*) {
          spdk_delay_us(clone_retry_delay_us);
          issue_write_clone_seg0();
      },
      nullptr);
}

// ---------- flatten ----------

static void issue_flatten_clone()
{
    g_ctx.current_phase = phase::flatten_clone;
    SPDK_NOTICELOG("starting flatten for clone image %s\n", g_ctx.clone_image_name.c_str());

    run_on_blk_thread([]() {
        g_ctx.blk_client->flatten_image(
          g_ctx.pool_name,
          g_ctx.clone_image_name,
          [](int32_t state) {
              if (state != err::E_SUCCESS) {
                  SPDK_ERRLOG("flatten failed with state %d\n", state);
                  app_stop_with_rc(EIO);
                  return;
              }
              SPDK_NOTICELOG("flatten completed successfully\n");
              issue_read_clone_after_flatten_seg0();
          });
    });
}

// ---------- verify metadata ----------

static void issue_verify_metadata()
{
    g_ctx.current_phase = phase::verify_metadata;
    g_ctx.mon_client->emplace_get_image_metadata_by_name_request(
      g_ctx.pool_name,
      g_ctx.clone_image_name,
      [](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
      {
          if (status != monitor::client::response_status::ok) {
              SPDK_ERRLOG("get image metadata failed with status %d\n", status);
              app_stop_with_rc(EIO);
              return;
          }

          auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
          if (!metadata) {
              SPDK_ERRLOG("missing image metadata response\n");
              app_stop_with_rc(EIO);
              return;
          }

          if (!metadata->parent_snapshot_id.empty()) {
              SPDK_ERRLOG(
                "flatten failed: clone still has parent_snapshot_id=%s\n",
                metadata->parent_snapshot_id.c_str());
              app_stop_with_rc(EIO);
              return;
          }

          if (metadata->depth != 0) {
              SPDK_ERRLOG("flatten failed: clone depth=%u (expected 0)\n", metadata->depth);
              app_stop_with_rc(EIO);
              return;
          }

          SPDK_NOTICELOG(
            "flatten verify success: parent_snapshot_id is empty, depth=0\n");

          g_ctx.current_phase = phase::done;
          SPDK_NOTICELOG("flatten proof passed\n");
          app_stop_with_rc(0);
      });
}

// ---------- image setup ----------

static void create_image_then_retry()
{
    g_ctx.mon_client->emplace_create_image_request(
      g_ctx.pool_name,
      g_ctx.image_name,
      default_image_size,
      default_object_size,
      [](const monitor::client::response_status status, monitor::client::request_context*)
      {
          if (status != monitor::client::response_status::ok &&
              status != monitor::client::response_status::created_image_exists) {
              SPDK_ERRLOG("create image failed with status %d\n", status);
              app_stop_with_rc(EIO);
              return;
          }

          SPDK_NOTICELOG("image create completed, fetching metadata\n");
          ensure_image_ready();
      });
}

static void ensure_image_ready()
{
    g_ctx.mon_client->emplace_get_image_metadata_by_name_request(
      g_ctx.pool_name,
      g_ctx.image_name,
      [](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
      {
          if (status == monitor::client::response_status::image_not_found) {
              create_image_then_retry();
              return;
          }
          if (status != monitor::client::response_status::ok) {
              SPDK_ERRLOG("get image metadata failed with status %d\n", status);
              app_stop_with_rc(EIO);
              return;
          }

          auto& metadata = std::get<std::unique_ptr<monitor::client::image_metadata>>(req_ctx->response_data);
          if (!metadata) {
              SPDK_ERRLOG("missing image metadata response\n");
              app_stop_with_rc(EIO);
              return;
          }
          if (metadata->status != "ready") {
              SPDK_ERRLOG("image status is not ready: %s\n", metadata->status.c_str());
              app_stop_with_rc(EIO);
              return;
          }

          auto metadata_copy = *metadata;
          g_ctx.pool_id = metadata_copy.pool_id;
          SPDK_NOTICELOG(
            "image metadata ready image=%s pool_id=%d current_snap_seq=%lu\n",
            metadata_copy.image_name.c_str(),
            metadata_copy.pool_id,
            metadata_copy.current_snap_seq);

          run_on_blk_thread([metadata_copy]() {
              g_ctx.blk_client->refresh_cached_image_metadata(metadata_copy);
              issue_write_base_seg0();
          });
      });
}

static void start_worker()
{
    spdk_cpuset cpumask{};
    spdk_cpuset_zero(&cpumask);
    auto current_core = spdk_env_get_current_core();
    spdk_cpuset_set_cpu(&cpumask, current_core, true);

    g_ctx.blk_thread = spdk_thread_create("flatten_proof_blk", &cpumask);
    auto opts = msg::rdma::client::make_options(g_ctx.pt);
    g_ctx.blk_client = std::make_unique<libblk_client>(g_ctx.mon_client.get(), g_ctx.blk_thread, opts);
    g_ctx.blk_client->start([]() {
        SPDK_NOTICELOG("flatten_proof block client started\n");
        ensure_image_ready();
    });
}

static void on_app_start(void*)
{
    if (!g_conf_path) {
        throw std::invalid_argument("config file is required");
    }

    boost::property_tree::read_json(std::string(g_conf_path), g_ctx.pt);
    g_ctx.seg0_base = make_pattern_block("@@BASE_0\n");
    g_ctx.seg1_base = make_pattern_block("@@BASE_1\n");
    g_ctx.seg2_base = make_pattern_block("@@BASE_2\n");
    g_ctx.seg0_clone = make_pattern_block("@@CLONE0\n");

    std::vector<monitor::client::endpoint> endpoints{};
    for (auto& mon : g_ctx.pt.get_child("mon_host")) {
        endpoints.push_back({mon.second.get_value<std::string>(), utils::default_monitor_port});
    }

    monitor::client::on_cluster_map_initialized_type init_cb = []() {
        int32_t pool_id = -1;
        if (!g_ctx.mon_client->get_pool_id(g_ctx.pool_name, pool_id)) {
            SPDK_ERRLOG("pool %s does not exist\n", g_ctx.pool_name.c_str());
            app_stop_with_rc(ENOENT);
            return;
        }
        SPDK_NOTICELOG("cluster map ready for pool %s\n", g_ctx.pool_name.c_str());
        start_worker();
    };

    g_ctx.mon_client = std::make_unique<monitor::client>(endpoints, std::move(init_cb));
    g_ctx.mon_client->start();
    g_ctx.mon_client->start_cluster_map_poller();
}

static void usage()
{
    printf(" -C <config>               path to json config file\n");
    printf(" Environment variables:\n");
    printf("   FB_FLATTEN_POOL         pool name, default fb\n");
    printf("   FB_FLATTEN_IMAGE        base image name, default flatten-proof-base\n");
    printf("   FB_FLATTEN_SNAPSHOT     snapshot name, default snap-flatten-proof\n");
    printf("   FB_FLATTEN_CLONE        clone image name, default flatten-proof-clone\n");
}

static int parse_arg(int ch, char* arg)
{
    switch (ch) {
    case 'C':
        g_conf_path = arg;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    spdk_app_opts opts{};
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "flatten_proof";
    opts.num_entries = 0;

    auto rc = spdk_app_parse_args(argc, argv, &opts, "C:", nullptr, parse_arg, usage);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    if (const char* pool = std::getenv("FB_FLATTEN_POOL")) {
        g_pool_name = pool;
        g_ctx.pool_name = pool;
    }
    if (const char* image = std::getenv("FB_FLATTEN_IMAGE")) {
        g_image_name = image;
        g_ctx.image_name = image;
    }
    if (const char* snapshot = std::getenv("FB_FLATTEN_SNAPSHOT")) {
        g_snapshot_name = snapshot;
        g_ctx.snapshot_name = snapshot;
    }
    if (const char* clone = std::getenv("FB_FLATTEN_CLONE")) {
        g_clone_image_name = clone;
        g_ctx.clone_image_name = clone;
    }

    spdk_iobuf_opts iobuf_opts{};
    spdk_iobuf_get_opts(&iobuf_opts, sizeof(iobuf_opts));
    iobuf_opts.small_pool_count = 64;
    iobuf_opts.large_pool_count = 8;
    spdk_iobuf_set_opts(&iobuf_opts);

    spdk_bdev_opts bdev_opts{};
    spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
    bdev_opts.iobuf_small_cache_size = 0;
    bdev_opts.iobuf_large_cache_size = 0;
    spdk_bdev_set_opts(&bdev_opts);

    return spdk_app_start(&opts, on_app_start, nullptr);
}
