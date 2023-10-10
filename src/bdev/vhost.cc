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

#include <spdk/stdinc.h>
#include <spdk/event.h>
#include <spdk/vhost.h>

#include <boost/algorithm/string.hpp>

#include "global.h"

static const char *g_pid_path = NULL;
static const char *g_mon_cluster_endpoints = nullptr;

static void
vhost_usage(void)
{
	printf(" -f <path>                 save pid to file under given path\n");
	printf(" -S <path>                 directory where to create vhost sockets (default: pwd)\n");
    printf(" -M <endpoints list>       format: monitor_node1_addr:monitor_node1_port,monitor_node2_addr:monitor_node2_port,monitor_node3_addr:monitor_node3_port...\n");
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
	case 'f':
		g_pid_path = arg;
		break;
	case 'S':
		spdk_vhost_set_socket_path(arg);
		break;
    case 'M':
        g_mon_cluster_endpoints = arg;
        break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void vhost_started(void *arg1)
{
    std::string mon_endpoints_raw(g_mon_cluster_endpoints);

    std::vector<std::string> split_result{};
    boost::split(split_result, mon_endpoints_raw, boost::is_any_of(","));
    if (split_result.empty()) {
        SPDK_ERRLOG("parse monitor cluster endpoints error\n");
        exit(-EINVAL);
    }

    std::vector<monitor::client::endpoint> mon_eps{};
    std::vector<std::string> split_ep_result{};
    for (auto& ep_raw : split_result) {
        boost::split(split_ep_result, ep_raw, boost::is_any_of(":"));
        if (split_ep_result.size() != 2) {
            SPDK_ERRLOG("parse monitor cluster endpoints error\n");
            exit(-EINVAL);
        }

        auto& host = split_ep_result[0];
        auto port = std::stoi(split_ep_result[1]);
        mon_eps.emplace_back(host, port);
        SPDK_DEBUGLOG(bdev_fastblock, "monitor node: %s:%d\n", host.c_str(), port);
        split_ep_result.clear();
    }

    global::par_mgr = std::make_shared<::partition_manager>(-1);
    global::mon_client = std::make_unique<monitor::client>(mon_eps, global::par_mgr);
    global::mon_client->start();
    global::mon_client->start_cluster_map_poller();
    global::blk_client = std::make_shared<::libblk_client>(global::mon_client.get());
    global::blk_client->start();
}

int main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "vhost";
	opts.print_level = ::spdk_log_level::SPDK_LOG_WARN;
    ::spdk_log_set_flag("libblk");
    ::spdk_log_set_flag("bdev_fastblock");
    ::spdk_log_set_flag("object_store");
	::spdk_log_set_flag("libblk");
	::spdk_log_set_flag("bdev_fastblock");

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:S:M:", NULL,
								  vhost_parse_arg, vhost_usage)) !=
		SPDK_APP_PARSE_ARGS_SUCCESS)
	{
		exit(rc);
	}

	if (g_pid_path)
	{
		save_pid(g_pid_path);
	}

	rc = spdk_app_start(&opts, vhost_started, NULL);
	spdk_app_fini();
	return rc;
}
