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

#include <spdk/thread.h>
#include <memory>
#include <map>

#include "utils/utils.h"

namespace monitor{
    class client;
}

class data_statistics{
public:
    data_statistics();

    void stop() {
        spdk_poller_unregister(&_timer);
    }

    void set_mon_client(std::shared_ptr<monitor::client> mon_client){
        _mon_client = mon_client;
    }

    void add_data(utils::operation_type type,  std::string &pg_name, uint64_t bytes);
    void insert_data(utils::operation_type type,  std::string pg_name, uint64_t bytes) {
        if(type == utils::operation_type::READ){
            auto it = _ios.find(pg_name);
            if(it != _ios.end()){
                it->second.read_ios++;
                it->second.read_bytes += bytes;
            }else{
                _ios[pg_name] = utils::cluster_io{.read_ios = 1, .read_bytes = bytes};
            }
        }else if(type == utils::operation_type::WRITE){
            auto it = _ios.find(pg_name);
            if(it != _ios.end()){
                it->second.write_ios++;
                it->second.write_bytes += bytes;
            }else{
                _ios[pg_name] = utils::cluster_io{.write_ios = 1, .write_bytes = bytes};
            }
        }
    }

    void send_data_to_mon();
private:
    struct spdk_thread * _thread;
    std::shared_ptr<monitor::client> _mon_client;
    std::map<std::string, utils::cluster_io> _ios;
    struct spdk_poller * _timer;
};

extern std::shared_ptr<data_statistics> g_data_statistics;