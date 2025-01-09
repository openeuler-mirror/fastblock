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

#include <optional>
#include "fastblock/bdev/global.h"
#include "common.h"

SPDK_LOG_REGISTER_COMPONENT(common)

const char* g_mon_cluster_endpoints = nullptr;
const char* g_conf_path{nullptr};
boost::property_tree::ptree g_pt{};
int g_core_num = 1;
std::string  g_app_name;
bool g_app_stop{false};

struct app_stop_context {
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

static app_stop_context g_app_stop_ctx{};

void
app_usage(void)
{
    printf("-C, --conf <path>    json configuration file path\n");
    printf("-N, --numa-node <id> the id of numa node\n");
    printf("-S, --core-num <num> the number of cpu core\n");
}

struct option g_cmdline_opts[] = {
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


void
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

int
app_parse_arg(int ch, char *arg)
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
	    app_usage();
		return -EINVAL;
	}
	return 0;
}

void fb_client_init(std::optional<std::function<void()>> &&cb) 
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
    core_sharded::construct(core_begin, core_sharded::system::capacity(), g_app_name);

	global::rpc_cli_opts = msg::rdma::client::make_options(g_pt);
	auto core_no = ::spdk_env_get_current_core();
	::spdk_cpuset cpumask{};
	::spdk_cpuset_zero(&cpumask);
	::spdk_cpuset_set_cpu(&cpumask, core_no, true);
	global::conn_cache = std::make_shared<::connect_cache>(&cpumask, global::rpc_cli_opts);

	global::mon_client = std::make_unique<monitor::client>(mon_eps, std::move(cb));
    global::mon_client->start();
    global::mon_client->start_cluster_map_poller();

    global::blk_clients.resize(core_sharded::system::capacity() + 1);
    global::app_thread_shard_id = core_sharded::system::capacity();
    global::vhost_worker_threads.resize(core_sharded::system::capacity() + 1);
}

void app_run(void *)
{
	fb_client_init(std::nullopt);
}

static void general_stop(){
    switch (g_app_stop_ctx.current_state) {
    case app_stop_context::state::monitor_stopped: {
        global::conn_cache->stop([]() mutable {
			SPDK_INFOLOG(common, "The connect_cache has been stopped\n");
			g_app_stop_ctx.current_state = app_stop_context::state::connect_cache_stopped;
			general_stop();
		});
		return;
	}
	case app_stop_context::state::connect_cache_stopped: {
        if(g_app_stop_ctx.counter == static_cast<int64_t>(global::blk_clients.size())){
            g_app_stop_ctx.counter = 0;
			g_app_stop_ctx.current_state = app_stop_context::state::stopping_spdk_threads;
			SPDK_INFOLOG(common, "The block_bench has been stopped\n");
			return general_stop();
		}

		auto index = g_app_stop_ctx.counter++;
		SPDK_INFOLOG(common, "start stopping the %ldth block client\n", index);
		if(global::blk_clients[index]){
			auto blk_thread = global::blk_clients[index]->get_blk_thread();
			global::blk_clients[index]->stop([index, blk_thread]() mutable {
				SPDK_INFOLOG(common, 
				        "the %ldth block_bench poller has been stopped, all %lu\n",
				        index, global::blk_clients.size());
				if(blk_thread != spdk_thread_get_app_thread()){
					spdk_thread_exit(blk_thread);
				}
				g_app_stop_ctx.current_state = app_stop_context::state::connect_cache_stopped;  
				general_stop();   
			});
		} else {
			SPDK_INFOLOG(common, 
                    "the %ldth block_bench poller has been stopped, all %lu\n",
                    index, global::blk_clients.size());
			g_app_stop_ctx.current_state = app_stop_context::state::connect_cache_stopped;  
			general_stop();
		}
		return;
	}
	case app_stop_context::state::stopping_spdk_threads: {
		SPDK_INFOLOG(common, "start stopping spdk threads\n");
		core_sharded::stop_all();
		
		spdk_app_stop(0);
		return;
	}
	default:
	    break;
	}
}

void app_stop(){
    if(g_app_stop)
	    return;
	SPDK_INFOLOG(common, "stop the app.\n");
	g_app_stop = true;
	global::mon_client->stop([](){
        g_app_stop_ctx.current_state = app_stop_context::state::monitor_stopped;
		SPDK_INFOLOG(common, "The monitor client has been stopped\n");
		general_stop();
	});
}

