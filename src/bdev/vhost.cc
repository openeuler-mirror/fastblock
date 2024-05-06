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

static const char* g_pid_path = NULL;
static const char* g_mon_cluster_endpoints = nullptr;
static const char* g_conf_path{nullptr};
boost::property_tree::ptree g_pt{};
int g_id{-1};

static void
vhost_usage(void)
{
    ::printf(" -C <path>    json configuration file path\n");
}

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

static int
vhost_parse_arg(int ch, char *arg)
{
	switch (ch)
	{

    case 'C':
        g_conf_path = arg;
        break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void vhost_started(void *arg1)
{
	std::string pid_path = "/var/tmp/vhost" + std::to_string(getpid()) + ".pid";
    save_pid(pid_path.c_str());
	auto vhost_path = "/var/tmp/bdev_vhost" + std::to_string(getpid()) + ".sock";
    SPDK_NOTICELOG_EX(
      "pid path is '%s', vhost socket path is '%s'\n",
      pid_path.c_str(), vhost_path.c_str());
    ::spdk_vhost_set_socket_path(vhost_path.c_str());

    std::vector<monitor::client::endpoint> mon_eps{};
    auto& monitors = g_pt.get_child("mon_host");
    auto pos = monitors.begin();
    for(; pos != monitors.end(); pos++){
        auto mon_addr = pos->second.get_value<std::string>();
        mon_eps.emplace_back(std::move(mon_addr), utils::default_monitor_port);
    }

    auto opts = msg::rdma::client::make_options(g_pt);
    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
    global::conn_cache = std::make_shared<::connect_cache>(&cpumask, opts);
    global::par_mgr = std::make_shared<::partition_manager>(-1, global::conn_cache);
    global::mon_client = std::make_unique<monitor::client>(mon_eps, global::par_mgr);
    global::mon_client->start();
    global::mon_client->start_cluster_map_poller();
    global::blk_client = std::make_shared<::libblk_client>(global::mon_client.get(), &cpumask, opts);
    global::blk_client->start();
}

int main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	// disable tracing because it's memory consuming
	opts.num_entries = 0;
	opts.name = "vhost";
	opts.print_level = ::spdk_log_level::SPDK_LOG_WARN;
    ::spdk_log_set_flag("libblk");
    ::spdk_log_set_flag("bdev_fastblock");
    ::spdk_log_set_flag("object_store");
	::spdk_log_set_flag("libblk");

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:", NULL,
								  vhost_parse_arg, vhost_usage)) !=
		SPDK_APP_PARSE_ARGS_SUCCESS)
	{
		exit(rc);
	}

    boost::property_tree::read_json(std::string(g_conf_path), g_pt);

	if (g_pid_path)
	{
		save_pid(g_pid_path);
	}

	rc = spdk_app_start(&opts, vhost_started, NULL);
	spdk_app_fini();
	return rc;
}
