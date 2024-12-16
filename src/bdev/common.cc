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
#include "common.h"
#include "fastblock/base/core_sharded.h"
#include "fastblock/client/libfblock.h"


const char* g_mon_cluster_endpoints = nullptr;
const char* g_conf_path{nullptr};
boost::property_tree::ptree g_pt{};
int g_core_num = 1;
std::string  g_app_name;

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


void app_run(void *arg1)
{
    libblk_client_create_context(g_pt);
}
