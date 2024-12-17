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
#include "utils/units.h"
#include "fastblock/utils/simple_poller.h"
#include "osd/partition_manager.h"

#include <spdk/event.h>
#include <spdk/string.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <fstream>
#include <sstream>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
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
    size_t deferred_count{0};
    std::unordered_map<size_t, std::unique_ptr<request_stack>> on_flight_request{};
    std::vector<double> durs{};
    decltype(durs)::iterator dur_it{durs.end()};
    std::unique_ptr<::libblk_client> blk_client{};
    bench_io_type current_io_type{bench_io_type::write};
    double acc_dur{0.0};
    size_t acc_dur_count{0};
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
    size_t core_context_size{0};
    std::unique_ptr<::bench_context[]> core_ctxs{nullptr};
    std::unique_ptr<::spdk_thread*[]> bench_threads{nullptr};
    utils::simple_poller watch_poller_holder{};
    ::read_callback read_done_cb{};
    ::write_callback write_done_cb{};
    uint64_t iops_start_at{};
    uint64_t deferred_time{};
    bool infinity{false};
    double acc_dur{0.0};
    size_t acc_dur_count{0};
    std::vector<double> lats{};
};

static constexpr size_t lat_tag_count{6};
static constexpr std::array<double, lat_tag_count> latency_tag = {0.1, 0.5, 0.9, 0.95, 0.99, 0.999};

static watcher_context g_watcher_ctx{};

inline double tick_to_us(const double tick) noexcept {
    return tick * 1000 * 1000 / ::spdk_get_ticks_hz();
}

inline double tick_to_ms(const double tick) noexcept {
    return tick * 1000 / ::spdk_get_ticks_hz();
}

inline double tick_to_s(const double tick) noexcept {
    return tick / ::spdk_get_ticks_hz();
}

struct print_stats_context {

public:

    struct stats_iops {
        uint32_t core{};
        double iops_val{};
    };

public:

    static void print_stats(
      std::vector<double>::iterator begin,
      const size_t size,
      bool should_print,
      std::optional<uint64_t> iops_dur_opt,
      std::optional<std::string> desc = std::nullopt) {
        auto end = begin + size;
        double mean{0.0};
        for (decltype(begin) it = begin; it != end; ++it) {
            mean += *it;
        }

        mean /= size;

        double accum{0.0};
        std::for_each(begin, end, [&accum, mean] (const double v) {
            accum += (v - mean) * (v - mean);
        });
        auto biased_stdv = std::sqrt(accum / size);

        std::sort(begin, end);
        auto min = *begin;
        auto max = *(end - 1);

        auto fmt = boost::format("");
        size_t count{0};
        auto watcher_lat_idx = lat_tag_count;
        for (auto lat_tag : latency_tag) {
            auto lat_at = static_cast<size_t>(lat_tag * size);
            auto lat = *(begin + lat_at);
            g_watcher_ctx.lats.at(watcher_lat_idx++) = lat;
            if (count == 0) {
                fmt = boost::format("%1%p%2%: %3%us") % fmt % lat_tag % tick_to_us(lat);
            } else {
                fmt = boost::format("%1%, p%2%: %3%us") % fmt % lat_tag % tick_to_us(lat);
            }
            ++count;
        }
        std::sort(g_watcher_ctx.lats.begin(), g_watcher_ctx.lats.end());

        if (not should_print) {
            return;
        }

        auto iops_dur = iops_dur_opt.value();
        auto iops_dur_sec = static_cast<double>(iops_dur) / ::spdk_get_ticks_hz();
        auto iops_val = size / iops_dur_sec;

        fmt = boost::format("%1%, mean: %2%us, min: %3%us, max: %4%us, biased stdv: %5%, iops: %6%, iops_dur_sec: %7%s, total_io_count: %8%")
          % fmt % tick_to_us(mean) % tick_to_us(min)
          % tick_to_us(max) % tick_to_us(biased_stdv)
          % iops_val % iops_dur_sec % size;

        std::cout << fmt.str() << std::endl;
    }

    static int print_poll(void* arg) {
        auto this_ctx = reinterpret_cast<print_stats_context*>(arg);
        this_ctx->print_stats(g_watcher_ctx.core_ctxs.get(), g_watcher_ctx.core_context_size);
        return SPDK_POLLER_BUSY;
    }

public:

    void from_conf(boost::property_tree::ptree& pt) {
        if (pt.count("io_print_stats_enable") == 1) {
            enable_print = pt.get_child("io_print_stats_enable").get_value<bool>();
        }

        if (pt.count("io_print_stats_interval_ms") == 1) {
            interval_us = pt.get_child("io_print_stats_interval_ms").get_value<int64_t>() * 1000;
        }

        if (not enable_print) {
            SPDK_NOTICELOG("print stats is not enabled\n");
        } else {
            SPDK_NOTICELOG(
              "block_bench will print stats every %ldus\n",
              interval_us);
        }

        if (pt.count("io_print_stats_take_single_core") == 1) {
            take_single_core = pt.get_child("io_print_stats_take_single_core").get_value<bool>();
        }

        if (core_sharded::system::capacity() == 1) {
            take_single_core = false;
        }
    }

    void print_stats(bench_context* ctxs, size_t ctx_size) noexcept {
        if (last_print_at == 0) {
            last_print_at = g_watcher_ctx.iops_start_at;
        }

        uint64_t iops_dur{::spdk_get_ticks() - last_print_at};
        last_print_at = ::spdk_get_ticks();
        auto tick_it = ticks.begin();
        size_t acc_size{0};
        size_t dur_size_per_core{0};

        locked = true;
        for (size_t i{0}; i < ctx_size; ++i) {
            acc_size += (ctxs[i].dur_it - ctxs[i].durs.begin());
        }

        if (ticks.size() < acc_size) {
            ticks.resize(acc_size);
            tick_it = ticks.begin();
        }

        for (size_t i{0}; i < ctx_size; ++i) {
            if (ctxs[i].durs.begin() == ctxs[i].dur_it) {
                continue;
            }

            std::copy(ctxs[i].durs.begin(), ctxs[i].dur_it, tick_it);
            dur_size_per_core = ctxs[i].dur_it - ctxs[i].durs.begin();
            ctxs[i].dur_it = ctxs[i].durs.begin();
            ctxs[i].acc_dur += dur_size_per_core;
            tick_it += dur_size_per_core;
        }
        locked = false;

        if (tick_it == ticks.begin()) {
            return;
        }

        for (auto it = ticks.begin(); it != tick_it; ++it) {
            g_watcher_ctx.acc_dur += *it;
        }
        g_watcher_ctx.acc_dur_count += acc_size;

        print_stats(ticks.begin(), tick_it - ticks.begin(), enable_print, iops_dur);
    }

    void stop_poll() {
        if (print_poller) {
            print_poller->unregister_poller();
            SPDK_NOTICELOG("the print poller has been stopped\n");
        }
    }

    void exit_thread() {
        if (take_single_core and print_thread) {
            ::spdk_set_thread(print_thread);
            ::spdk_thread_exit(print_thread);
            print_thread = nullptr;
            ::spdk_set_thread(nullptr);
            SPDK_NOTICELOG("the print thread has been exited\n");
        }
    }

public:

    bool enable_print{false};
    bool take_single_core{true};
    int64_t last_print_at{0};
    int64_t interval_us{1000};
    std::unique_ptr<utils::simple_poller> print_poller{nullptr};
    std::vector<double> ticks{};
    std::unique_ptr<size_t[]> prev_print_at{nullptr};
    ::spdk_thread* print_thread{nullptr};
    std::unique_ptr<std::vector<double>[]> dur_per_core{nullptr};
    std::unique_ptr<decltype(dur_per_core)::element_type::iterator[]> dur_end_it_per_core{nullptr};
    bool locked{false};
};

struct stop_context {
    enum class state {
        running = 1,
        monitor_stopped,
        connect_cache_stopped,
        stopping_block_clients,
        stopping_spdk_threads
    };

    state current_state{state::running};
    int64_t counter{0};
};

static char* g_conf_path{nullptr};
static std::shared_ptr<::connect_cache> conn_cache;
static std::shared_ptr<::partition_manager> par_mgr;
static std::unique_ptr<monitor::client> mon_client;
static std::string sample_data{};
static boost::property_tree::ptree g_pt{};
static bool g_force_stop{false};
static print_stats_context g_print_ctx{};
static stop_context g_stop_ctx{};

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

    auto tick = ::spdk_get_ticks();
    auto dur = static_cast<double>(tick - stack_ptr->start_tick);
    if (tick >= watcher_ctx->iops_start_at) {
        if (not g_print_ctx.locked) {
            if (bench_ctx->dur_it == bench_ctx->durs.end()) {
                bench_ctx->durs.push_back(dur);
                bench_ctx->dur_it = bench_ctx->durs.end();
            } else {
                *(bench_ctx->dur_it) = dur;
                bench_ctx->dur_it++;
            }
        }

        if(bench_ctx->deferred_count != 0){
            //到达计时点（既延期到期）
            bench_ctx->on_flight_io_count -= bench_ctx->deferred_count;
            bench_ctx->deferred_count = 0;
        }
        bench_ctx->done_io_count++;
    } else {
        bench_ctx->deferred_count++;
        if (bench_ctx->on_flight_io_count == bench_ctx->io_count) {
            //延期时间还没到，此核上的所有io已经完成
            bench_ctx->on_flight_io_count -= bench_ctx->deferred_count;
            bench_ctx->deferred_count = 0;
        }
    }
    bench_ctx->on_flight_request.erase(stack_ptr->id);

    if (watcher_ctx->infinity or bench_ctx->on_flight_io_count < bench_ctx->io_count) {
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

    auto tick = ::spdk_get_ticks();
    auto dur = static_cast<double>(tick - stack_ptr->start_tick);
    if (tick >= watcher_ctx->iops_start_at) {
        if (not g_print_ctx.locked) {
            if (bench_ctx->dur_it == bench_ctx->durs.end()) {
                bench_ctx->durs.push_back(dur);
                bench_ctx->dur_it = bench_ctx->durs.end();
            } else {
                bench_ctx->dur_it++;
                *(bench_ctx->dur_it) = dur;
            }
        }

        if(bench_ctx->deferred_count != 0){
            //到达计时点（既延时到期）
            bench_ctx->on_flight_io_count -= bench_ctx->deferred_count;
            bench_ctx->deferred_count = 0;
        }
        bench_ctx->done_io_count++;
    } else {
        bench_ctx->deferred_count++;
        if (bench_ctx->on_flight_io_count == bench_ctx->io_count) {
            //延期时间还没到，此核上的所有io已经完成
            bench_ctx->on_flight_io_count -= bench_ctx->deferred_count;
            bench_ctx->deferred_count = 0;
        }
    }
    bench_ctx->on_flight_request.erase(stack_ptr->id);

    if (watcher_ctx->infinity or bench_ctx->on_flight_io_count < bench_ctx->io_count) {
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
    auto* thd = ::spdk_thread_create(FB_FMT_1("blkbench_%1%", ::spdk_env_get_current_core()).c_str(), &cpumask);
    auto opts = msg::rdma::client::make_options(g_pt);
    ctx->blk_client = std::make_unique<::libblk_client>(
      mon_client.get(),
      thd, opts);
    SPDK_DEBUGLOG(bbench, "starting block client\n");
    ctx->blk_client->start([] () {
        SPDK_NOTICELOG("block client started on core %d\n", ::spdk_env_get_current_core());
    });

    SPDK_DEBUGLOG(bbench, "start sending rpc\n");
    SPDK_INFOLOG(bbench, "deferred_time %ld\n", watcher_ctx->deferred_time);
    watcher_ctx->iops_start_at = ::spdk_get_ticks() + watcher_ctx->deferred_time * ::spdk_get_ticks_hz();
    for (size_t i{0}; i < watcher_ctx->io_depth; ++i) {
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

void general_stop_callback() {
    switch (g_stop_ctx.current_state) {
    case stop_context::state::monitor_stopped: {
        SPDK_NOTICELOG("start stopping connect_cache\n");
        conn_cache->stop([] () {
            SPDK_NOTICELOG("The connect_cache has been stopped\n");
            g_stop_ctx.current_state = stop_context::state::connect_cache_stopped;
            general_stop_callback();
        });
        return;
    }
    case stop_context::state::connect_cache_stopped: {
        SPDK_NOTICELOG("start stopping watch_context's poller\n");
        g_watcher_ctx.watch_poller_holder.unregister_poller([] () mutable {
            SPDK_NOTICELOG("The block_bench watcher poller has been unregistered\n");
            g_stop_ctx.current_state = stop_context::state::stopping_block_clients;
            return general_stop_callback();
        });
        return;
    }
    case stop_context::state::stopping_block_clients: {
        if (g_stop_ctx.counter == static_cast<int64_t>(g_watcher_ctx.core_context_size)) {
            g_stop_ctx.counter = 0;
            g_stop_ctx.current_state = stop_context::state::stopping_spdk_threads;
            SPDK_NOTICELOG("all block_bench pollers have been stopped\n");
            return general_stop_callback();
        }

        auto index = g_stop_ctx.counter++;
        SPDK_NOTICELOG("start stopping the %ldth block client\n", index + 1);
        g_watcher_ctx.core_ctxs[index].blk_client->stop([index] () mutable {
            SPDK_NOTICELOG(
              "the %ldth block_bench poller has been stopped, all %ld\n",
              index + 1, g_watcher_ctx.core_context_size);
            g_stop_ctx.current_state = stop_context::state::stopping_block_clients;
            general_stop_callback();
        });
        return;
    }
    case stop_context::state::stopping_spdk_threads: {
        SPDK_NOTICELOG("start stopping all spdk threads\n");
        for (uint32_t i{0}; i < g_watcher_ctx.core_context_size; ++i) {
            ::spdk_set_thread(g_watcher_ctx.bench_threads[i]);
            ::spdk_thread_exit(g_watcher_ctx.bench_threads[i]);
            ::spdk_set_thread(nullptr);
            SPDK_NOTICELOG("the %dth block_bench thread has been stopped\n", i + 1);
        }
        g_print_ctx.exit_thread();
        core_sharded::stop_all();
        SPDK_NOTICELOG("all spdk threads have been stopped, stop the spdk app\n");
        ::spdk_app_stop(0);
        return;
    }
    default:
        break;
    }
}

void on_app_stop() noexcept {
    if (g_force_stop) { return; }
    SPDK_NOTICELOG("Stop the block_bench\n");
    g_force_stop = true;

    g_print_ctx.stop_poll();
    mon_client->stop([] () mutable {
        g_stop_ctx.current_state = stop_context::state::monitor_stopped;
        SPDK_NOTICELOG("The monitor client has been stopped\n");
        general_stop_callback();
    });
}

int watch_poller(void* arg) {
    if (g_force_stop) {
        return SPDK_POLLER_IDLE;
    }
    auto* ctx = reinterpret_cast<watcher_context*>(arg);
    if (ctx->is_exit) {
        return SPDK_POLLER_IDLE;
    }

    bool is_all_done{true};
    auto& core_ctxs = ctx->core_ctxs;
    for (size_t i{0}; i < ctx->core_context_size; ++i) {
        is_all_done = is_all_done && (core_ctxs[i].done_io_count == core_ctxs[i].io_count);
    }

    if (is_all_done) {
        auto iops_dur = ::spdk_get_ticks() - ctx->iops_start_at;
        g_print_ctx.stop_poll();
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

        double mean = ctx->acc_dur / ctx->acc_dur_count;
        auto fmt = boost::format("mean: %1%us") % tick_to_us(mean);
        for (size_t i{0}; i < lat_tag_count; ++i) {
            fmt = boost::format("%1%, p%2%: %3%us") % fmt % latency_tag[i] % tick_to_us(ctx->lats[i]);
        }

        auto iops_dur_sec = static_cast<double>(iops_dur) / ::spdk_get_ticks_hz();
        auto iops_val = ctx->acc_dur_count / iops_dur_sec;
        fmt = boost::format("%1%, iops: %2%, iops_dur_sec: %3%s, total_io_count: %4%") % fmt % iops_val % iops_dur_sec % ctx->acc_dur_count;
        std::cout << fmt.str() << "\n";
        std::cout << "======================================================================================\n";

        if (should_exit) {
            ctx->is_exit = true;
            on_app_stop();
        }
    }

    return SPDK_POLLER_IDLE;
}

void on_app_start(void* arg) {
    auto core_begin = core_sharded::system::begin();
    auto n_core = std::max(
      core_sharded::system::size_type{1},
      core_sharded::system::capacity());
    core_sharded::construct(core_begin, n_core, "block_bench");

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

    if (g_pt.count("infinity") != 0) {
        watcher_ctx->infinity = g_pt.get_child("infinity").get_value<bool>();
    }

    for (size_t i{0}; i < lat_tag_count * 2; ++i) {
        watcher_ctx->lats.push_back(std::numeric_limits<double>::max());
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
    g_print_ctx.from_conf(g_pt);

    if (watcher_ctx->image_name.empty()) {
        watcher_ctx->image_name = random_string(32);
    }
    watcher_ctx->io_queue_size = g_pt.get_child("io_queue_size").get_value<int32_t>();
    watcher_ctx->io_queue_request = g_pt.get_child("io_queue_request").get_value<int32_t>();
    watcher_ctx->pool_id = g_pt.get_child("pool_id").get_value<int32_t>();
    watcher_ctx->pool_name = g_pt.get_child("pool_name").get_value<std::string>();
    if(g_pt.count("deferred_time") > 0){
        watcher_ctx->deferred_time = g_pt.get_child("deferred_time").get_value<uint64_t>();
    } else {
        watcher_ctx->deferred_time = 10;
    }

    std::vector<monitor::client::endpoint> eps{};
    auto& monitors = g_pt.get_child("mon_host");
    auto pos = monitors.begin();
    for(; pos != monitors.end(); pos++){
        auto mon_addr = pos->second.get_value<std::string>();
        eps.emplace_back(std::move(mon_addr), utils::default_monitor_port);
    }

    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);

    auto opts = msg::rdma::client::make_options(g_pt);
    conn_cache = std::make_shared<::connect_cache>(&cpumask, opts);
    par_mgr = std::make_shared<::partition_manager>(-1, conn_cache);
    monitor::client::on_cluster_map_initialized_type cb = [watcher_ctx] () {
        auto n_core = ::spdk_env_get_core_count();
        if (g_print_ctx.take_single_core) {
            n_core--;
        }
        watcher_ctx->core_context_size = n_core;
        watcher_ctx->core_ctxs = std::make_unique<bench_context[]>(watcher_ctx->core_context_size);
        g_print_ctx.prev_print_at = std::make_unique<size_t[]>(watcher_ctx->core_context_size);
        std::memset(g_print_ctx.prev_print_at.get(), 0, sizeof(size_t) * watcher_ctx->core_context_size);
        g_print_ctx.dur_per_core = std::make_unique<std::vector<double>[]>(watcher_ctx->core_context_size);
        g_print_ctx.dur_end_it_per_core =
          std::make_unique<decltype(g_print_ctx.dur_end_it_per_core)::element_type[]>(watcher_ctx->core_context_size);

        watcher_ctx->read_done_cb = on_read_done;
        watcher_ctx->write_done_cb = on_write_done;
        watcher_ctx->bench_threads = std::make_unique<::spdk_thread*[]>(watcher_ctx->core_context_size);

        ::spdk_cpuset tmp_cpumask{};
        uint32_t core_no{core_sharded::system::first_core()};
        if (g_print_ctx.take_single_core) {
            core_no = core_sharded::system::next_core(core_no);
        }

        ::spdk_thread* thread{};
        uint32_t core_count{0};
        auto total_io_count = watcher_ctx->total_io_count;
        auto io_count_per_core = total_io_count / static_cast<size_t>(watcher_ctx->core_context_size);
        for (; core_no < UINT32_MAX; core_no = ::spdk_env_get_next_core(core_no)) {
            auto& ctx = watcher_ctx->core_ctxs[core_count];
            ctx.watcher_ctx = watcher_ctx;
            ctx.core = core_no;
            if (not watcher_ctx->infinity) {
                if (core_count == watcher_ctx->core_context_size - 1) {
                    ctx.io_count = total_io_count;
                    auto min_io_count = std::min(total_io_count, io_count_per_core);
                    watcher_ctx->io_depth = std::min(watcher_ctx->io_depth, min_io_count);
                } else {
                    ctx.io_count = io_count_per_core;
                }
                total_io_count -= io_count_per_core;
            }

            ::spdk_cpuset_zero(&tmp_cpumask);
            ::spdk_cpuset_set_cpu(&tmp_cpumask, core_no, true);
            std::string thread_name{(boost::format("bench_poller_%1%") % core_no).str()};
            thread = ::spdk_thread_create(thread_name.c_str(), &tmp_cpumask);
            assert(!!thread);
            ::spdk_thread_send_msg(thread, on_thread_received_msg, &ctx);
            watcher_ctx->bench_threads[core_count] = thread;
            core_count++;
        }

        ::spdk_thread* mgr_thread{watcher_ctx->bench_threads[0]};
        if (g_print_ctx.take_single_core) {
            core_no = core_sharded::system::first_core();
            SPDK_NOTICELOG("run print poller on core %u\n", core_no);
            ::spdk_cpuset_zero(&tmp_cpumask);
            ::spdk_cpuset_set_cpu(&tmp_cpumask, core_no, true);
            g_print_ctx.print_thread = ::spdk_thread_create("print_thread", &tmp_cpumask);
            mgr_thread = g_print_ctx.print_thread;
        }

        g_print_ctx.print_poller = std::make_unique<utils::simple_poller>();
        g_print_ctx.print_poller->set_thread(mgr_thread);
        g_print_ctx.print_poller->register_poller(
          print_stats_context::print_poll,
          &g_print_ctx,
          g_print_ctx.interval_us,
          "blk_bench_print");

        if(not watcher_ctx->infinity) {
            watcher_ctx->watch_poller_holder.set_thread(mgr_thread);
            watcher_ctx->watch_poller_holder.register_poller(watch_poller, watcher_ctx, 0, "blk_bench_watch");
        } else {
            SPDK_NOTICELOG("run block bench in infinity mode\n");
        }
    };

    mon_client = std::make_unique<monitor::client>(eps, reinterpret_cast<void*>(par_mgr.get()), std::nullopt, std::move(cb));
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
    opts.shutdown_cb = on_app_stop;
    rc = ::spdk_app_start(&opts, on_app_start, &g_watcher_ctx);
    if (rc) {
        SPDK_ERRLOG("ERROR: Start spdk app failed\n");
    }

    ::spdk_app_fini();
    return rc;
}
