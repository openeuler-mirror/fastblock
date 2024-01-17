/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "client/libfblock.h"
#include "utils/units.h"
#include "utils/simple_poller.h"
#include "osd/partition_manager.h"

#include <spdk/event.h>
#include <spdk/string.h>

#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <vector>
#include <random>
#include <string>

SPDK_LOG_REGISTER_COMPONENT(bbench)

namespace {
enum bench_io_type {
    write = 1,
    read,
    write_read
};

struct request_stack {
    size_t id{};
    void* ctx{};
    uint64_t offset{0};
    uint64_t start_tick{::spdk_get_ticks()};
};

struct bench_context {
    void* watcher_ctx{nullptr};
    uint32_t core{};
    std::list<std::string> write_obj_name{};
    size_t request_id_gen{0};
    size_t on_flight_io_count{0};
    size_t io_count{0};
    size_t done_io_count{0};
    std::unordered_map<size_t, std::unique_ptr<request_stack>> on_flight_request{};
    std::vector<double> durs{};
    std::unique_ptr<::libblk_client> blk_client{};
    bench_io_type current_io_type{bench_io_type::write};
};

struct watcher_context {
    bench_io_type io_type{};
    size_t io_size{};
    size_t total_io_count{};
    size_t io_depth{1};
    int32_t pool_id{};
    int32_t io_queue_size{};
    int32_t io_queue_request{};
    std::string pool_name{};
    std::string image_name{};
    size_t image_size{0};
    size_t object_size{0};
    bool is_exit{false};
    bench_io_type current_bench_type{bench_io_type::write};
    std::unique_ptr<::bench_context[]> core_ctxs{nullptr};
    std::unique_ptr<::spdk_thread*[]> bench_threads{nullptr};
    utils::simple_poller watch_poller_holder{};
    ::read_callback read_done_cb{};
    ::write_callback write_done_cb{};
    uint64_t iops_start_at{};
};

static char* g_conf_path{nullptr};
static std::shared_ptr<::connect_cache> conn_cache;
static std::shared_ptr<::partition_manager> par_mgr;
static std::unique_ptr<monitor::client> mon_client;
static std::string sample_data{};
static boost::property_tree::ptree g_pt{};

std::string random_string(const size_t length) {
    static std::string chars{
      "abcdefghijklmnopqrstuvwxyz"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"};

    std::random_device rd{};
    std::uniform_int_distribution<decltype(chars)::size_type> index_dist{0, chars.size() - 1};
    std::string ret(length, ' ');
    for (size_t i{0}; i < length; ++i) {
        ret[i] = chars[index_dist(rd)];
    }

    return ret;
}
}

inline double tick_to_us(const double tick) noexcept {
    return tick * 1000 * 1000 / ::spdk_get_ticks_hz();
}

inline double tick_to_ms(const double tick) noexcept {
    return tick * 1000 / ::spdk_get_ticks_hz();
}

static void usage() {
    ::printf("-C <path>             path to bench json config file\n");
}

static int parse_arg(int ch, char* arg) {
    switch (ch) {
    case 'C':
        g_conf_path = arg;
        break;
    default:
        throw std::invalid_argument{"Unknown options"};
    }

    return 0;
}

void write_once(bench_context* ctx) {
    auto* watcher_ctx = reinterpret_cast<watcher_context*>(ctx->watcher_ctx);
    ++(ctx->on_flight_io_count);
    auto rnd = ::rand();
    auto offset = (rnd * watcher_ctx->io_size) % watcher_ctx->image_size;
    auto request_stk = std::make_unique<request_stack>(
      ctx->request_id_gen++, ctx,
      offset, ::spdk_get_ticks());
    ctx->blk_client->write(
      watcher_ctx->pool_id, watcher_ctx->image_name, request_stk->offset,
      reinterpret_cast<::spdk_bdev_io*>(request_stk.get()),
      sample_data,
      watcher_ctx->write_done_cb);
    ctx->on_flight_request.emplace(request_stk->id, std::move(request_stk));
}

void read_once(bench_context* ctx) {
    auto* watcher_ctx = reinterpret_cast<watcher_context*>(ctx->watcher_ctx);
    ++(ctx->on_flight_io_count);
    auto rnd = ::rand();
    auto offset = (rnd * watcher_ctx->io_size) % watcher_ctx->image_size;
    auto request_stk = std::make_unique<request_stack>(
      ctx->request_id_gen++, reinterpret_cast<void*>(ctx),
      offset, ::spdk_get_ticks());
    ctx->blk_client->read(
      watcher_ctx->pool_id, watcher_ctx->image_name, request_stk->offset, watcher_ctx->io_size,
      reinterpret_cast<::spdk_bdev_io*>(request_stk.get()), watcher_ctx->read_done_cb);
    ctx->on_flight_request.emplace(request_stk->id, std::move(request_stk));
    if (watcher_ctx->io_type == bench_io_type::write_read) {
        ctx->write_obj_name.pop_front();
    }
}

void on_write_done(::spdk_bdev_io* ctx, [[maybe_unused]] int32_t res) {
    if (res != errc::success) {
        SPDK_ERRLOG("Write object error\n");
    }
    auto* stack_ptr = reinterpret_cast<request_stack*>(ctx);
    auto* bench_ctx = reinterpret_cast<bench_context*>(stack_ptr->ctx);
    auto* watcher_ctx = reinterpret_cast<watcher_context*>(bench_ctx->watcher_ctx);

    auto dur = static_cast<double>(::spdk_get_ticks() - stack_ptr->start_tick);
    SPDK_DEBUGLOG(
      bbench,
      "write duration is %lfus/%lfms, raw value is %lf, start_tsc is %lf\n",
      tick_to_us(dur), tick_to_ms(dur), dur, stack_ptr->start_tick);
    bench_ctx->durs.push_back(dur);
    SPDK_DEBUGLOG(bbench, "The %dth write request done\n", stack_ptr->id);
    bench_ctx->done_io_count++;

    if (stack_ptr->id % 100 == 0) {
        SPDK_INFOLOG(
          bbench,
          "%ldth request done, done_io_count is %ld, io_count is %ld\n",
          stack_ptr->id, bench_ctx->done_io_count, bench_ctx->io_count);
    }

    if (bench_ctx->on_flight_io_count < bench_ctx->io_count) {
        write_once(bench_ctx);
    }
}

void on_read_done(::spdk_bdev_io* arg, char* data, uint64_t size, int32_t res) {
    if (res != errc::success) {
        SPDK_ERRLOG("Read object error\n");
    }

    auto* stack_ptr = reinterpret_cast<request_stack*>(arg);
    auto* bench_ctx = reinterpret_cast<bench_context*>(stack_ptr->ctx);
    auto* watcher_ctx = reinterpret_cast<watcher_context*>(bench_ctx->watcher_ctx);

    auto dur = static_cast<double>(::spdk_get_ticks() - stack_ptr->start_tick);
    SPDK_DEBUGLOG(
      bbench,
      "read duration is %lfus/%lfms, raw value is %lf, start_tsc is %lf\n",
      tick_to_us(dur), tick_to_ms(dur), dur, stack_ptr->start_tick);
    bench_ctx->durs.push_back(dur);
    SPDK_DEBUGLOG(bbench, "The %dth read request done\n", stack_ptr->id);
    bench_ctx->on_flight_request.erase(stack_ptr->id);
    bench_ctx->done_io_count++;

    if (stack_ptr->id % 100 == 0) {
        SPDK_INFOLOG(bbench, "%ldth request done\n", stack_ptr->id);
    }

    if (bench_ctx->on_flight_io_count < bench_ctx->io_count) {
        read_once(bench_ctx);
    }
}

void on_thread_received_msg(void* arg) {
    auto* ctx = reinterpret_cast<bench_context*>(arg);
    auto* watcher_ctx = reinterpret_cast<watcher_context*>(ctx->watcher_ctx);
    SPDK_NOTICELOG(
      "Start bench request on core %d, io count is %ld\n",
      ctx->core, ctx->io_count);
    // FIXME:
    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
    auto opts = msg::rdma::client::make_options(
      g_pt.get_child("msg").get_child("client"),
      g_pt.get_child("msg").get_child("rdma"));
    ctx->blk_client = std::make_unique<::libblk_client>(
      mon_client.get(),
      &cpumask, opts);
    SPDK_DEBUGLOG(bbench, "starting block client\n");
    ctx->blk_client->start();

    SPDK_DEBUGLOG(bbench, "start sending rpc\n");
    watcher_ctx->iops_start_at = ::spdk_get_ticks();
    for (auto i{0}; i < watcher_ctx->io_depth; ++i) {
        switch (watcher_ctx->io_type) {
        case bench_io_type::write:
            write_once(ctx);
            break;
        case bench_io_type::read:
            read_once(ctx);
            break;
        case bench_io_type::write_read: {
            switch (ctx->current_io_type) {
            case bench_io_type::write:
                write_once(ctx);
                break;
            case bench_io_type::read: {
                auto head_it = ctx->on_flight_request.begin();
                head_it->second->start_tick = ::spdk_get_ticks();
                ctx->blk_client->read(
                  watcher_ctx->pool_id,
                  watcher_ctx->image_name,
                  head_it->second->offset,
                  watcher_ctx->io_size,
                  reinterpret_cast<::spdk_bdev_io*>(head_it->second.get()),
                  on_read_done);
                ctx->on_flight_io_count++;
                ctx->on_flight_request.erase(head_it);
                break;
            }
            default:
                break;
            }
        }
        default:
            break;
        }
    }
}

int watch_poller(void* arg) {
    auto* ctx = reinterpret_cast<watcher_context*>(arg);
    if (ctx->is_exit) {
        return SPDK_POLLER_IDLE;
    }

    bool is_all_done{true};
    auto n_core = ::spdk_env_get_core_count();
    auto& core_ctxs = ctx->core_ctxs;
    for (decltype(n_core) i{0}; i < n_core; ++i) {
        is_all_done = is_all_done && (core_ctxs[i].done_io_count == core_ctxs[i].io_count);
    }

    if (is_all_done) {
        auto iops_dur = ::spdk_get_ticks() - ctx->iops_start_at;
        bool should_exit{true};
        switch (ctx->io_type) {
        case bench_io_type::write_read:
            if (core_ctxs[0].current_io_type == bench_io_type::write) {
                should_exit = false;
            }
            ctx->current_bench_type = core_ctxs[0].current_io_type;
            break;
        default:
            ctx->current_bench_type = ctx->io_type;
            break;
        }

        std::vector<double> durations{};
        for (decltype(n_core) i{0}; i < n_core; ++i) {
            durations.insert(durations.end(), core_ctxs[i].durs.begin(), core_ctxs[i].durs.end());
            if (ctx->io_type == bench_io_type::write_read and core_ctxs[i].current_io_type == bench_io_type::write) {
                core_ctxs[i].current_io_type = bench_io_type::read;
                core_ctxs[i].durs.clear();
                core_ctxs[i].on_flight_io_count = 0;
                core_ctxs[i].done_io_count = 0;
            }
        }

        SPDK_DEBUGLOG(bbench, "All requests done, durations count is %ld\n", durations.size());
        std::sort(durations.begin(), durations.end());

        double mean{};
        for (auto& dur : durations) {
            mean += dur;
        }
        mean /= durations.size();

        double accum{0.0};
        std::for_each(
          durations.begin(), durations.end(),
          [&accum, mean] (const double v) {
              accum += (v - mean) * (v - mean);
          }
        );
        auto biased_stdv = std::sqrt(accum / (durations.size()));

        static constexpr size_t lat_tag_count{6};
        static constexpr std::array<double, lat_tag_count> latency_tag = {0.1, 0.5, 0.9, 0.95, 0.99, 0.999};

        switch (ctx->current_bench_type) {
        case bench_io_type::write:
            std::cout << "===============================[write latency]========================================\n";
            break;
        case bench_io_type::read:
            std::cout << "===============================[read  latency]========================================\n";
            break;
        default:
            break;
        }

        auto fmt = boost::format("");

        size_t count{0};
        for (auto lat_tag : latency_tag) {
            auto lat_at = static_cast<size_t>(lat_tag * durations.size());

            SPDK_DEBUGLOG(bbench, "p%f at %ld\n", lat_tag, lat_at);
            auto lat = durations.at(lat_at);
            if (count == 0) {
                fmt = boost::format("%1%p%2%: %3%us") % fmt % lat_tag % tick_to_us(lat);
            } else {
                fmt = boost::format("%1%, p%2%: %3%us") % fmt % lat_tag % tick_to_us(lat);
            }
            ++count;
        }

        auto min = durations.at(0);
        auto max = durations.at(durations.size() - 1);

        auto iops_dur_sec = static_cast<double>(iops_dur) / ::spdk_get_ticks_hz();
        auto iops_val = ctx->total_io_count / iops_dur_sec;

        fmt = boost::format("%1%, mean: %2%us, min: %3%us, max: %4%us, biased stdv: %5%, iops: %6%, iops_dur_sec: %7%, total_io_count: %8%")
          % fmt % tick_to_us(mean) % tick_to_us(min)
          % tick_to_us(max) % tick_to_us(biased_stdv)
          % iops_val % iops_dur_sec % (ctx->total_io_count);

        std::cout << fmt.str() << "\n";
        std::cout << "======================================================================================\n";

        if (should_exit) {
            ctx->is_exit = true;
            auto n_core = ::spdk_env_get_core_count();
            for (int i{0}; i < n_core; ++i) {
                ::spdk_set_thread(ctx->bench_threads[i]);
                ctx->core_ctxs[i].blk_client->stop();
                ::spdk_set_thread(nullptr);
            }
            mon_client->stop();
            ctx->watch_poller_holder.unregister();
            ::spdk_app_stop(0);
        }
    }

    return SPDK_POLLER_IDLE;
}

void on_app_start(void* arg) {
    auto* watcher_ctx = reinterpret_cast<watcher_context*>(arg);

    SPDK_INFOLOG(bbench, "Going to parse json conf file %s\n", g_conf_path);

    boost::property_tree::read_json(std::string(g_conf_path), g_pt);

    auto raw_io_type = g_pt.get_child("io_type").get_value<std::string>();
    if (raw_io_type == "write") {
        watcher_ctx->io_type = bench_io_type::write;
        SPDK_DEBUGLOG(bbench, "io_type is write\n");
    } else if (raw_io_type == "read") {
        watcher_ctx->io_type = bench_io_type::read;
        SPDK_DEBUGLOG(bbench, "io_type is read\n");
    } else if (raw_io_type == "write_read") {
        watcher_ctx->io_type = bench_io_type::write_read;
        SPDK_DEBUGLOG(bbench, "io_type is write_read\n");
    } else {
        SPDK_ERRLOG("ERROR: unknown io type: %s\n", raw_io_type.c_str());
        exit(-EINVAL);
    }

    watcher_ctx->io_size = g_pt.get_child("io_size").get_value<size_t>();
    sample_data = std::string(watcher_ctx->io_size, 0x55);
    watcher_ctx->total_io_count = g_pt.get_child("io_count").get_value<size_t>();
    watcher_ctx->io_depth = g_pt.get_child("io_depth").get_value<size_t>();
    watcher_ctx->image_name = g_pt.get_child("image_name").get_value<std::string>();
    watcher_ctx->image_size = g_pt.get_child("image_size").get_value<size_t>();
    watcher_ctx->object_size = g_pt.get_child("object_size").get_value<size_t>();
    if (watcher_ctx->image_size <= watcher_ctx->object_size) {
        throw std::invalid_argument{"image size should be greater than object size"};
    }

    if (watcher_ctx->image_name.empty()) {
        watcher_ctx->image_name = random_string(32);
    }
    watcher_ctx->io_queue_size = g_pt.get_child("io_queue_size").get_value<int32_t>();
    watcher_ctx->io_queue_request = g_pt.get_child("io_queue_request").get_value<int32_t>();
    watcher_ctx->pool_id = g_pt.get_child("pool_id").get_value<int32_t>();
    watcher_ctx->pool_name = g_pt.get_child("pool_name").get_value<std::string>();

    auto& monitors = g_pt.get_child("monitor");
    std::vector<monitor::client::endpoint> eps{};
    for (auto& monitor_node : monitors) {
        auto mon_host = monitor_node.second.get_child("host").get_value<std::string>();
        auto mon_port = monitor_node.second.get_child("port").get_value<uint16_t>();
        eps.emplace_back(std::move(mon_host), mon_port);
    }

    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
    auto opts = msg::rdma::client::make_options(
      g_pt.get_child("msg").get_child("client"),
      g_pt.get_child("msg").get_child("rdma"));
    conn_cache = std::make_shared<::connect_cache>(&cpumask, opts);
    par_mgr = std::make_shared<::partition_manager>(-1, conn_cache);
    monitor::client::on_cluster_map_initialized_type cb = [watcher_ctx] () {
        auto n_core = ::spdk_env_get_core_count();
        watcher_ctx->core_ctxs = std::make_unique<bench_context[]>(n_core);
        watcher_ctx->read_done_cb = on_read_done;
        watcher_ctx->write_done_cb = on_write_done;
        watcher_ctx->bench_threads = std::make_unique<::spdk_thread*[]>(n_core);

        ::spdk_cpuset tmp_cpumask{};
        uint32_t core_no{0};
        ::spdk_thread* thread{};
        uint32_t core_count{0};
        auto total_io_count = watcher_ctx->total_io_count;
        auto io_count_per_core = total_io_count / static_cast<size_t>(n_core);
        SPDK_ENV_FOREACH_CORE(core_no) {
            auto& ctx = watcher_ctx->core_ctxs[core_count];
            if (core_count == n_core - 1) {
                ctx.io_count = total_io_count;
                auto min_io_count = std::min(total_io_count, io_count_per_core);
                watcher_ctx->io_depth = std::min(watcher_ctx->io_depth, min_io_count);
            } else {
                ctx.io_count = io_count_per_core;
            }
            total_io_count -= io_count_per_core;
            ctx.watcher_ctx = watcher_ctx;
            ctx.core = core_count;

            ::spdk_cpuset_zero(&tmp_cpumask);
            ::spdk_cpuset_set_cpu(&tmp_cpumask, core_no, true);
            std::string thread_name{(boost::format("bench_poller_%1%") % core_no).str()};
            thread = ::spdk_thread_create(thread_name.c_str(), &tmp_cpumask);
            assert(!!thread);
            ::spdk_thread_send_msg(thread, on_thread_received_msg, &ctx);
            watcher_ctx->bench_threads[core_count] = thread;
            core_count++;
        }
        watcher_ctx->watch_poller_holder.poller = SPDK_POLLER_REGISTER(watch_poller, watcher_ctx, 0);
    };

    mon_client = std::make_unique<monitor::client>(eps, par_mgr, std::nullopt, std::move(cb));
    mon_client->start();
    mon_client->start_cluster_map_poller();
    mon_client->emplace_create_image_request(
      watcher_ctx->pool_name, watcher_ctx->image_name,
      watcher_ctx->image_size, watcher_ctx->object_size,
      [watcher_ctx] (const monitor::client::response_status s, monitor::client::request_context* req_ctx) {
          switch (s) {
          case monitor::client::response_status::ok:
          case monitor::client::response_status::created_image_exists:
              SPDK_INFOLOG(bbench, "image is ready\n");
              break;
          default:
              SPDK_ERRLOG("Create image error, error code is %d\n", s);
              throw std::runtime_error{"create image error"};
          }
      }
    );
}

int main(int argc, char** argv) {
    ::spdk_app_opts opts{};
    ::spdk_app_opts_init(&opts, sizeof(opts));

    int rc{0};
    if ((rc = ::spdk_app_parse_args(argc, argv, &opts, "C:", nullptr, parse_arg, usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        ::exit(rc);
    }

    opts.name = "block bench";
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;
    watcher_context ctx{};
    rc = ::spdk_app_start(&opts, on_app_start, &ctx);
    if (rc) {
        SPDK_ERRLOG("ERROR: Start spdk app failed\n");
    }

    ::spdk_app_fini();
    return rc;
}
