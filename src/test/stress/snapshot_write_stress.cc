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

#include "fastblock/client/fb_client.h"
#include "fastblock/monclient/client.h"
#include "fastblock/msg/rdma/client.h"
#include "fastblock/utils/utils.h"

#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/string.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

SPDK_LOG_REGISTER_COMPONENT(snapshot_stress)

namespace {

enum class run_mode {
    write = 1,
    read
};

static char* g_conf_path{nullptr};
static std::string g_pool_name{"fb"};
static uint32_t g_target_pg{0};
static uint32_t g_write_count{64};
static uint32_t g_io_size{4096};
static run_mode g_mode{run_mode::write};

struct request_ctx {
    uint64_t id{0};
};

struct app_ctx {
    boost::property_tree::ptree pt{};
    std::unique_ptr<monitor::client> mon_client{};
    std::unique_ptr<fblock_client> fb_client{};
    spdk_thread* blk_thread{nullptr};
    std::string pool_name{g_pool_name};
    int32_t pool_id{-1};
    uint32_t pool_pg_num{0};
    uint32_t target_pg{0};
    std::string object_name{};
    std::string payload{};
    std::atomic<bool> stopping{false};
    uint64_t next_request_id{0};
    uint32_t total_writes{0};
    uint32_t writes_done{0};
    int rc{0};
};

static app_ctx g_ctx{};

// jenkins hash function copied from fb_client.cc
#define mix(a, b, c)       \
    {                      \
        a = a - b;         \
        a = a - c;         \
        a = a ^ (c >> 13); \
        b = b - c;         \
        b = b - a;         \
        b = b ^ (a << 8);  \
        c = c - a;         \
        c = c - b;         \
        c = c ^ (b >> 13); \
        a = a - b;         \
        a = a - c;         \
        a = a ^ (c >> 12); \
        b = b - c;         \
        b = b - a;         \
        b = b ^ (a << 16); \
        c = c - a;         \
        c = c - b;         \
        c = c ^ (b >> 5);  \
        a = a - b;         \
        a = a - c;         \
        a = a ^ (c >> 3);  \
        b = b - c;         \
        b = b - a;         \
        b = b ^ (a << 10); \
        c = c - a;         \
        c = c - b;         \
        c = c ^ (b >> 15); \
    }

static unsigned jenkins_hash(const std::string& str, unsigned length)
{
    const unsigned char* k = reinterpret_cast<const unsigned char*>(str.c_str());
    uint32_t a, b, c;
    uint32_t len;

    len = length;
    a = 0x9e3779b9;
    b = a;
    c = 0;

    while (len >= 12) {
        a = a + (k[0] + (static_cast<uint32_t>(k[1]) << 8) + (static_cast<uint32_t>(k[2]) << 16) +
                 (static_cast<uint32_t>(k[3]) << 24));
        b = b + (k[4] + (static_cast<uint32_t>(k[5]) << 8) + (static_cast<uint32_t>(k[6]) << 16) +
                 (static_cast<uint32_t>(k[7]) << 24));
        c = c + (k[8] + (static_cast<uint32_t>(k[9]) << 8) + (static_cast<uint32_t>(k[10]) << 16) +
                 (static_cast<uint32_t>(k[11]) << 24));
        mix(a, b, c);
        k = k + 12;
        len = len - 12;
    }

    c = c + length;
    switch (len) {
    case 11:
        c = c + (static_cast<uint32_t>(k[10]) << 24);
        [[fallthrough]];
    case 10:
        c = c + (static_cast<uint32_t>(k[9]) << 16);
        [[fallthrough]];
    case 9:
        c = c + (static_cast<uint32_t>(k[8]) << 8);
        [[fallthrough]];
    case 8:
        b = b + (static_cast<uint32_t>(k[7]) << 24);
        [[fallthrough]];
    case 7:
        b = b + (static_cast<uint32_t>(k[6]) << 16);
        [[fallthrough]];
    case 6:
        b = b + (static_cast<uint32_t>(k[5]) << 8);
        [[fallthrough]];
    case 5:
        b = b + k[4];
        [[fallthrough]];
    case 4:
        a = a + (static_cast<uint32_t>(k[3]) << 24);
        [[fallthrough]];
    case 3:
        a = a + (static_cast<uint32_t>(k[2]) << 16);
        [[fallthrough]];
    case 2:
        a = a + (static_cast<uint32_t>(k[1]) << 8);
        [[fallthrough]];
    case 1:
        a = a + k[0];
        [[fallthrough]];
    default:
        break;
    }
    mix(a, b, c);

    return c;
}

template <class T>
static inline typename std::enable_if<
    (std::is_integral<T>::value && sizeof(T) <= sizeof(unsigned)),
    unsigned>::type
cbits(T v)
{
    if (v == 0) {
        return 0;
    }
    return (sizeof(v) * 8) - __builtin_clz(v);
}

static unsigned calc_target_pg(const std::string& object_name, uint32_t pg_num)
{
    const auto seed = jenkins_hash(object_name, object_name.size());
    const auto pg_mask = (1U << cbits(pg_num - 1)) - 1;
    (void)pg_mask;
    return seed % pg_num;
}

static std::string find_object_name_for_pg(uint32_t pg_num, uint32_t target_pg)
{
    for (uint32_t i = 0; i < 1000000; ++i) {
        auto object_name = std::string("snapshot-stress-") + std::to_string(i);
        if (calc_target_pg(object_name, pg_num) == target_pg) {
            return object_name;
        }
    }

    throw std::runtime_error("failed to find object name for target pg");
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

    if (g_ctx.fb_client) {
        auto* blk_thread = g_ctx.blk_thread;
        g_ctx.fb_client->stop([blk_thread, stop_monitor = std::move(stop_monitor)]() mutable {
            if (blk_thread) {
                spdk_thread_exit(blk_thread);
            }
            stop_monitor();
        });
    } else {
        stop_monitor();
    }
}

static void issue_next_write();

static void on_write_done(void* arg, int32_t state)
{
    auto* req = reinterpret_cast<request_ctx*>(arg);
    delete req;

    if (state != err::E_SUCCESS) {
        SPDK_ERRLOG("write object failed: %s(%d)\n", err::string_status(state), state);
        app_stop_with_rc(EIO);
        return;
    }

    g_ctx.writes_done++;
    if (g_ctx.writes_done % 10 == 0 || g_ctx.writes_done == g_ctx.total_writes) {
        SPDK_NOTICELOG(
          "write progress %u/%u object=%s target_pg=%u\n",
          g_ctx.writes_done, g_ctx.total_writes,
          g_ctx.object_name.c_str(), g_ctx.target_pg);
    }

    if (g_ctx.writes_done >= g_ctx.total_writes) {
        SPDK_NOTICELOG("write stress finished, object=%s target_pg=%u\n",
          g_ctx.object_name.c_str(), g_ctx.target_pg);
        app_stop_with_rc(0);
        return;
    }

    issue_next_write();
}

static void on_read_done(void* arg, uint64_t, const std::string& data, int32_t state)
{
    auto* req = reinterpret_cast<request_ctx*>(arg);
    delete req;

    if (state != err::E_SUCCESS) {
        SPDK_ERRLOG("read object failed: %s(%d)\n", err::string_status(state), state);
        app_stop_with_rc(EIO);
        return;
    }

    if (data.size() != g_ctx.payload.size()) {
        SPDK_ERRLOG("read size mismatch, got %lu expect %lu\n", data.size(), g_ctx.payload.size());
        app_stop_with_rc(EIO);
        return;
    }

    if (data != g_ctx.payload) {
        SPDK_ERRLOG("read data mismatch for object %s\n", g_ctx.object_name.c_str());
        app_stop_with_rc(EIO);
        return;
    }

    SPDK_NOTICELOG("read verify success, object=%s target_pg=%u size=%lu\n",
      g_ctx.object_name.c_str(), g_ctx.target_pg, data.size());
    app_stop_with_rc(0);
}

static void issue_next_write()
{
    auto* req = new request_ctx{g_ctx.next_request_id++};
    auto data = g_ctx.payload;
    g_ctx.fb_client->write_object(
      g_ctx.object_name,
      0,
      data,
      g_ctx.pool_id,
      &on_write_done,
      req);
}

static void start_worker()
{
    spdk_cpuset cpumask{};
    spdk_cpuset_zero(&cpumask);
    auto current_core = spdk_env_get_current_core();
    spdk_cpuset_set_cpu(&cpumask, current_core, true);

    g_ctx.blk_thread = spdk_thread_create("snapshot_stress_blk", &cpumask);
    auto opts = msg::rdma::client::make_options(g_ctx.pt);
    g_ctx.fb_client = std::make_unique<fblock_client>(g_ctx.mon_client.get(), g_ctx.blk_thread, opts);
    g_ctx.fb_client->start([]() {
        SPDK_NOTICELOG("snapshot_write_stress block client started\n");
        if (g_mode == run_mode::write) {
            issue_next_write();
        } else {
            auto* req = new request_ctx{0};
            g_ctx.fb_client->read_object(
              g_ctx.object_name,
              0,
              g_ctx.payload.size(),
              g_ctx.pool_id,
              &on_read_done,
              req,
              0);
        }
    });
}

static void on_app_start(void*)
{
    if (!g_conf_path) {
        throw std::invalid_argument("config file is required");
    }

    boost::property_tree::read_json(std::string(g_conf_path), g_ctx.pt);
    g_ctx.payload.assign(g_io_size, 0x5a);
    g_ctx.total_writes = g_write_count;
    g_ctx.target_pg = g_target_pg;

    std::vector<monitor::client::endpoint> endpoints{};
    for (auto& mon : g_ctx.pt.get_child("mon_host")) {
        endpoints.push_back({mon.second.get_value<std::string>(), utils::default_monitor_port});
    }

    monitor::client::on_cluster_map_initialized_type init_cb = []() {
        if (!g_ctx.mon_client->get_pool_id(g_ctx.pool_name, g_ctx.pool_id)) {
            SPDK_ERRLOG("pool %s does not exist\n", g_ctx.pool_name.c_str());
            app_stop_with_rc(ENOENT);
            return;
        }

        g_ctx.pool_pg_num = g_ctx.mon_client->get_pg_num(g_ctx.pool_id);
        g_ctx.object_name = find_object_name_for_pg(g_ctx.pool_pg_num, g_ctx.target_pg);
        SPDK_NOTICELOG("pool=%s pool_id=%d pg_num=%u target_pg=%u object=%s\n",
          g_ctx.pool_name.c_str(), g_ctx.pool_id, g_ctx.pool_pg_num,
          g_ctx.target_pg, g_ctx.object_name.c_str());
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
    printf("   FB_SNAPSHOT_POOL        pool name, default fb\n");
    printf("   FB_SNAPSHOT_PG          target pg id, default 0\n");
    printf("   FB_SNAPSHOT_WRITES      write count in write mode, default 64\n");
    printf("   FB_SNAPSHOT_IO_SIZE     io size in bytes, default 4096\n");
    printf("   FB_SNAPSHOT_MODE        write or read, default write\n");
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
    opts.name = "snapshot_write_stress";
    opts.num_entries = 0;

    auto rc = spdk_app_parse_args(argc, argv, &opts, "C:", nullptr, parse_arg, usage);
    if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    if (const char* pool = std::getenv("FB_SNAPSHOT_POOL")) {
        g_pool_name = pool;
    }
    if (const char* target_pg = std::getenv("FB_SNAPSHOT_PG")) {
        g_target_pg = spdk_strtol(target_pg, 10);
    }
    if (const char* writes = std::getenv("FB_SNAPSHOT_WRITES")) {
        g_write_count = spdk_strtol(writes, 10);
    }
    if (const char* io_size = std::getenv("FB_SNAPSHOT_IO_SIZE")) {
        g_io_size = spdk_strtol(io_size, 10);
    }
    if (const char* mode = std::getenv("FB_SNAPSHOT_MODE")) {
        if (!std::strcmp(mode, "write")) {
            g_mode = run_mode::write;
        } else if (!std::strcmp(mode, "read")) {
            g_mode = run_mode::read;
        } else {
            return -EINVAL;
        }
    }

    return spdk_app_start(&opts, on_app_start, nullptr);
}
