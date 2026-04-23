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
#include "mon_client.h"
#include "partition_manager.h"

void monitor_client::load_pgs() {
    SPDK_INFOLOG(osd, "load pgs\n");
    std::map<uint64_t, std::vector<utils::pg_info_type>> pools;
    if (_pm) {
        _pm->load_pgs_map(pools);
    }
    for(auto& [pool_id, infos] : pools){
        _pg_map.pool_version[pool_id] = 0;
        auto [it, _] = _pg_map.pool_pg_map.try_emplace(pool_id);
        auto& pgs = it->second;
        for(auto& info : infos){
            pgs[info.pg_id] = std::make_unique<utils::pg_info_type>(std::move(info));
        }
    }
}

/*
   sharded_ports  <shard_id, <core_id, rdma_port, raw_port>>>
*/
void monitor_client::emplace_osd_boot_request(
  const int osd_id,
  const std::string& osd_addr,
  const std::map<uint32_t, utils::core_shard_map>& sharded_ports,
  const std::string& osd_uuid,
  const int64_t size,
  const uint32_t core_num,
  const std::string& config,
  on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* boot_req = req->mutable_boot_request();
    boot_req->set_osd_id(osd_id);
    boot_req->set_uuid(osd_uuid.c_str());
    boot_req->set_address(osd_addr.c_str());
    boot_req->set_config(config);
    auto* proto_sharded_ports = boot_req->mutable_sharded_ports();
    for (auto it = sharded_ports.begin(); it != sharded_ports.end(); ++it) {
        msg::ShardCore shard_core;
        shard_core.set_coreid(it->second.core_id);
        shard_core.set_port(it->second.port);
        shard_core.set_raw_port(it->second.raw_port);
        proto_sharded_ports->insert({it->first, std::move(shard_core)});
    }
    boot_req->set_size(size);
    boot_req->set_core_num(core_num);
    char hostname[1024];
    ::gethostname(hostname, sizeof(hostname));
    boot_req->set_host(hostname);

    auto* req_ctx = new client::request_context{
      this, std::move(req), std::monostate{}, std::move(cb)};
    enqueue_request(req_ctx);
}

void monitor_client::send_data_statistics_request(
  std::map<std::string, utils::cluster_io> &ios) {
    auto req = std::make_unique<msg::Request>();
    auto data_req = req->mutable_data_statistics_request();
    auto data = data_req->mutable_data();
    for (auto &[pg_name, io] : ios) {
        msg::DataStatistics ds;
        ds.set_read_ios(io.read_ios);
        ds.set_read_bytes(io.read_bytes);
        ds.set_write_ios(io.write_ios);
        ds.set_write_bytes(io.write_bytes);
        ds.set_objects(io.objects);
        (*data)[pg_name] = std::move(ds);
    }

    auto cb = [](const response_status status, request_context*ctx){
        SPDK_DEBUGLOG(osd, "send data_statistics finish.\n");
        if (status != response_status::ok) {
            SPDK_ERRLOG("send data_statistics finish failed\n");
        }
    };

    auto* req_ctx = new client::request_context{
      this, std::move(req), std::monostate{}, std::move(cb)};
    enqueue_request(req_ctx);
}

bool monitor_client::is_terminate() noexcept {
    if(_is_terminate)
        return true;
    if(_pm && _pm->is_terminated())
        return true;
    return false;
}

void monitor_client::connect_osd(std::unique_ptr<monitor::client::response_stack> &resp_stack, 
                            utils::osd_info_t &osd_info,
                            std::shared_ptr<msg::Response> response) {
    if (not resp_stack) {
        resp_stack = std::make_unique<response_stack>(response, 0);
    }

    for (auto it = osd_info.sharded_ports.begin(); it != osd_info.sharded_ports.end(); ++it) {
        auto shard_id = it->first;
        auto core_port = it->second;

         SPDK_INFOLOG(osd, 
          "Starting connect to osd %s:%d, shard %d, core %d\n",
          osd_info.address.c_str(), core_port.port, shard_id, core_port.core_id);

        _pm->get_pg_group().create_connect(
          osd_info.node_id,
          shard_id,
          osd_info.address,
          core_port.port,
          [this, node_id = osd_info.node_id, raw_stack = resp_stack.get()] (void *, int res) {
              if (res != err::E_SUCCESS) {
                  SPDK_ERRLOG("ERROR: Connect failed\n");
                  auto it = _osd_map.data.find(node_id);
                  if (it != _osd_map.data.end()) {
                      it->second->isup = false;
                  }
              }

              raw_stack->un_connected_count--;
              SPDK_DEBUGLOG(osd, "Connected, un-connected count is %ld\n",
                raw_stack->un_connected_count);
          }
        );
        resp_stack->un_connected_count++;
    }    
}

void monitor_client::remove_osd_connect(int node_id, utils::complete_fun&& conn_cb) {
    _pm->get_pg_group().remove_connect(node_id, std::move(conn_cb));
}

void monitor_client::remove_pg(monitor::client::pg_map::pool_id_type pool_id, 
                monitor::client::pg_map::pg_id_type pg_id, 
                monitor::client::pg_map::version_type pool_version){
    auto ctx = new monitor::delete_pg_context{.cli = this, .pool_id = pool_id, .pg_id = pg_id, .pool_version = pool_version};
    
    _pg_map.set_pool_update(pool_id, pg_id, pool_version, 1);
    SPDK_DEBUGLOG(osd, "remove pg %d.%d\n", pool_id, pg_id);
    _pm->delete_partition(pool_id, pg_id, monitor::delete_pg_done, ctx);
}

void monitor_client::delete_pg_from_osd(const google::protobuf::Map<google::protobuf::int32, msg::PGInfos> &pgs, 
            uint64_t pool_id,
            std::unordered_map<monitor::client::pg_map::pg_id_type, std::unique_ptr<utils::pg_info_type>> &pg_infos) {
    auto info_it = pgs.find(pool_id);
    if (info_it == pgs.end()){
        return;
    }
    bool contain = false;
    auto current_osdid = _pm->get_current_node_id();
    auto pool_version = _pg_map.pool_version[pool_id];

    for (auto& info : info_it->second.pi()){
        auto pgid = info.pgid();
        auto pg_item = pg_infos.find(pgid);
        if(pg_item == pg_infos.end()){
            continue;
        }
        for (auto osd_id : info.osdid()) {
            if(osd_id == current_osdid){
                contain = true;
                break;
            }
        }
        if(contain)
            continue;

        contain = false;
        for(auto osd_id : pg_item->second->osds){
            if(osd_id == current_osdid){
                contain = true;
                break;
            }
        }
        if(!contain)
            continue;
        /* 当前osd在本地pgmap中pg的osd列表中，但不在monitor的pgmap的osd列表中，说明当前osd已经从pg中
         * 移除（可能是osd out，monitor把它从pg中移除，但osd本地pg信息还未清除）,需要删除此osd上的pg信息。
         */
        remove_pg(pool_id, pgid, pool_version);
    }    
}

void monitor_client::create_pg(monitor::client::pg_map::pool_id_type pool_id, 
              std::string& pool_name,
              monitor::client::pg_map::version_type pool_version,
              const msg::PGInfo &info) {
    int current_osdid = _pm->get_current_node_id();
    bool in_pg = false;
    for(auto osd_id : info.osdid()){
        if(osd_id == current_osdid){
            in_pg = true;
            break;
        }
    }
    if(!in_pg){
        for(auto osd_id : info.newosdid()){
            if(osd_id == current_osdid){
                in_pg = true;
                break;
            }
        }
        if(!in_pg){
            SPDK_INFOLOG(osd, "current osd %d not in pg %d\n", current_osdid, info.pgid());
            return;
        }
    }

    _pg_map.set_pool_update(pool_id, info.pgid(), pool_version, 1);
    auto new_pg_done = [this, pool_id, pool_name, pool_version, pg_id = info.pgid(), pg_version = info.version(), osds = info.osdid()]
      (void *, int perrno){
        if(perrno != 0){
            SPDK_ERRLOG("create pg %d.%d failed: %s\n", pool_id, pg_id, spdk_strerror(perrno));
            _pg_map.set_pool_update(pool_id, pg_id, pool_version, -1);
            return;
        }

        auto pit = std::make_unique<utils::pg_info_type>();
        pit->pg_id = pg_id;
        pit->version = pg_version;
        std::string osd_str;
        for (auto osd_id : osds) {
            pit->osds.push_back(osd_id);
            osd_str += std::to_string(osd_id) + ",";
        }

        SPDK_DEBUGLOG(osd, "core [%u] pool: %d pg: %lu version: %ld  osd_list: %s\n",
          ::spdk_env_get_current_core(), pool_id, pit->pg_id, pit->version, osd_str.data());

        _pg_map.pool_pg_map[pool_id].emplace(pit->pg_id, std::move(pit));
        _pg_map.set_pool_update(pool_id, pg_id, pool_version, 0);
        _pg_map.add_pools(pool_id, pool_name);
    };  

    std::vector<utils::osd_info_t> osds{};
    for (auto osd_id : info.osdid()) {
        if(_osd_map.data.find(osd_id) == _osd_map.data.end()){
            SPDK_WARNLOG("not find osd %d in osdmap\n", osd_id);
            new_pg_done(nullptr, -err::E_NODEV);
            return;
        }
        osds.push_back(*(_osd_map.data.at(osd_id)));
    }
    _pm->create_partition(pool_id, info.pgid(), info.coreindex() ,std::move(osds), 0, std::move(new_pg_done), nullptr);  
}

class change_membership_complete : public utils::context {
public:
    friend monitor::client;
    change_membership_complete(uint64_t pool_id, uint64_t pg_id, int64_t version, std::vector<int32_t> osds, monitor::client* cli)
    : _pool_id(pool_id)
    , _pg_id(pg_id)
    , _version(version)
    , _osds(std::move(osds))
    , _client(cli) {}

    void finish(int r) override {
        SPDK_INFOLOG(osd, "change membership of pg %lu.%lu version: %ld finish: result %d\n", _pool_id, _pg_id, _version, r);
        if(r != 0) {
            _client->get_pg_map().set_pool_update(_pool_id, _pg_id, _version, -1);
            return;
        }
        if(!_client->get_pg_map().pool_pg_map[_pool_id].contains(_pg_id)){
            SPDK_INFOLOG(osd, "pg %lu.%lu not in pool_pg_map", _pool_id, _pg_id);
            return;
        }
        auto& pit = _client->get_pg_map().pool_pg_map[_pool_id][_pg_id];
        pit->version = _version;
        pit->osds = std::exchange(_osds, {});
        _client->get_pg_map().set_pool_update(_pool_id, _pg_id, _version, 0);
    }
private:
    uint64_t _pool_id;
    uint64_t _pg_id;
    int64_t _version;
    std::vector<int32_t> _osds;
    monitor::client* _client;
};

void monitor_client::change_pg_membership(const msg::PGInfo &info, 
                        monitor::client::pg_map::pool_id_type pool_id,
                        monitor::client::pg_map::version_type pool_version,
                        monitor::client::pg_map::pg_id_type pgid) {
    std::vector<int32_t> new_osds;
    std::string osd_str;
    std::vector<utils::osd_info_t> new_osd_infos;

    for (auto osd_id : info.newosdid()){
        new_osds.push_back(osd_id);
        osd_str += std::to_string(osd_id) + ",";
        auto& osd_info = _osd_map.data[osd_id];
        new_osd_infos.emplace_back(*osd_info);
    }

    SPDK_INFOLOG(osd, "pool: %d  pg: %d version: %ld  osd_list: %s\n",
            pool_id,  pgid, info.version(), osd_str.data());
    auto complete = new change_membership_complete(pool_id, pgid, info.version(), new_osds, this);
    _pg_map.set_pool_update(pool_id, pgid, pool_version, 1);
    _pm->change_pg_membership(pool_id, pgid, new_osd_infos, complete);    
}

//info为从monitor收到的pg的信息
void monitor_client::check_and_active_pg(monitor::client::pg_map::pool_id_type pool_id, 
                                    monitor::client::pg_map::pg_id_type pg_id, 
                                    monitor::client::pg_map::version_type pool_version, 
                                    const msg::PGInfo &info) {
    auto& pit = _pg_map.pool_pg_map[pool_id][pg_id];
    if(pit->version != 0)
        return;

    //出现这种情况是当前osd刚重启，恢复处理的_pg_map中pg的版本都是0，这时需要激活pg
    
    
    bool pg_is_remap = (info.state() & PgRemapped) != 0;
    auto active_pg_done = [this, pool_id, pool_version, pg_id = info.pgid(), pg_version = info.version(), pg_is_remap]
      (void *arg, int perrno){
        if(perrno != 0){
            SPDK_ERRLOG("activate pg %d.%d failed: %s\n", pool_id, pg_id, spdk_strerror(perrno));
            if(!pg_is_remap)
                _pg_map.set_pool_update(pool_id, pg_id, pool_version, -1);
            return;
        }
        if(!pg_is_remap){
            _pg_map.set_pool_update(pool_id, pg_id, pool_version, 0);
            _pg_map.pool_pg_map[pool_id][pg_id]->version = pg_version;
        }
        SPDK_INFOLOG(osd, "activate pg %d.%d done, pg_version %ld pg_is_remap %d\n",
                pool_id, pg_id, pg_version, pg_is_remap);
    };
    if(!pg_is_remap){
        _pg_map.set_pool_update(pool_id, info.pgid(), pool_version, 1);
    }
    
    _pm->active_partition(pool_id, info.pgid(), std::move(active_pg_done), nullptr);
}

/*  
 * 检查当前osd是否在从monitor收到的pg的osd成员列表中
 *   info： 从monitor收到的pg的信息
 */
bool monitor_client::in_monitor_list(const msg::PGInfo &info){
    bool contain_in_monitor = false;

    auto current_osdid = _pm->get_current_node_id();

    for (auto osd_id : info.osdid()){
        if(osd_id == current_osdid){
            contain_in_monitor = true;
        }
    }

    return contain_in_monitor;
}
