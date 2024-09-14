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

#include <spdk/stdinc.h>
#include <spdk/event.h>
#include <spdk/vhost.h>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "global.h"
#include "utils/get_core.h"

#include <csignal>

#include <vector>
#include <chrono>

static const char* g_mon_cluster_endpoints = nullptr;
static const char* g_conf_path{nullptr};
boost::property_tree::ptree g_pt{};

static int g_core_num = 1;

/********************************** test **********************************************/
static int iops_count = 0;
static int evt_count = 0;
bool g_start_is_init{false};
std::chrono::system_clock::time_point g_iops_start_at;
bool g_is_rpc_done{false};
std::chrono::system_clock::time_point g_iops_without_rpc_start_at;
std::string g_obj_name = "test";
std::vector<uint64_t> g_call_count{};
std::vector<std::unique_ptr<utils::simple_poller>> g_pollers{};
bool g_is_all_iops_print{false};
/********************************** test **********************************************/
static void
vhost_usage(void)
{
    printf("-C, --conf <path>    json configuration file path\n");
    printf("-N, --numa-node <id> the id of numa node\n");
    printf("-S, --core-num <num> the number of cpu core\n");
}

static struct option g_cmdline_opts[] = {
#define BLOCK_OPTION_CONF 'C'
    {
        .name = "conf",
        .has_arg = 1,
        .flag = NULL,
        .val = BLOCK_OPTION_CONF,
    },
#define BLOCK_OPTION_NUMA_NODE 'N'
    {
        .name = "numa-node",
        .has_arg = 1,
        .flag = NULL,
        .val = BLOCK_OPTION_NUMA_NODE,
    },
#define BLOCK_OPTION_CORE_NUM 'S'
	{
		.name = "core-num",
		.has_arg = 1,
		.flag = NULL,
		.val = BLOCK_OPTION_CORE_NUM,
	},
    {
        .name = NULL
    }
};

static void
save_pid(const char *pid_path)
{
    FILE *pid_file;

    pid_file = fopen(pid_path, "w");
    if (pid_file == NULL)
    {
        fprintf(stderr, "Couldn't create pid file '%s': %s\n", pid_path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    fprintf(pid_file, "%d\n", getpid());
    fclose(pid_file);
}

static inline void clean_pid_file(const char *pid_path){
    unlink(pid_path);
}

static int
vhost_parse_arg(int ch, char *arg)
{
    switch (ch)
    {

    case BLOCK_OPTION_CONF:
        g_conf_path = arg;
        break;
    case BLOCK_OPTION_NUMA_NODE:
        utils::g_numa_node = atoi(arg);
        break;
    case BLOCK_OPTION_CORE_NUM:
        g_core_num = atoi(arg);
        break;
    default:
        vhost_usage();
        return -EINVAL;
    }
    return 0;
}

static int bench_poll(void* arg) {
    static __thread uint32_t count_index = spdk_env_get_current_core() - spdk_env_get_first_core();

    if (fastblock::global::mon_client->is_pg_map_empty()) {
        return SPDK_POLLER_IDLE;
    }

    if (not g_start_is_init) {
        g_start_is_init = true;
        SPDK_NOTICELOG("start bench_fn\n");
        g_iops_start_at = std::chrono::system_clock::now();
    }

    auto dur = std::chrono::system_clock::now() - g_iops_start_at;
    if (g_call_count[count_index] == 5000000) {
        auto iops_sec = static_cast<double>(dur.count()) / 1000 / 1000 / 1000;
        auto iops = g_call_count[count_index]  / iops_sec;
        SPDK_ERRLOG(
          "*************************** iops: %lf, iops_count: %ld, core_id: %d ***************************\n",
          iops, g_call_count[count_index] , spdk_env_get_current_core());
        SPDK_ERRLOG(
          "*************************** duration: %ldus/%lfs ***************************\n\n",
          dur.count() / 1000, iops_sec);

        if (not g_is_all_iops_print) {
            g_is_all_iops_print = true;

            for (auto it = std::begin(g_call_count); it != std::end(g_call_count); ++it) {
                iops_count += *it;
            }

            auto iops_sec = static_cast<double>(dur.count()) / 1000 / 1000 / 1000;
            auto iops = iops_count  / iops_sec;

            SPDK_ERRLOG(
              "=========================== total iops: %lf, iops_count: %d, core_count: %d ===========================\n",
              iops, iops_count, core_sharded::system::capacity());
            SPDK_ERRLOG(
              "=========================== duration: %ldus/%lfs ===========================\n\n",
              dur.count() / 1000, iops_sec);
        }

        for (auto it = g_pollers.begin(); it != g_pollers.end(); ++it) {
            it->get()->unregister_poller();
            spdk_set_thread(it->get()->get_thread());
            spdk_thread_exit(it->get()->get_thread());
            spdk_set_thread(nullptr);
        }

        return SPDK_POLLER_IDLE;
    }

    fastblock::global::conn_pool->get_leader_stub(
      1, 0, g_obj_name,
      [idx = count_index] (osd::rpc_service_osd_Stub* _) {
          g_call_count[idx]++;
      }
    );

    return SPDK_POLLER_BUSY;
}

static void poller_bench_fn(std::vector<uint32_t> cores) {
    for (auto it = cores.begin(); it != cores.end(); ++it) {
        auto mask = core_sharded::make_cpumask(*it);
        auto* thread = spdk_thread_create(FMT_1("poller_bench_%1%", *it).c_str(), mask.get());
        auto poller = std::make_unique<utils::simple_poller>(thread);
        poller->register_poller(bench_poll, nullptr, 0);
        g_pollers.push_back(std::move(poller));
    }
}

static void bench_fn(void* arg1, void* arg2) {
    if (fastblock::global::mon_client->is_pg_map_empty()) {
        auto* evt = ::spdk_event_allocate(::spdk_env_get_first_core(), bench_fn, nullptr, nullptr);
        ::spdk_event_call(evt);
        return;
    }

    if (not g_start_is_init) {
        g_start_is_init = true;
        SPDK_NOTICELOG("start bench_fn\n");
        g_iops_start_at = std::chrono::system_clock::now();
    }

    static __thread uint32_t next_lcore = UINT32_MAX;
    static __thread uint32_t count_index = spdk_env_get_current_core() - spdk_env_get_first_core();
    if (next_lcore == UINT32_MAX) {
        next_lcore = spdk_env_get_next_core(spdk_env_get_current_core());
        if (next_lcore == UINT32_MAX) {
            next_lcore = spdk_env_get_first_core();
        }
    }

    if (g_call_count[0] != 2500000) {
        fastblock::global::conn_pool->get_leader_stub(
            1, 0, g_obj_name,
            [next = next_lcore, idx = count_index] (osd::rpc_service_osd_Stub* _) {
                g_call_count[idx]++;
                auto* evt = ::spdk_event_allocate(next, bench_fn, nullptr, nullptr);
                ::spdk_event_call(evt);
           }
        );
        return;
    }

    for (auto it = std::begin(g_call_count); it != std::end(g_call_count); ++it) {
        iops_count += *it;
    }

    auto dur = std::chrono::system_clock::now() - g_iops_start_at;
    auto iops_sec = static_cast<double>(dur.count()) / 1000 / 1000 / 1000;
    auto iops = iops_count / iops_sec;
    SPDK_ERRLOG(
      "*************************** iops: %lf, iops_count: %d, core_count: %d ***************************\n",
      iops, iops_count, g_core_num);
    SPDK_ERRLOG(
      "*************************** duration: %ldus/%lfs ***************************\n",
      dur.count() / 1000, iops_sec);

    auto without_rpc_dur = std::chrono::system_clock::now() - g_iops_start_at;
    auto without_rpc_iops_sec = (static_cast<double>(dur.count()) - 25346000.0) / 1000 / 1000 / 1000;
    auto without_rpc_iops = (iops_count - 1) / without_rpc_iops_sec;
    SPDK_ERRLOG("*************************** without_rpc_iops: %lf ***************************\n", without_rpc_iops);
    SPDK_ERRLOG(
      "*************************** without_rpc_duration: %ldus/%lfs ***************************\n",
      without_rpc_dur.count() / 1000, without_rpc_iops_sec);
}

static void vhost_started(void *arg1)
{
    std::vector<monitor::client::endpoint> mon_eps{};
    auto &monitors = g_pt.get_child("mon_host");
    auto pos = monitors.begin();
    for (; pos != monitors.end(); pos++)
    {
        auto mon_addr = pos->second.get_value<std::string>();
        mon_eps.emplace_back(std::move(mon_addr), utils::default_monitor_port);
    }


        auto core_begin = core_sharded::system::begin();
        auto n_core = std::max(
          core_sharded::system::size_type{1},
          core_sharded::system::capacity() - 1);
        core_sharded::construct(core_begin, n_core, "vhost");

    auto opts = msg::rdma::client::make_options(g_pt);
    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
    fastblock::global::conn_cache = std::make_shared<::connect_cache>(&cpumask, opts);
    fastblock::global::par_mgr = std::make_shared<::partition_manager>(-1, fastblock::global::conn_cache);
    fastblock::global::mon_client = std::make_unique<monitor::client>(mon_eps, fastblock::global::par_mgr);
    fastblock::global::mon_client->start();
    fastblock::global::mon_client->start_cluster_map_poller();
    SPDK_NOTICELOG("monitor client started\n");

    std::vector<core_sharded::core_id_type> workers_core{};
    if (core_sharded::system::capacity() == 1) {
        workers_core.push_back(core_sharded::system::first_core());
    } else {
        auto app_core = core_sharded::system::current_core();
        auto core_it = core_sharded::system::begin();
        for (; core_it != core_sharded::system::end(); ++core_it) {
            // if (*core_it == app_core) {
            //     continue;
            // }
            workers_core.push_back(*core_it);
        }
    }

    fastblock::global::pg_cli = std::make_unique<fastblock::client::pg_client>(
      workers_core.front(),
      ::spdk_get_thread(),
      fastblock::global::mon_client.get());
    fastblock::global::pg_cli->start([]{});
    SPDK_NOTICELOG("pg client started\n");

    SPDK_NOTICELOG(
      "vhost workers core size is %ld, system available cores size is %d\n",
      workers_core.size(), core_sharded::system::capacity());
    fastblock::global::conn_pool = std::make_unique<fastblock::client::connection_pool>(
      workers_core.size(),
      workers_core,
      opts,
      fastblock::global::mon_client.get(),
      fastblock::global::pg_cli.get());
    fastblock::global::conn_pool->start([workers_core] (bool success) mutable {
        if (not success) {
            SPDK_ERRLOG("the connection pool starts error\n");
            std::raise(SIGINT);
            return;
        }

        SPDK_NOTICELOG("the connection started\n");

        fastblock::global::blk_cli_pool = std::make_unique<fastblock::client::block_client_pool>(
          ::spdk_get_thread(),
          fastblock::global::mon_client.get(),
          fastblock::global::pg_cli.get(),
          fastblock::global::conn_pool.get(),
          workers_core.size(),
          workers_core);

        fastblock::global::bdev_blk_cli = std::make_unique<fastblock::client::bdev_block_client>(
          workers_core.front(),
          fastblock::global::blk_cli_pool.get());

        SPDK_NOTICELOG("vhost started\n");

        SPDK_ERRLOG("start sending leader query request...\n");
        if (g_core_num % 2 == 0) {
            g_call_count.resize(g_core_num);
            for (auto i = 0; i < g_core_num; i++) {
                g_call_count[i] = 0;
            }
            // bench_fn(nullptr, nullptr);
            poller_bench_fn(workers_core);
        }
    });
}

int main(int argc, char *argv[])
{
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    // disable tracing because it's memory consuming
    opts.num_entries = 0;
    opts.name = "vhost";
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;

    if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:N:S:", g_cmdline_opts,
                                  vhost_parse_arg, vhost_usage)) !=
        SPDK_APP_PARSE_ARGS_SUCCESS)
    {
        exit(rc);
    }

    if(utils::g_numa_node >= 0){
        int max_node = utils::get_numa_node();
        if(utils::g_numa_node > max_node){
            std::cerr << "invalid --numa-node " << utils::g_numa_node << ": valid range [0, " << max_node << "]" << std::endl;
            return -EINVAL;
        }
    }

    auto core_mask = utils::get_core_mask(g_core_num);
    if(!core_mask){
        return -EINVAL;
    }
    std::string reactor_mask = core_mask.value();
    opts.reactor_mask = reactor_mask.c_str();

    boost::property_tree::read_json(std::string(g_conf_path), g_pt);

    std::string pid_path = "/var/tmp/vhost" + std::to_string(getpid()) + ".pid";
    save_pid(pid_path.c_str());
    auto vhost_path = "/var/tmp/bdev_vhost_" + std::to_string(getpid()) + ".sock";
    SPDK_NOTICELOG(
        "pid path is '%s', vhost socket path is '%s'\n",
        pid_path.c_str(), vhost_path.c_str());

    opts.rpc_addr = vhost_path.c_str();
    rc = spdk_app_start(&opts, vhost_started, NULL);
    spdk_app_fini();
    utils::unclaim_cores();
    clean_pid_file(pid_path.c_str());
    return rc;
}
