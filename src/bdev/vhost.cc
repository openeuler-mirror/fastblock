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

#include "fastblock/bdev/global.h"
#include "utils/get_core.h"

static const char* g_mon_cluster_endpoints = nullptr;
static const char* g_conf_path{nullptr};
boost::property_tree::ptree g_pt{};

static int g_core_num = 1;
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
    	core_sharded::construct(core_begin, core_sharded::system::capacity(), "vhost");

	global::rpc_cli_opts = msg::rdma::client::make_options(g_pt);
	auto core_no = ::spdk_env_get_current_core();
	::spdk_cpuset cpumask{};
	::spdk_cpuset_zero(&cpumask);
	::spdk_cpuset_set_cpu(&cpumask, core_no, true);
	global::conn_cache = std::make_shared<::connect_cache>(&cpumask, global::rpc_cli_opts);

    global::mon_client = std::make_unique<monitor::client>(mon_eps, nullptr);
    global::mon_client->start();
    global::mon_client->start_cluster_map_poller();

    global::blk_clients.resize(core_sharded::system::capacity());
    global::vhost_worker_threads.resize(core_sharded::system::capacity());
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
    std::cout << "run vhost with cpumask " << core_mask.value() << std::endl;
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
