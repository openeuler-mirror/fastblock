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
#pragma once

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <spdk/stdinc.h>
#include <spdk/event.h>
#include <spdk/vhost.h>

#include "utils/get_core.h"


extern const char* g_mon_cluster_endpoints ;
extern const char* g_conf_path;
extern boost::property_tree::ptree g_pt;
extern int g_core_num ;
extern std::string  g_app_name;
extern bool g_app_stop;

void
app_usage(void);

extern struct option g_cmdline_opts[];

void
save_pid(const char *pid_path);

inline void clean_pid_file(const char *pid_path){
	unlink(pid_path);
}

int
app_parse_arg(int ch, char *arg);

void fb_client_init(std::optional<std::function<void()>> &&cb, bool should_create_pm = false);
void app_run(void *arg1);

void app_stop();