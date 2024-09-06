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

#include <sys/sysinfo.h>
#include <numa.h>
#include <vector>
#include <map>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <optional>
#include <iostream>
#include <string>
#include <unistd.h>

#include "get_core.h"

namespace utils {

int g_numa_node = -1;

static std::map<int, int> g_core_locks;
static std::vector<int> get_all_cores() {
    auto core_count = std::min(get_nprocs(), MAX_CPU_CORES);

    std::vector<int> cores;
    if(g_numa_node >= 0){
        if(numa_available() < 0){
            std::cerr << "The system does not support numa!" << std::endl;
        } else {
            struct bitmask *cpumask = numa_allocate_cpumask();
            numa_node_to_cpus(g_numa_node, cpumask);
            for(int  core = 0; core < core_count; core++){
                if(numa_bitmask_isbitset(cpumask, core)){
                    cores.push_back(core);
                }
            }
            return std::move(cores);
        }
    }
    for(int core = 0; core < core_count; core++){
        cores.push_back(core);
    }

    return std::move(cores);
}

static std::vector<int> get_free_core(int num){
    std::vector<int> cores;
    auto all_cores = get_all_cores();
    int c = 0;
    char core_name[40];
    int core_fd;

	struct flock core_lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0,
	};

    for(auto &core : all_cores) {
        if(c >= num)
            break;
        snprintf(core_name, sizeof(core_name), "/var/tmp/spdk_core_lock_%03d", core);
        core_fd = open(core_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (core_fd == -1){
            std::cerr << "could not open " << core_name << " : " << strerror(errno) << std::endl;
            return cores;
        }

        if (fcntl(core_fd, F_SETLK, &core_lock) != 0) {
            close(core_fd);
            continue;
        }
        g_core_locks[core] = core_fd;
        cores.push_back(core);
        c++;
    }
    return std::move(cores);
}

std::optional<std::string> get_core_mask(uint64_t num) {
    std::vector<int> cores = get_free_core(num);
    if(cores.size() != num){
        std::cerr << "could not get valid core" << std::endl;
        return std::nullopt;
    }

    std::string core_mask = "[";
    for(uint64_t i = 0; i < cores.size(); i++) {
        if(i != 0)
            core_mask += ",";
        core_mask += std::to_string(cores[i]);
    }
    core_mask += "]";
    return   core_mask;
}

void unclaim_cores() {
    char core_name[40];
    for (auto [core, core_fd] : g_core_locks) {
        if(core_fd != -1) {
			snprintf(core_name, sizeof(core_name), "/var/tmp/spdk_core_lock_%03d", core);
			close(core_fd);

			g_core_locks[core] = -1;
			unlink(core_name);
		}
	}
}

int get_numa_node(){
    return numa_max_node();
}

}