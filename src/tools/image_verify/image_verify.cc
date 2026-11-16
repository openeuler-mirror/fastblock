/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/**
 * Data-correctness verifier for a fastblock image.
 *
 * Unlike block_bench (latency/IOPS, constant 0x55 fill), this tool writes a
 * unique VERIFYA1 payload at every offset and reads it back byte-for-byte.
 * dump-compare walks replica AOS files offline.
 *
 *   image_verify -C conf.json -M write  [-S seed] [-I image]
 *   image_verify -C conf.json -M verify [-S seed] [-I image]
 *   image_verify -M dump-compare [--align 512] [--seed N] aos_file...
 */

#include "dump_compare.h"
#include "verify_pattern.h"

#include "fastblock/base/core_sharded.h"
#include "fastblock/client/libfblock.h"
#include "fastblock/utils/err_num.h"
#include "fastblock/utils/simple_poller.h"
#include "fastblock/utils/utils.h"

#include <spdk/event.h>
#include <spdk/string.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

SPDK_LOG_REGISTER_COMPONENT(imgv)

using image_verify::check_payload;
using image_verify::make_payload;
using image_verify::pattern_error;
using image_verify::pattern_error_name;

namespace {

enum class io_mode { write, verify };

struct request_stack {
  uint64_t id{0};
  uint64_t offset{0};
  std::string payload;
};

struct app_context {
  io_mode mode{io_mode::write};
  uint64_t seed{1};
  size_t io_size{4096};
  size_t io_count{0};
  size_t io_depth{1};
  int32_t pool_id{0};
  std::string pool_name;
  std::string image_name;
  size_t image_size{0};
  size_t object_size{0};
  size_t issued{0};
  size_t completed{0};
  size_t inflight{0};
  size_t mismatches{0};
  size_t io_errors{0};
  uint64_t next_id{1};
  bool stopping{false};
  bool map_ready{false};
  bool image_ready{false};
  bool io_started{false};
  std::unique_ptr<libblk_client> blk_client;
  ::spdk_thread* io_thread{nullptr};
  utils::simple_poller watch_poller;
  std::unordered_map<uint64_t, std::unique_ptr<request_stack>> inflight_req;
};

static char* g_conf_path{nullptr};
static char* g_mode_arg{nullptr};
static char* g_seed_arg{nullptr};
static char* g_image_arg{nullptr};
static boost::property_tree::ptree g_pt{};
static std::shared_ptr<connect_cache> g_conn_cache;
static std::unique_ptr<monitor::client> g_mon_client;
static app_context g_app{};
static int g_exit_rc{0};
static constexpr const char* k_osd_no_huge_key = "osd_no_huge";
static constexpr const char* k_osd_mem_size_mb_key = "osd_mem_size_mb";
static constexpr const char* k_osd_iobuf_small_pool_count_key =
    "osd_iobuf_small_pool_count";
static constexpr const char* k_osd_iobuf_large_pool_count_key =
    "osd_iobuf_large_pool_count";

void hex_preview(const void* data, size_t n, char* out, size_t out_len) {
  const auto* p = static_cast<const uint8_t*>(data);
  size_t used = 0;
  const size_t show = n < 16 ? n : 16;
  for (size_t i = 0; i < show && used + 4 < out_len; ++i) {
    used += static_cast<size_t>(
        std::snprintf(out + used, out_len - used, "%02x", p[i]));
  }
  if (used < out_len) {
    out[used] = '\0';
  }
}

void request_stop(int rc) {
  if (g_exit_rc == 0 && rc != 0) {
    g_exit_rc = rc;
  }
  if (!g_app.stopping) {
    std::raise(SIGINT);
  }
}

void issue_one();

void on_write_done(::spdk_bdev_io* ctx, int32_t res) {
  auto* stk = reinterpret_cast<request_stack*>(ctx);
  if (res != errc::success) {
    SPDK_ERRLOG("write error res=%d offset=%lu\n", res, stk->offset);
    ++g_app.io_errors;
    g_app.inflight_req.erase(stk->id);
    if (g_app.inflight > 0) {
      --g_app.inflight;
    }
    request_stop(EIO);
    return;
  }
  g_app.inflight_req.erase(stk->id);
  if (g_app.inflight > 0) {
    --g_app.inflight;
  }
  ++g_app.completed;
  if ((g_app.completed % 16) == 0 || g_app.completed == g_app.io_count) {
    SPDK_NOTICELOG("write progress %lu/%lu\n",
                   static_cast<unsigned long>(g_app.completed),
                   static_cast<unsigned long>(g_app.io_count));
  }
  issue_one();
}

void on_read_done(::spdk_bdev_io* ctx, char* data, uint64_t size, int32_t res) {
  auto* stk = reinterpret_cast<request_stack*>(ctx);
  if (res != errc::success) {
    SPDK_ERRLOG("read error res=%d offset=%lu\n", res, stk->offset);
    ++g_app.io_errors;
    g_app.inflight_req.erase(stk->id);
    if (g_app.inflight > 0) {
      --g_app.inflight;
    }
    request_stop(EIO);
    return;
  }
  pattern_error st = pattern_error::ok;
  if (size != g_app.io_size) {
    st = pattern_error::length_mismatch;
  } else {
    st = check_payload(data, static_cast<size_t>(size), g_app.seed, stk->offset);
  }
  if (st != pattern_error::ok) {
    char got[48]{};
    hex_preview(data, static_cast<size_t>(size), got, sizeof(got));
    SPDK_ERRLOG(
        "verify fail offset=%lu size=%lu err=%s head=%s\n", stk->offset,
        static_cast<unsigned long>(size), pattern_error_name(st), got);
    ++g_app.mismatches;
    g_app.inflight_req.erase(stk->id);
    if (g_app.inflight > 0) {
      --g_app.inflight;
    }
    request_stop(EIO);
    return;
  }
  g_app.inflight_req.erase(stk->id);
  if (g_app.inflight > 0) {
    --g_app.inflight;
  }
  ++g_app.completed;
  if ((g_app.completed % 16) == 0 || g_app.completed == g_app.io_count) {
    SPDK_NOTICELOG("verify progress %lu/%lu\n",
                   static_cast<unsigned long>(g_app.completed),
                   static_cast<unsigned long>(g_app.io_count));
  }
  issue_one();
}

void issue_one() {
  if (g_app.stopping) {
    return;
  }
  if (g_app.issued >= g_app.io_count) {
    return;
  }
  const uint64_t offset = g_app.issued * g_app.io_size;
  if (offset + g_app.io_size > g_app.image_size) {
    SPDK_ERRLOG("offset %lu beyond image_size %lu\n",
                static_cast<unsigned long>(offset),
                static_cast<unsigned long>(g_app.image_size));
    request_stop(EINVAL);
    return;
  }
  auto stk = std::make_unique<request_stack>();
  stk->id = g_app.next_id++;
  stk->offset = offset;
  stk->payload = make_payload(g_app.seed, offset, g_app.io_size);
  auto* raw = stk.get();
  ++g_app.issued;
  ++g_app.inflight;
  g_app.inflight_req.emplace(raw->id, std::move(stk));

  if (g_app.mode == io_mode::write) {
    g_app.blk_client->write(static_cast<uint64_t>(g_app.pool_id),
                            g_app.image_name, offset,
                            reinterpret_cast<::spdk_bdev_io*>(raw),
                            raw->payload, on_write_done);
  } else {
    g_app.blk_client->read(static_cast<uint64_t>(g_app.pool_id),
                           g_app.image_name, offset, g_app.io_size,
                           reinterpret_cast<::spdk_bdev_io*>(raw),
                           on_read_done);
  }
}

int watch_poller(void*) {
  if (g_app.stopping) {
    return SPDK_POLLER_IDLE;
  }
  if (g_app.io_started && g_app.completed == g_app.io_count &&
      g_app.inflight == 0) {
    SPDK_NOTICELOG("%s done ok=%lu errors=%lu mismatches=%lu seed=%lu image=%s\n",
                   g_app.mode == io_mode::write ? "write" : "verify",
                   static_cast<unsigned long>(g_app.completed),
                   static_cast<unsigned long>(g_app.io_errors),
                   static_cast<unsigned long>(g_app.mismatches),
                   static_cast<unsigned long>(g_app.seed),
                   g_app.image_name.c_str());
    std::cout << "image_verify " << (g_app.mode == io_mode::write ? "write" : "verify")
              << " OK count=" << g_app.completed << " seed=" << g_app.seed
              << " image=" << g_app.image_name << " io_size=" << g_app.io_size
              << "\n";
    request_stop(0);
  }
  return SPDK_POLLER_IDLE;
}

void start_io() {
  if (g_app.io_started || g_app.stopping) {
    return;
  }
  if (!g_app.map_ready || !g_app.image_ready || !g_app.blk_client) {
    return;
  }
  int32_t pool_id = 0;
  if (!g_mon_client->get_pool_id(g_app.pool_name, pool_id)) {
    SPDK_ERRLOG("pool %s does not exist\n", g_app.pool_name.c_str());
    request_stop(ENOENT);
    return;
  }
  g_app.pool_id = pool_id;
  g_app.io_started = true;
  SPDK_NOTICELOG(
      "start %s pool=%s(%d) image=%s seed=%lu io_size=%lu io_count=%lu depth=%lu\n",
      g_app.mode == io_mode::write ? "write" : "verify", g_app.pool_name.c_str(),
      g_app.pool_id, g_app.image_name.c_str(),
      static_cast<unsigned long>(g_app.seed),
      static_cast<unsigned long>(g_app.io_size),
      static_cast<unsigned long>(g_app.io_count),
      static_cast<unsigned long>(g_app.io_depth));

  const size_t kick = std::min(g_app.io_depth, g_app.io_count);
  for (size_t i = 0; i < kick; ++i) {
    issue_one();
  }
}

void on_client_thread(void*) {
  auto core_no = ::spdk_env_get_current_core();
  ::spdk_cpuset cpumask{};
  ::spdk_cpuset_zero(&cpumask);
  ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
  auto opts = msg::rdma::client::make_options(g_pt);
  g_app.blk_client =
      std::make_unique<libblk_client>(g_mon_client.get(), g_app.io_thread, opts);
  g_app.blk_client->start([]() {
    SPDK_NOTICELOG("image_verify client started\n");
    start_io();
  });
}

void on_app_stop() noexcept {
  if (g_app.stopping) {
    return;
  }
  g_app.stopping = true;
  SPDK_NOTICELOG("stopping image_verify\n");

  g_app.watch_poller.unregister_poller([]() {
    if (g_mon_client) {
      g_mon_client->stop([]() {
        if (g_conn_cache) {
          g_conn_cache->stop([]() {
            auto finish = []() {
              core_sharded::stop_all();
              ::spdk_app_stop(g_exit_rc);
            };
            if (g_app.blk_client) {
              auto* thd = g_app.blk_client->get_blk_thread();
              g_app.blk_client->stop([thd, finish]() {
                if (thd != nullptr) {
                  ::spdk_thread_exit(thd);
                }
                finish();
              });
            } else {
              finish();
            }
          });
        } else {
          core_sharded::stop_all();
          ::spdk_app_stop(g_exit_rc);
        }
      });
    } else {
      core_sharded::stop_all();
      ::spdk_app_stop(g_exit_rc);
    }
  });
}

void parse_conf() {
  boost::property_tree::read_json(std::string(g_conf_path), g_pt);

  std::string mode = "write";
  if (g_mode_arg != nullptr) {
    mode = g_mode_arg;
  } else if (g_pt.count("mode") != 0) {
    mode = g_pt.get<std::string>("mode");
  } else if (g_pt.count("io_type") != 0) {
    const auto t = g_pt.get<std::string>("io_type");
    if (t == "read") {
      mode = "verify";
    }
  }
  if (mode == "write") {
    g_app.mode = io_mode::write;
  } else if (mode == "verify" || mode == "read") {
    g_app.mode = io_mode::verify;
  } else {
    throw std::invalid_argument("mode must be write or verify");
  }

  if (g_seed_arg != nullptr) {
    g_app.seed = std::stoull(g_seed_arg, nullptr, 0);
  } else if (g_pt.count("seed") != 0) {
    g_app.seed = g_pt.get<uint64_t>("seed");
  }

  g_app.io_size = g_pt.get<size_t>("io_size");
  g_app.io_count = g_pt.get<size_t>("io_count");
  g_app.io_depth = g_pt.get<size_t>("io_depth");
  g_app.image_name = g_pt.get<std::string>("image_name");
  g_app.image_size = g_pt.get<size_t>("image_size");
  g_app.object_size = g_pt.get<size_t>("object_size");
  g_app.pool_name = g_pt.get<std::string>("pool_name");
  if (g_image_arg != nullptr && *g_image_arg != '\0') {
    g_app.image_name = g_image_arg;
  }
  if (g_app.io_size == 0 || g_app.io_count == 0 || g_app.io_depth == 0) {
    throw std::invalid_argument("io_size/io_count/io_depth must be > 0");
  }
  if (g_app.io_count * g_app.io_size > g_app.image_size) {
    throw std::invalid_argument(
        "io_count * io_size exceeds image_size (refusing wrap)");
  }
  if (g_app.image_name.empty()) {
    throw std::invalid_argument("image_name is required");
  }
}

void on_app_start(void*) {
  parse_conf();

  auto core_begin = core_sharded::system::begin();
  auto n_core = std::max(core_sharded::system::size_type{1},
                         core_sharded::system::capacity());
  core_sharded::construct(core_begin, n_core, "image_verify");

  std::vector<monitor::client::endpoint> eps;
  auto& monitors = g_pt.get_child("mon_host");
  for (auto pos = monitors.begin(); pos != monitors.end(); ++pos) {
    auto mon_addr = pos->second.get_value<std::string>();
    eps.emplace_back(std::move(mon_addr), utils::default_monitor_port);
  }

  auto core_no = ::spdk_env_get_current_core();
  ::spdk_cpuset cpumask{};
  ::spdk_cpuset_zero(&cpumask);
  ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
  auto opts = msg::rdma::client::make_options(g_pt);
  g_conn_cache = std::make_shared<connect_cache>(&cpumask, opts);

  monitor::client::on_cluster_map_initialized_type map_cb = []() {
    SPDK_NOTICELOG("cluster map ready\n");
    g_app.map_ready = true;
    start_io();
  };
  g_mon_client = std::make_unique<monitor::client>(eps, std::move(map_cb));
  g_mon_client->start();
  g_mon_client->start_cluster_map_poller();
  g_mon_client->emplace_create_image_request(
      g_app.pool_name, g_app.image_name, g_app.image_size, g_app.object_size,
      [](const monitor::client::response_status s,
         monitor::client::request_context*) {
        switch (s) {
        case monitor::client::response_status::ok:
        case monitor::client::response_status::created_image_exists:
          SPDK_NOTICELOG("image %s ready\n", g_app.image_name.c_str());
          g_app.image_ready = true;
          start_io();
          break;
        default:
          SPDK_ERRLOG("create image failed status=%d\n", static_cast<int>(s));
          request_stop(EIO);
          break;
        }
      });

  g_app.io_thread = ::spdk_thread_create("imgv_io", &cpumask);
  g_app.watch_poller.set_thread(::spdk_get_thread());
  g_app.watch_poller.register_poller(watch_poller, nullptr, 1000, "imgv_watch");
  ::spdk_thread_send_msg(g_app.io_thread, on_client_thread, nullptr);
}

void usage() {
  ::printf(
      "-C <path>   json config (same mon/rdma keys as block_bench)\n"
      "-M <mode>   write | verify | dump-compare\n"
      "-S <seed>   payload seed (must match between write and verify)\n"
      "-I <name>   override image_name\n"
      "dump-compare: image_verify -M dump-compare [--align N] [--seed N] "
      "aos_file...\n");
}

int parse_arg(int ch, char* arg) {
  switch (ch) {
  case 'C':
    g_conf_path = arg;
    break;
  case 'M':
    g_mode_arg = arg;
    break;
  case 'S':
    g_seed_arg = arg;
    break;
  case 'I':
    g_image_arg = arg;
    break;
  default:
    throw std::invalid_argument{"Unknown options"};
  }
  return 0;
}

std::string mode_from_argv(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if ((std::strcmp(argv[i], "--mode") == 0 ||
         std::strcmp(argv[i], "-M") == 0) &&
        i + 1 < argc) {
      return argv[i + 1];
    }
    if (std::strncmp(argv[i], "--mode=", 7) == 0) {
      return argv[i] + 7;
    }
  }
  return {};
}

void apply_spdk_mem_opts(::spdk_app_opts* opts) {
  if (g_conf_path == nullptr) {
    return;
  }
  boost::property_tree::ptree startup_pt{};
  boost::property_tree::read_json(std::string(g_conf_path), startup_pt);
  if (startup_pt.count(k_osd_no_huge_key) > 0) {
    opts->no_huge = startup_pt.get<bool>(k_osd_no_huge_key);
  }
  if (startup_pt.count(k_osd_mem_size_mb_key) > 0) {
    opts->mem_size = startup_pt.get<int>(k_osd_mem_size_mb_key);
  } else if (opts->no_huge) {
    opts->mem_size = 1024;
  }
  if (opts->no_huge || startup_pt.count(k_osd_iobuf_small_pool_count_key) > 0 ||
      startup_pt.count(k_osd_iobuf_large_pool_count_key) > 0) {
    spdk_iobuf_opts iobuf_opts = {};
    spdk_iobuf_get_opts(&iobuf_opts, sizeof(iobuf_opts));
    iobuf_opts.opts_size = sizeof(iobuf_opts);
    if (opts->no_huge) {
      iobuf_opts.small_pool_count = 1024;
      iobuf_opts.large_pool_count = 64;
    }
    if (startup_pt.count(k_osd_iobuf_small_pool_count_key) > 0) {
      iobuf_opts.small_pool_count =
          startup_pt.get<uint64_t>(k_osd_iobuf_small_pool_count_key);
    }
    if (startup_pt.count(k_osd_iobuf_large_pool_count_key) > 0) {
      iobuf_opts.large_pool_count =
          startup_pt.get<uint64_t>(k_osd_iobuf_large_pool_count_key);
    }
    if (spdk_iobuf_set_opts(&iobuf_opts) != 0) {
      SPDK_ERRLOG("configure spdk iobuf options failed\n");
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  const auto mode = mode_from_argv(argc, argv);
  if (mode == "dump-compare") {
    return image_verify::run_dump_compare(argc, argv);
  }

  ::spdk_app_opts opts{};
  ::spdk_app_opts_init(&opts, sizeof(opts));
  int rc = ::spdk_app_parse_args(argc, argv, &opts, "C:M:S:I:", nullptr,
                                 parse_arg, usage);
  if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
    ::exit(rc);
  }
  if (g_conf_path == nullptr) {
    usage();
    return 2;
  }

  opts.name = "image_verify";
  opts.shutdown_cb = on_app_stop;
  opts.iova_mode = "va";
  apply_spdk_mem_opts(&opts);

  rc = ::spdk_app_start(&opts, on_app_start, nullptr);
  ::spdk_app_fini();
  return rc;
}
