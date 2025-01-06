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

#include "fastblock/monclient/client.h"

class partition_manager;

class monitor_client : public monitor::client {
public:
    monitor_client(
      const std::vector<monitor::client::endpoint>& endpoints,
      std::shared_ptr<partition_manager> pm,
      int osd_id,
      const size_t max_fail,
      const bool auto_reconnect,
      const std::chrono::system_clock::duration dur)
    : monitor::client(endpoints, std::nullopt, osd_id, max_fail, auto_reconnect, dur)
    , _pm(pm) {}

    void emplace_osd_boot_request(
      const int,
      const std::string&,
      const std::map<uint32_t, std::pair<uint32_t, uint32_t>>&,
      const std::string&,
      const int64_t,
      const uint32_t,
      const std::string&,
      on_response_callback_type&&);

    void send_data_statistics_request(
      std::map<std::string, utils::cluster_io> &ios);

    void load_pgs() override;

    bool is_terminate() noexcept override;

    void connect_osd(std::unique_ptr<monitor::client::response_stack> &, utils::osd_info_t &, std::shared_ptr<msg::Response>) override;
    void remove_osd_connect(int , utils::complete_fun&& ) override;

    void delete_pg_from_osd(const google::protobuf::Map<google::protobuf::int32, msg::PGInfos> &, 
            uint64_t,
            std::unordered_map<monitor::client::pg_map::pg_id_type, std::unique_ptr<utils::pg_info_type>> &) override;

    void create_pg(monitor::client::pg_map::pool_id_type pool_id, 
                  monitor::client::pg_map::version_type pool_version,
                  const msg::PGInfo &info) override;

    void remove_pg(monitor::client::pg_map::pool_id_type pool_id, 
                  monitor::client::pg_map::pg_id_type pg_id, 
                  monitor::client::pg_map::version_type pool_version) override; 

    void change_pg_membership(const msg::PGInfo &, 
                    monitor::client::pg_map::pool_id_type,
                    monitor::client::pg_map::version_type,
                    monitor::client::pg_map::pg_id_type) override; 

    void check_and_active_pg(monitor::client::pg_map::pool_id_type pool_id, monitor::client::pg_map::pg_id_type pg_id, 
                              monitor::client::pg_map::version_type pool_version, const msg::PGInfo &info) override;
    
    bool in_monitor_list(const msg::PGInfo &info) override;
private:
    std::shared_ptr<partition_manager> _pm;
};

