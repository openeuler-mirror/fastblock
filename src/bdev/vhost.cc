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

#include <iostream>
#include "common.h"

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
								  app_parse_arg, app_usage)) !=
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
	g_app_name = "vhost";
	rc = spdk_app_start(&opts, app_run, NULL);
	spdk_app_fini();
	utils::unclaim_cores();
	clean_pid_file(pid_path.c_str());
	return rc;
}
