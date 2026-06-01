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

SPDK_LOG_REGISTER_COMPONENT(rollback_proof)

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
constexpr uint64_t io_size = 4096;

static char* g_conf_path{nullptr};
static std::string g_pool_name{"fb"};
static std::string g_image_name{"rollback-proof-base"};
static std::string g_snapshot_name{"snap-proof"};

enum class phase {
    init = 0,
    write_seg0_a,
    write_seg0_b,
    write_seg1_c,
    rollback,
    read_seg0,
    read_seg1,
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
    int32_t pool_id{-1};
    std::string seg0_a{};
    std::string seg0_b{};
    std::string seg1_c{};
    phase current_phase{phase::init};
    std::atomic<bool> stopping{false};
    int rc{0};
};

static app_ctx g_ctx{};

struct thread_msg {
    std::function<void()> fn{};
};

static std::string make_pattern_block(const std::string& prefix)
{
    auto data = std::string(io_size, '\0');
    memcpy(data.data(), prefix.data(), prefix.size());
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

static void issue_write_seg0_a();
static void issue_write_seg0_b();
static void issue_write_seg1_c();
static void issue_read_seg0();
static void issue_read_seg1();
static void ensure_image_ready();

static void on_read_done(struct spdk_bdev_io*, char* buf, uint64_t len, int32_t state)
{
    if (state != err::E_SUCCESS) {
        fail_with_state("image read", state);
        return;
    }

    auto data = std::string(buf, buf + len);
    switch (g_ctx.current_phase) {
    case phase::read_seg0:
        if (data.rfind("@@SEG0@@A\n", 0) != 0) {
            SPDK_ERRLOG("SEG0 verify failed after rollback\n");
            app_stop_with_rc(EIO);
            return;
        }
        SPDK_NOTICELOG("SEG0 verify success after rollback\n");
        issue_read_seg1();
        return;
    case phase::read_seg1:
        if (std::any_of(data.begin(), data.end(), [](char ch) { return ch != '\0'; })) {
            SPDK_ERRLOG("SEG1 zero verify failed after rollback\n");
            app_stop_with_rc(EIO);
            return;
        }
        SPDK_NOTICELOG("SEG1 zero verify success after rollback\n");
        g_ctx.current_phase = phase::done;
        app_stop_with_rc(0);
        return;
    default:
        SPDK_ERRLOG("unexpected read callback phase %d\n", static_cast<int>(g_ctx.current_phase));
        app_stop_with_rc(EIO);
        return;
    }
}

static void issue_read_seg0()
{
    g_ctx.current_phase = phase::read_seg0;
    g_ctx.blk_client->read(g_ctx.pool_id, g_ctx.image_name, seg0_offset, io_size, nullptr, &on_read_done);
}

static void issue_read_seg1()
{
    g_ctx.current_phase = phase::read_seg1;
    g_ctx.blk_client->read(g_ctx.pool_id, g_ctx.image_name, seg1_offset, io_size, nullptr, &on_read_done);
}

static void create_snapshot_after_seg0_a()
{
    g_ctx.mon_client->emplace_create_image_snapshot_request(
      g_ctx.pool_name,
      g_ctx.image_name,
      g_ctx.snapshot_name,
      [](const monitor::client::response_status status, monitor::client::request_context* req_ctx)
      {
          if (status != monitor::client::response_status::ok) {
              SPDK_ERRLOG("create snapshot failed with status %d\n", status);
              app_stop_with_rc(EIO);
              return;
          }

          auto& metadata = std::get<std::unique_ptr<monitor::client::snapshot_metadata>>(req_ctx->response_data);
          if (!metadata) {
              SPDK_ERRLOG("create snapshot response missing metadata\n");
              app_stop_with_rc(EIO);
              return;
          }

          SPDK_NOTICELOG(
            "snapshot created image=%s snapshot=%s snap_seq=%lu snapshot_id=%s\n",
            g_ctx.image_name.c_str(),
            g_ctx.snapshot_name.c_str(),
            metadata->snap_seq,
            metadata->snapshot_id.c_str());

          run_on_blk_thread([snap_seq = metadata->snap_seq]() {
              g_ctx.blk_client->advance_cached_image_snap_seq(g_ctx.pool_id, g_ctx.image_name, snap_seq);
              issue_write_seg0_b();
          });
      });
}

static void on_write_done(struct spdk_bdev_io*, int32_t state)
{
    if (state != err::E_SUCCESS) {
        fail_with_state("image write", state);
        return;
    }

    switch (g_ctx.current_phase) {
    case phase::write_seg0_a:
        SPDK_NOTICELOG("wrote SEG0=A at offset 0\n");
        create_snapshot_after_seg0_a();
        return;
    case phase::write_seg0_b:
        SPDK_NOTICELOG("wrote SEG0=B at offset 0 after snapshot\n");
        issue_write_seg1_c();
        return;
    case phase::write_seg1_c:
        SPDK_NOTICELOG("wrote SEG1=C at offset %lu after snapshot\n", seg1_offset);
        g_ctx.current_phase = phase::rollback;
        g_ctx.blk_client->rollback_image_to_snapshot(
          g_ctx.pool_name,
          g_ctx.image_name,
          g_ctx.snapshot_name,
          [](int32_t rollback_state)
          {
              if (rollback_state != err::E_SUCCESS) {
                  fail_with_state("rollback", rollback_state);
                  return;
              }
              SPDK_NOTICELOG("rollback completed successfully\n");
              issue_read_seg0();
          });
        return;
    default:
        SPDK_ERRLOG("unexpected write callback phase %d\n", static_cast<int>(g_ctx.current_phase));
        app_stop_with_rc(EIO);
        return;
    }
}

static void issue_write_seg0_a()
{
    g_ctx.current_phase = phase::write_seg0_a;
    auto buf = g_ctx.seg0_a;
    g_ctx.blk_client->write(g_ctx.pool_id, g_ctx.image_name, seg0_offset, nullptr, buf, &on_write_done);
}

static void issue_write_seg0_b()
{
    g_ctx.current_phase = phase::write_seg0_b;
    auto buf = g_ctx.seg0_b;
    g_ctx.blk_client->write(g_ctx.pool_id, g_ctx.image_name, seg0_offset, nullptr, buf, &on_write_done);
}

static void issue_write_seg1_c()
{
    g_ctx.current_phase = phase::write_seg1_c;
    auto buf = g_ctx.seg1_c;
    g_ctx.blk_client->write(g_ctx.pool_id, g_ctx.image_name, seg1_offset, nullptr, buf, &on_write_done);
}

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
              issue_write_seg0_a();
          });
      });
}

static void start_worker()
{
    spdk_cpuset cpumask{};
    spdk_cpuset_zero(&cpumask);
    auto current_core = spdk_env_get_current_core();
    spdk_cpuset_set_cpu(&cpumask, current_core, true);

    g_ctx.blk_thread = spdk_thread_create("rollback_proof_blk", &cpumask);
    auto opts = msg::rdma::client::make_options(g_ctx.pt);
    g_ctx.blk_client = std::make_unique<libblk_client>(g_ctx.mon_client.get(), g_ctx.blk_thread, opts);
    g_ctx.blk_client->start([]() {
        SPDK_NOTICELOG("rollback_proof block client started\n");
        ensure_image_ready();
    });
}

static void on_app_start(void*)
{
    if (!g_conf_path) {
        throw std::invalid_argument("config file is required");
    }

    boost::property_tree::read_json(std::string(g_conf_path), g_ctx.pt);
    g_ctx.seg0_a = make_pattern_block("@@SEG0@@A\n");
    g_ctx.seg0_b = make_pattern_block("@@SEG0@@B\n");
    g_ctx.seg1_c = make_pattern_block("@@SEG1@@C\n");

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
    printf("   FB_ROLLBACK_POOL        pool name, default fb\n");
    printf("   FB_ROLLBACK_IMAGE       image name, default rollback-proof-base\n");
    printf("   FB_ROLLBACK_SNAPSHOT    snapshot name, default snap-proof\n");
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
    opts.name = "rollback_proof";
    opts.num_entries = 0;

    auto rc = spdk_app_parse_args(argc, argv, &opts, "C:", nullptr, parse_arg, usage);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    if (const char* pool = std::getenv("FB_ROLLBACK_POOL")) {
        g_pool_name = pool;
        g_ctx.pool_name = pool;
    }
    if (const char* image = std::getenv("FB_ROLLBACK_IMAGE")) {
        g_image_name = image;
        g_ctx.image_name = image;
    }
    if (const char* snapshot = std::getenv("FB_ROLLBACK_SNAPSHOT")) {
        g_snapshot_name = snapshot;
        g_ctx.snapshot_name = snapshot;
    }

    return spdk_app_start(&opts, on_app_start, nullptr);
}
