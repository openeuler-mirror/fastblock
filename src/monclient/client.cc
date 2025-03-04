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

#include "fastblock/monclient/client.h"
#include "fastblock/monclient/messages.pb.h"
#include "fastblock/utils/err_num.h"

namespace {
static void make_sharded_ports(
    const google::protobuf::Map<google::protobuf::uint32, msg::ShardCore>& proto_shard_ports,
    std::map<uint32_t, utils::core_shard_map>& osd_sharded_ports) {
    for (auto it = proto_shard_ports.begin(); it != proto_shard_ports.end(); ++it) {
        osd_sharded_ports.emplace(
          it->first, utils::core_shard_map{it->second.port(), it->second.coreid(), it->first});
    }
}
}

namespace monitor {

/*
 * spdk callbacks
 */

int core_poller_handler(void* arg) {
    auto* mon_cli = reinterpret_cast<client*>(arg);
    if (not mon_cli->is_running()) {
        return SPDK_POLLER_IDLE;
    }

    if (mon_cli->is_terminate()) {
        return SPDK_POLLER_IDLE;
    }

    auto is_busy = mon_cli->core_poller_handler();

    return is_busy ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

int send_cluster_map_request(void* arg) {
    auto* mon_cli = reinterpret_cast<client*>(arg);
    if (not mon_cli->is_running()) {
        return SPDK_POLLER_IDLE;
    }
    if (mon_cli->is_terminate()) {
        return SPDK_POLLER_IDLE;
    }
    mon_cli->send_cluster_map_request();
    return SPDK_POLLER_BUSY;
}

void do_start(void* arg) {
    auto* mon_cli = reinterpret_cast<client*>(arg);
    mon_cli->load_pgs();
    mon_cli->handle_start();
}

void do_stop(void* arg) {
    auto* ctx = reinterpret_cast<client::stop_context*>(arg);
    ctx->this_client->handle_stop(std::unique_ptr<client::stop_context>{ctx});
}

void do_start_cluster_map_poller(void* arg) {
    SPDK_INFOLOG(mon, "Starting clustermap poller...\n");
    auto* mon_cli = reinterpret_cast<client*>(arg);
    mon_cli->handle_start_cluster_map_poller();
}

void do_emplace_request(void* arg) {
    auto ctx = reinterpret_cast<client::request_context*>(arg);
    ctx->this_client->handle_emplace_request(ctx);
}

/*
 * methods
 */

void client::start() {
    if (not _current_thread) {
        SPDK_ERRLOG("ERROR: Cant get current spdk thread\n");
        throw std::runtime_error{"cant get current spdk thread"};
    }

    ::spdk_thread_send_msg(_current_thread, do_start, this);
}

void client::handle_start() {
    SPDK_INFOLOG(mon, "Starting monitor client...\n");

    _cluster->connect();
    _core_poller.register_poller(monitor::core_poller_handler, this, 0, "mon_core");

    _is_running = true;
}

void client::stop(std::optional<std::function<void()>>&& cb) {
    auto* ctx = new stop_context{this, std::move(cb)};
    ::spdk_thread_send_msg(_current_thread, do_stop, ctx);
}

void client::handle_stop(std::unique_ptr<client::stop_context> ctx) {
    if (_is_terminate) {
        return;
    }

    SPDK_INFOLOG(mon, "Stop the monitor client\n");
    _is_terminate = true;
    _get_cluster_map_poller.unregister_poller();
    _core_poller.unregister_poller();

    try {
        if (ctx->on_stop_cb.has_value()) {
            (ctx->on_stop_cb.value())();
        }
    } catch (const std::exception& e) {
        SPDK_ERRLOG(
          "Caught exception when executing callback on stopping monitor client: %s\n",
          e.what());
    }
}

void client::start_cluster_map_poller() {
    ::spdk_thread_send_msg(_current_thread, do_start_cluster_map_poller, this);
}

void client::handle_start_cluster_map_poller() {
    _get_cluster_map_poller.register_poller(
      monitor::send_cluster_map_request, this, _poll_period_us, "cluster_map");
}

void client::emplace_create_image_request(
  const std::string pool_name, const std::string image_name,
  const int64_t size, const int64_t object_size,
  on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* create_image_req = req->mutable_create_image_request();
    create_image_req->set_poolname(std::move(pool_name));
    create_image_req->set_imagename(std::move(image_name));
    create_image_req->set_size(size);
    create_image_req->set_object_size(object_size);

    auto* ctx = new request_context{
      this, std::move(req), std::monostate{}, std::move(cb)};
    enqueue_request(ctx);
}

void client::emplace_remove_image_request(
  const std::string pool_name,
  const std::string image_name,
  on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* real_req = req->mutable_remove_image_request();
    real_req->set_poolname(std::move(pool_name));
    real_req->set_imagename(std::move(image_name));

    auto* ctx = new client::request_context{
      this, std::move(req), std::monostate{}, std::move(cb)};
    enqueue_request(ctx);
}

void client::emplace_resize_image_request(
  const std::string pool_name, const std::string image_name,
  const int64_t size, on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* real_req = req->mutable_resize_image_request();
    real_req->set_poolname(std::move(pool_name));
    real_req->set_imagename(std::move(image_name));
    real_req->set_size(size);

    auto* ctx = new client::request_context{
      this, std::move(req), std::monostate{}, std::move(cb)};
    enqueue_request(ctx);
}

void client::emplace_get_image_info_request(
  const std::string pool_name,
  const std::string image_name,
  on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* real_req = req->mutable_get_imageinfo_request();
    real_req->set_poolname(std::move(pool_name));
    real_req->set_imagename(std::move(image_name));

    auto* ctx = new client::request_context{
      this, std::move(req), std::monostate{}, std::move(cb)};
    enqueue_request(ctx);
}

void client::emplace_list_pool_request(on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    [[maybe_unused]] auto _ = req->mutable_list_pools_request();
    auto* ctx = new client::request_context{
      this, std::move(req), std::monostate{}, std::move(cb)};
    enqueue_request(ctx);
}

void client::handle_emplace_request(client::request_context* ctx) {
    _requests.push_back(std::unique_ptr<request_context>{ctx});
}

void client::send_cluster_map_request() {
    auto req = std::make_unique<msg::Request>();
    auto* cluster_req = req->mutable_get_cluster_map_request();

    auto* osd_req = cluster_req->mutable_gom_request();
    osd_req->set_currentversion(-1);
    osd_req->set_osdid(_self_osd_id);

    auto* pg_map_req = cluster_req->mutable_gpm_request();
    if (not _pg_map.pool_version.empty()) {
        for (const auto& pair : _pg_map.pool_version) {
            (*(pg_map_req->mutable_pool_versions()))[pair.first] = pair.second;
        }
    }

    _internal_requests.push_back(std::move(req));
}

bool client::core_poller_handler() {
    return handle_response() | consume_request();
}

client::response_status
client::to_response_status(const msg::CreateImageErrorCode e) noexcept {
    switch (e) {
    case msg::CreateImageErrorCode::createImageOk:
        return client::response_status::ok;
    case msg::CreateImageErrorCode::imageExists:
        return client::response_status::created_image_exists;
    case msg::CreateImageErrorCode::marshalImageContextError:
        return client::response_status::marshal_image_context_error;
    case msg::CreateImageErrorCode::putEtcdError:
        return client::response_status::server_put_ectd_error;
    case msg::CreateImageErrorCode::unknownPoolName:
        return client::response_status::unknown_pool_name;
    case msg::CreateImageErrorCode::imageNameTooLong:
        return client::response_status::image_name_too_long;
    default:
        return client::response_status::unknown_server_status;
    }
}

client::response_status
client::to_response_status(const msg::RemoveImageErrorCode e) noexcept {
    switch (e) {
    case msg::RemoveImageErrorCode::imageNotFound:
        return client::response_status::image_not_found;
    case msg::RemoveImageErrorCode::removeImageFail:
        return client::response_status::server_error;
    case msg::RemoveImageErrorCode::removeImageOk:
        return client::response_status::ok;
    default:
        return client::response_status::unknown_server_status;
    }
}

client::response_status
client::to_response_status(const msg::ResizeImageErrorCode e) noexcept {
    switch (e) {
    case msg::ResizeImageErrorCode::marshalResizeImageContextError:
        return client::response_status::marshal_image_context_error;
    case msg::ResizeImageErrorCode::putResizeImageEtcdError:
        return client::response_status::server_put_ectd_error;
    case msg::ResizeImageErrorCode::resizeImageNotFound:
        return client::response_status::image_not_found;
    case msg::ResizeImageErrorCode::resizeImageOk:
        return client::response_status::ok;
    default:
        return client::response_status::unknown_server_status;
    }
}

client::response_status
client::to_response_status(const msg::GetImageErrorCode e) noexcept {
    switch (e) {
    case msg::GetImageErrorCode::getImageNotFound:
        return client::response_status::image_not_found;
    case msg::GetImageErrorCode::getImageOk:
        return client::response_status::ok;
    default:
        return client::response_status::unknown_server_status;
    }
}

void client::create_pg(pg_map::pool_id_type pool_id, std::string& pool_name, pg_map::version_type pool_version, const msg::PGInfo &info){
    auto pit = std::make_unique<utils::pg_info_type>();
    pit->pg_id = info.pgid();
    pit->version = info.version();
    std::string osd_str;
    for (auto osd_id : info.osdid()){
        pit->osds.push_back(osd_id);
        osd_str += std::to_string(osd_id) + ",";
    }
    SPDK_DEBUGLOG(mon, "core [%u] pool: %d pg: %lu version: %ld  osd_list: %s\n",
          ::spdk_env_get_current_core(), pool_id, pit->pg_id, pit->version, osd_str.data());
    _pg_map.pool_pg_map[pool_id].emplace(pit->pg_id, std::move(pit));
    _pg_map.add_pools(pool_id, pool_name);
}

void delete_pg_done(void *arg, int perrono) {
    auto ctx = (delete_pg_context *)arg;
    auto pool_id = ctx->pool_id;
    auto pg_id = ctx->pg_id;
    auto pool_version = ctx->pool_version;
    auto client = ctx->cli;

    auto &pg_map = client->get_pg_map();
    if(perrono != 0){
        SPDK_ERRLOG("delete pg %d.%d failed: %s\n", pool_id, pg_id, spdk_strerror(perrono));
        pg_map.set_pool_update(pool_id, pg_id, pool_version, -1);
        return;
    }

    pg_map.set_pool_update(pool_id, pg_id, pool_version, 0);
    SPDK_DEBUGLOG(mon, "delete pg %d.%d success.\n", pool_id, pg_id);
    pg_map.pool_pg_map[pool_id].erase(pg_id);
    if(pg_map.pool_pg_map[pool_id].empty()){
        pg_map.delete_pool(pool_id);
        SPDK_DEBUGLOG(mon, "delete pool %d success.\n", pool_id);
    }
    delete ctx;
}

void client::remove_pg(pg_map::pool_id_type pool_id, pg_map::pg_id_type pg_id, pg_map::version_type pool_version){
    auto ctx = new delete_pg_context{.cli = this, .pool_id = pool_id, .pg_id = pg_id, .pool_version = pool_version};

    _pg_map.set_pool_update(pool_id, pg_id, pool_version, 1);
    delete_pg_done(ctx, 0);
}

void client::process_pg_map(const msg::GetPgMapResponse& pg_map_response) {

    auto& pv = pg_map_response.poolid_pgmapversion();
    auto& ec = pg_map_response.errorcode();
    auto& pgs = pg_map_response.pgs();
    auto& pool_ids = pg_map_response.pools();

    auto pool_item = _pg_map.pool_pg_map.begin();
    while(pool_item != _pg_map.pool_pg_map.end()){
        auto pool_id = pool_item->first;
        if(_pg_map.pool_is_updating(pool_id)){
            SPDK_DEBUGLOG(mon, "pool %d is updating\n", pool_id);
            pool_item++;
            continue;
        }
        auto pool_version = _pg_map.pool_version[pool_id];
        auto& pg_infos = pool_item->second;
        pool_item++;

        if(!ec.contains(pool_id)){
            SPDK_WARNLOG("pool %d is deleted\n", pool_id);
            //pool被删除
            auto pg_item = pg_infos.begin();
            while(pg_item != pg_infos.end()){
                auto pg_id = pg_item->first;
                pg_item++;
                remove_pg(pool_id, pg_id, pool_version);
            }
        }else{
            delete_pg_from_osd(pgs, pool_id, pg_infos); 
        }
    }

    if (pv.empty()) {
        SPDK_DEBUGLOG(mon, "Empty pg map from cluster map response\n");
        return;
    }
    SPDK_DEBUGLOG(mon, "ec count is %ld, pgs count is %ld\n", ec.size(), pgs.size());

    std::remove_cvref_t<decltype(ec)>::key_type pool_key{};
    for (auto& pair : ec) {
        pool_key = pair.first;
        SPDK_DEBUGLOG(mon, "pg map key is %d\n", pool_key);
        if (pair.second != msg::GetPgMapErrorCode::pgMapGetOk) {
            SPDK_ERRLOG("ERROR: get pg map(key: %d) return error: %d\n",
              pool_key, pair.second);
            continue;
        }

        if (!pgs.contains(pool_key) or !pv.contains(pool_key) or !pool_ids.contains(pool_key)) {
            SPDK_ERRLOG("pool %d has no pgmap, that is a error\n", pool_key);
            continue;
        }

        auto pools_it = pool_ids.find(pool_key);
        auto pool_name = pools_it->second;
        if(_pg_map.pool_is_updating(pool_key)){
            SPDK_DEBUGLOG(mon, "pool %d is updating\n", pool_key);
            continue;
        }

        SPDK_DEBUGLOG(mon, "going to process pg with key %d\n", pool_key);
        if (not _pg_map.pool_pg_map.contains(pool_key)) {
            auto info_it = pgs.find(pool_key);
            if (info_it == pgs.end()) {
                SPDK_DEBUGLOG(mon, "Cant find the info of pg %d\n", pool_key);
                continue;
            }

            _pg_map.pool_version.emplace(pool_key, pv.at(pool_key));
            SPDK_DEBUGLOG(mon, "pgs size is %ld\n", pgs.size());

            for (auto& info : info_it->second.pi()) {
                create_pg(pool_key, pool_name, pv.at(pool_key), info);
            }

            continue;
        }

        if(_pg_map.pool_version[pool_key] == pv.at(pool_key)){
            //检查之前添加pool时，是否有pg没有添加成功
            auto info_it = pgs.find(pool_key);
            if (info_it == pgs.end()) {
                SPDK_DEBUGLOG(mon, "Cant find the info of pg %d\n", pool_key);
                continue;
            }

            for (auto& info : info_it->second.pi()){
                if(not _pg_map.pool_pg_map[pool_key].contains(info.pgid())){
                    create_pg(pool_key, pool_name, pv.at(pool_key), info);
                }
            }
            continue;
        }

        /*
         *  下面的change_pg_membership时会检查当前osd是否是pg的leader，如果不是就相当于更新pg成员失败，如果更新成员失败，
         *   _pg_map中pg 版本和pool版本都不会更新。当pool下的所有pg都处理成功（包含创建pg和pg更新成员）后，_pg_map中pg 版本
         *   和pool版本都会更新。
         *   若更新pg成员失败，下次调用process_pg_map检查到这里时，因为_pg_map中pool版本小，还是会进行下面的处理：
         *       如果_pg_map中pg版本和monitor中pg版本相同，不需要处理。
         *       如果_pg_map中pg版本为0，出现这种情况是当前osd刚重启，需要激活pg
         *       如果monitor上pg还是PgRemapped，就需要继续调用change_pg_membership；
         *       如果monitor上pg不是PgRemapped，就需要分情况了
         *          1 当前osd不在monitor上pg的osd列表中，表示monitor的pg成员变更完成，当前osd从pg中移除，因此需要删除osd上pg。
         *          2 当前osd在monitor上pg的osd列表中，但_pg_map中pg的osd列表与monitor上pg的osd列表不同，表示monitor的pg成员
         *            变更完成，因此需要更新_pg_map中pg的osd列表为monitor上pg的osd列表
         */
        if (_pg_map.pool_version[pool_key] < pv.at(pool_key)) {
            auto info_it = pgs.find(pool_key);
            if (info_it == pgs.end()) {
                SPDK_DEBUGLOG(mon, "Cant find the info of pg %d\n", pool_key);
                continue;
            }

            auto process_pg = [this](pg_map::pool_id_type pool_id, const msg::PGInfo& info, int64_t pool_version_in_mon){
                auto pgid = info.pgid();
                auto& pit = _pg_map.pool_pg_map[pool_id][pgid];

                std::vector<int> osds_in_mon;
                for (auto osd_id : info.osdid()){
                    osds_in_mon.push_back(osd_id);
                }
                bool contain_in_monitor = in_monitor_list(info);

                //当前node不在monitor上pg的osd列表中
                if(!contain_in_monitor ){
                    remove_pg(pool_id, pgid, _pg_map.pool_version[pool_id]);
                } else if(contain_in_monitor){
                    if(osds_in_mon.size() != pit->osds.size()){
                        _pg_map.set_pool_update(pool_id, pgid, pool_version_in_mon, 1);
                    }else if(!std::is_permutation(osds_in_mon.begin(), osds_in_mon.end(), pit->osds.begin())){
                        _pg_map.set_pool_update(pool_id, pgid, pool_version_in_mon, 1);
                    }else{
                        return;
                    }
                    pit->osds = osds_in_mon;
                    pit->version = info.version();
                    _pg_map.set_pool_update(pool_id, pgid, pool_version_in_mon, 0);
                }
            };

            SPDK_INFOLOG(mon, "pool %d version: %ld pool_version: %ld\n", pool_key, pv.at(pool_key), _pg_map.pool_version[pool_key]);
            for (auto& info : info_it->second.pi()) {
                auto pgid = info.pgid();
                if(_pg_map.pool_pg_map[pool_key].contains(pgid)){
                    auto& pit = _pg_map.pool_pg_map[pool_key][pgid];
                    if(info.version() == pit->version)
                        continue;

                    check_and_active_pg(pool_key, pgid, pv.at(pool_key), info);
                    
                    if((info.state() & PgRemapped) == 0){
                        process_pg(pool_key, info, pv.at(pool_key));
                        continue;
                    }
                    
                    change_pg_membership(info, pool_key, pv.at(pool_key), pgid);
                }else{
                    create_pg(pool_key, pool_name, pv.at(pool_key), info);
                }
            }
        }
    }
}

void client::connect_osd(std::unique_ptr<response_stack> &resp_stack, 
                        utils::osd_info_t &, 
                        std::shared_ptr<msg::Response> response) {
    if (not resp_stack) {
        resp_stack = std::make_unique<response_stack>(response, 0);
    }    
}

void client::process_osd_map(std::shared_ptr<msg::Response> response) {
    auto& osd_map_response = response->get_cluster_map_response().gom_response();

    if (osd_map_response.errorcode() != msg::OsdMapErrorCode::ok) {
        SPDK_ERRLOG(
            "ERROR: get osd map return error, error code is %d\n",
            osd_map_response.errorcode());
        return;
    }

    auto monitor_osd_map_version = osd_map_response.osdmapversion();
    if (monitor_osd_map_version < _osd_map.version) {
        SPDK_ERRLOG(
            "Error: the version(%ld) of which monitor responsed is less than local ones(%ld)\n",
            monitor_osd_map_version, _osd_map.version);
        return;
    }

    if (monitor_osd_map_version == _osd_map.version) {
        SPDK_INFOLOG(mon,
          "getosdmap: osdmapversion is the same: %ld, process pg map directly\n",
          monitor_osd_map_version);
        process_pg_map(response->get_cluster_map_response().gpm_response());
        return;
    }

    SPDK_DEBUGLOG(mon, "The version responing from monitor is %ld\n", monitor_osd_map_version);
    _osd_map.version = monitor_osd_map_version;
    auto& osds = osd_map_response.osds();
    decltype(_responses)::value_type resp_stack{};
    bool should_create_connect{false};

    for (int i{0}; i < osds.size(); ++i) {
        bool is_valid_addr{true};
        for (auto it = osds[i].sharded_ports().begin(); it != osds[i].sharded_ports().end(); ++it) {
            auto port = it->second.port();
            if (not valid_osd_address(osds[i].address(), port)) {
                SPDK_NOTICELOG(
                  "osd %d address %s:%d is invalid, skip it\n",
                  osds[i].osdid(),
                  osds[i].address().c_str(),
                  port);
                is_valid_addr = false;
                break;
            }
        }
        if (not is_valid_addr) {
            SPDK_INFOLOG(
              mon,
              "skip osd %d due to invalid address, ispendingcreate: %d\n",
              osds[i].osdid(),
              osds[i].ispendingcreate());
            continue;
        }

        auto osd_it = _osd_map.data.find(osds[i].osdid());
        if (osd_it != _osd_map.data.end()) {
            should_create_connect =
              not osd_it->second->isup and
              osds[i].isup() and
              osd_it->second->node_id != _self_osd_id;

            SPDK_DEBUGLOG(
              mon,
              "osd %d found, osd isup %d,  rsp osd isup %d, should_create_connect is %d\n",
              osd_it->second->node_id, osd_it->second->isup, osds[i].isup(), should_create_connect);

            osd_it->second->node_id = osds[i].osdid();
            osd_it->second->isin = osds[i].isin();
            osd_it->second->ispendingcreate = osds[i].ispendingcreate();
            auto& sharded_ports = osds[i].sharded_ports();
            osd_it->second->sharded_ports.clear();
            make_sharded_ports(sharded_ports, osd_it->second->sharded_ports);
            osd_it->second->address = osds[i].address();
            if(osd_it->second->isup && !osds[i].isup() && osd_it->second->node_id != _self_osd_id){
                SPDK_DEBUGLOG(mon, "osd %d is down, remove connect to it\n", osds[i].osdid());

                remove_osd_connect(osds[i].osdid(),
                  [this, node_id = osds[i].osdid()](void *, int ){
                    SPDK_DEBUGLOG(mon, "remove the connect to osd %d\n",node_id);
                  }
                );
                osd_it->second->isup = false;
            }
            osd_it->second->isup = osds[i].isup();
        } else {
            should_create_connect =
              osds[i].isup() and osds[i].osdid() != _self_osd_id;

            SPDK_DEBUGLOG(mon,
              "osd %d not found, rsp osd isup %d, should_create_connect is %d\n",
              osds[i].osdid(),
              osds[i].isup(),
              should_create_connect);

            auto osd_info = std::make_unique<utils::osd_info_t>(
              osds[i].osdid(),
              osds[i].isin(),
              osds[i].isup(),
              osds[i].ispendingcreate(),
              std::map<uint32_t, utils::core_shard_map>{},
              osds[i].address());
            auto& proto_shard_ports = osds[i].sharded_ports();
            make_sharded_ports(proto_shard_ports, osd_info->sharded_ports);

            auto [it, _] = _osd_map.data.emplace(osd_info->node_id, std::move(osd_info));
            osd_it = it;
        }

        auto& osd_info = *(osd_it->second);
        if (should_create_connect) {
            connect_osd(resp_stack, osd_info, response);
        }
    }

    auto data_it = _osd_map.data.begin();
    while(data_it != _osd_map.data.end()){
        auto it = std::find_if(
            osds.begin(), osds.end(),
            [k = data_it->first] (const auto& osd) {
                return osd.osdid() == k;
            }
        );

        if (it != osds.end()) {
            data_it++;
            continue;
        }

        auto node_id = data_it->first;
        data_it++;
        SPDK_DEBUGLOG(mon,
          "The osd with id %d not found in the newer osd map\n",
          node_id);
        
        remove_osd_connect(node_id,
          [this, node_id](void *, int res){
            if(res == err::E_SUCCESS){
              _osd_map.data.erase(node_id);
              SPDK_INFOLOG(mon, "remove the connect to osd %d\n",node_id);
            }
          }        
        );
    }

    if (not resp_stack) {
        SPDK_DEBUGLOG(mon, "empty response stack struct\n");
        return;
    }

    SPDK_DEBUGLOG(
      mon, "created response_stack, un-connected size is %lu\n",
      resp_stack->un_connected_count);
    _responses.push_back(std::move(resp_stack));
}

void client::process_clustermap_response(std::shared_ptr<msg::Response> response) {
    if (not _responses.empty()) {
        auto head_it = _responses.begin();
        auto* stack_ptr = head_it->get();
        SPDK_DEBUGLOG(mon, "head response un-connected count is %ld\n",
          stack_ptr->un_connected_count);
        if (stack_ptr->un_connected_count == 0) {
            auto& pg_map_resp = stack_ptr->response->get_cluster_map_response().gpm_response();
            process_pg_map(pg_map_resp);
            _responses.erase(head_it);
            if (_cluster_map_init_cb) {
                try {
                    _cluster_map_init_cb.value()();
                } catch (...) {
                    SPDK_ERRLOG("ERRPR: invoke the callback on getting cluster map at first time\n");
                }
                _cluster_map_init_cb = std::nullopt;
            }
        }
    }

    process_osd_map(response);
}

int client::send_request(std::unique_ptr<msg::Request> req, bool send_directly) {
    return send_request(req.get(), send_directly);
}

int client::send_request(msg::Request* req, bool send_directly) {
    if(req->union_case() == msg::Request::UnionCase::kPgMemberChangeFinishRequest){
        auto &pg_req = req->pg_member_change_finish_request();
        SPDK_DEBUGLOG(mon, "in pg %lu.%lu, send PgMemberChangeFinishRequest to mon result %d, send_directly %d\n",
          pg_req.poolid(), pg_req.pgid(), pg_req.result(), send_directly);
    }

    if (not send_directly) {
        auto message_size = req->ByteSizeLong();
        auto serialized_size = message_size + _meta_length;
        if (serialized_size > _last_request_serialize_size) {
            _last_request_serialize_size = serialized_size;
            _request_serialize_buf = std::make_unique<char[]>(_last_request_serialize_size);
        }
        std::memcpy(_request_serialize_buf.get(), &message_size, _meta_length);
        req->SerializeToArray(_request_serialize_buf.get() + _meta_length, serialized_size);
        _request_iov.iov_base = _request_serialize_buf.get();
        _request_iov.iov_len = serialized_size;
        SPDK_DEBUGLOG(
          mon,
          "union_case is %d, message_size is %ld, serialized_size is %ld\n",
          req->union_case(), message_size, serialized_size);
    }

    auto rc = _cluster->writev(&_request_iov, 1);
    SPDK_DEBUGLOG(mon, "rc %ld\n", rc);
    if (rc != -1) { ++_request_counter; }
    return rc;
}

void client::process_response(std::shared_ptr<msg::Response> response) {
    auto response_case = response->union_case();
    SPDK_DEBUGLOG(mon, "Got response type %d\n", response_case);

    switch (response_case) {
    case msg::Response::UnionCase::kGetClusterMapResponse:
        _last_cluster_map_at = std::chrono::system_clock::now();
        SPDK_DEBUGLOG(mon, "received cluster map response\n");
        process_clustermap_response(response);
        break;
    case msg::Response::UnionCase::kBootResponse: {
        SPDK_DEBUGLOG(mon, "Received osd boot response\n");

        auto& req_ctx = _on_flight_requests.front();
        if (response->boot_response().result() != 0) {
            SPDK_ERRLOG("EREOR: monitor notifies boot failed\n");
        }

        auto result = response->boot_response().result();

        req_ctx->cb(response_status(result), req_ctx.get());
        if(result == 0)
            SPDK_NOTICELOG("osd %d is booted\n", _self_osd_id);
        _on_flight_requests.pop_front();
        break;
    }
    case msg::Response::UnionCase::kLeaderBeElectedResponse: {
        SPDK_DEBUGLOG(mon, "Received leader be elected response\n");

        auto& req_ctx = _on_flight_requests.front();
        response_status status;
        if(not response->leader_be_elected_response().ok())
            status = response_status::fail;
        else
            status = response_status::ok;

        req_ctx->cb(status, req_ctx.get());
        _on_flight_requests.pop_front();
        break;
    }
    case msg::Response::UnionCase::kPgMemberChangeFinishResponse: {
        SPDK_DEBUGLOG(mon, "Received pg_member_change_finish_request response\n");
        auto& req_ctx = _on_flight_requests.front();
        response_status status;

        if(not response->pg_member_change_finish_response().ok())
            status = response_status::fail;
        else
            status = response_status::ok;

        req_ctx->cb(status, req_ctx.get());
        _on_flight_requests.pop_front();
        break;
    }
    case msg::Response::UnionCase::kCreateImageResponse: {
        SPDK_DEBUGLOG(mon, "Received create image response\n");
        process_general_response(response->create_image_response());
        break;
    }
    case msg::Response::UnionCase::kRemoveImageResponse: {
        SPDK_DEBUGLOG(mon, "Received remove image response\n");
        process_general_response(response->remove_image_response());
        break;
    }
    case msg::Response::UnionCase::kResizeImageResponse: {
        SPDK_DEBUGLOG(mon, "Received resize image response\n");
        process_general_response(response->resize_image_response());
        break;
    }
    case msg::Response::UnionCase::kGetImageInfoResponse: {
        SPDK_DEBUGLOG(mon, "Received get image response\n");
        auto& resp = response->get_imageinfo_response();
        auto& img_info = resp.imageinfo();
        auto err_code = resp.errorcode();
        auto err_code_str = msg::GetImageErrorCode_Name(err_code);

        SPDK_DEBUGLOG(
          mon,
          "Received image(%s) response, error code is %s(%d), image size is %ld, object size is %ld\n",
          img_info.imagename().c_str(), err_code_str.c_str(), err_code,
          img_info.size(), img_info.object_size());

        auto& req_ctx = _on_flight_requests.front();
        req_ctx->response_data = std::make_unique<client::image_info>(
          img_info.poolname(), img_info.imagename(),
          img_info.size(), img_info.object_size());

        req_ctx->cb(to_response_status(err_code), req_ctx.get());
        _on_flight_requests.pop_front();
        break;
    }
    case msg::Response::UnionCase::kListPoolsResponse: {
        SPDK_DEBUGLOG(mon, "Received list pools response\n");
        auto& response_pools = response->list_pools_response().pi();
        auto pools_size = response_pools.size();

        SPDK_DEBUGLOG(mon, "List pool size is %d\n", pools_size);

        auto pools_info = std::make_unique<pools>(pools_size, nullptr);
        pools_info->data = std::make_unique<pools::pool[]>(pools_size);

        for (int i{0}; i < pools_size; ++i) {
            pools_info->data[i].pool_id = response_pools.at(i).poolid();
            pools_info->data[i].name = response_pools.at(i).name();
            pools_info->data[i].pg_size = response_pools.at(i).pgsize();
            pools_info->data[i].pg_count = response_pools.at(i).pgcount();
            pools_info->data[i].failure_domain = response_pools.at(i).failuredomain();
            pools_info->data[i].root = response_pools.at(i).root();
        }

        auto& req_ctx = _on_flight_requests.front();
        req_ctx->response_data = std::move(pools_info);
        req_ctx->cb(response_status::ok, req_ctx.get());
        _on_flight_requests.pop_front();
        break;
    }
    case msg::Response::UnionCase::kDataStatisticsResponse: {
        SPDK_DEBUGLOG(mon, "Received data statistics response\n");

        auto& req_ctx = _on_flight_requests.front();
        response_status status;
        if(not response->data_statistics_response().ok())
            status = response_status::fail;
        else
            status = response_status::ok;

        req_ctx->cb(status, req_ctx.get());
        _on_flight_requests.pop_front();
        break;
    }
    default:
        SPDK_NOTICELOG(
          "Skipping monitor response with response case %d\n",
          static_cast<std::underlying_type_t<msg::Response::UnionCase>>(response_case));
    }
}

bool client::handle_response() {
    if(is_terminate())
        return false;   
    
    if (_request_counter == 0) { return false; }

    if (not _cluster->is_connected()) {
        return false;
    }

    uint64_t msg_len = 0;
    ssize_t rc = 0;

    rc = _cluster->recv(_response_buffer.get() + _read_bytes, _should_read_bytes - _read_bytes);
    if (rc == 0) [[likely]] {
        return false;
    }

    if (rc < 0) [[unlikely]] {
        SPDK_ERRLOG(
          "ERROR: spdk_sock_recv() failed, errno %d: %s\n",
          errno, ::spdk_strerror(errno));

        return false;
    }

    _read_bytes += rc;
    SPDK_DEBUGLOG(mon, "_read_bytes %ld, _should_read_bytes %ld\n", _read_bytes, _should_read_bytes);
    if (_read_bytes < _should_read_bytes){
        return true;
    }

    _read_bytes = _read_bytes - _should_read_bytes;
    if(_read_bytes < 0){
        SPDK_ERRLOG("read error, _read_bytes is %ld, _should_read_bytes is %ld\n", _read_bytes, _should_read_bytes);
        return false;
    }

    if(_is_read_len) {
        _is_read_len = !_is_read_len;

        std::memcpy(&msg_len, _response_buffer.get(), _msg_len_size);
        _should_read_bytes = msg_len;
        if(msg_len > _buffer_size) {
            _response_buffer = std::make_unique<char[]>(msg_len);
        }
        return true;
    }
    _is_read_len = !_is_read_len;

    --_request_counter;
    auto response = std::make_shared<msg::Response>();
    response->ParseFromArray(_response_buffer.get(), _should_read_bytes);
    process_response(response);
    _should_read_bytes = _msg_len_size;
    _read_bytes = 0;

    return true;
}

void client::enqueue_request(client::request_context* ctx) {
    if (::spdk_env_get_current_core() == _current_core) {
        _requests.push_back(std::unique_ptr<request_context>{ctx});
        return;
    }

    ::spdk_thread_send_msg(_current_thread, monitor::do_emplace_request, ctx);
}

void client::consume_general_request(bool is_cached) {
    auto& head_req = _requests.front();
    auto rc = send_request(head_req->req.get(), is_cached);

    if (rc < 0) {
        _cached = client::cached_request_class::general;
        if (_log_time_check.check_and_update()) {
            SPDK_ERRLOG("ERROR: Send request error, return code is %d\n", rc);
        }
    } else {
        _cached = client::cached_request_class::none;
        _on_flight_requests.push_back(std::move(head_req));
        _requests.pop_front();
    }
}

void client::consume_internal_request(bool is_cached) {
    int rc{0};
    if (is_cached) {
        rc = send_request(nullptr, is_cached);
    } else {
        auto head = std::move(_internal_requests.front());
        rc = send_request(std::move(head), is_cached);
    }

    if (rc < 0) {
        _cached = client::cached_request_class::internal;
        if (_log_time_check.check_and_update()) {
            SPDK_ERRLOG("ERROR: Send request error, return code is %d\n", rc);
        }
    } else {
        SPDK_DEBUGLOG(mon, "Consumed 1 internal request\n");
        _cached = client::cached_request_class::none;
        _internal_requests.pop_front();
    }
}

bool client::consume_request() {
    if(is_terminate())
        return false;        

    bool ret{false};

    switch (_cached) {
    case client::cached_request_class::none: {
        if (not _requests.empty()) {
            consume_general_request(false);
            ret = true;
        }

        if (not _internal_requests.empty()) {
            consume_internal_request(false);
            ret = true;
        }

        break;
    }
    case client::cached_request_class::general:
        consume_general_request(true);
        ret = true;
        break;
    case client::cached_request_class::internal:
        consume_internal_request(true);
        ret = true;
        break;
    default:
        break;
    }

    return ret;
}

utils::osd_info_t* client::get_pg_first_available_osd_info(int32_t pool_id, int32_t pg_id) {
    auto it = _pg_map.pool_pg_map.find(pool_id);
    if (it == _pg_map.pool_pg_map.end()) {
        SPDK_ERRLOG("ERROR: Cant find the pg map of pool id %d\n", pool_id);
        return nullptr;
    }

    auto map_it = it->second.find(pg_id);
    if (map_it == it->second.end()) {
        SPDK_ERRLOG("ERROR: Cant find the pg map of pg id %d\n", pg_id);
        return nullptr;
    }

    decltype(_osd_map.data)::iterator osd_map_it{};
    for (auto osd_id : map_it->second->osds) {
        osd_map_it = _osd_map.data.find(osd_id);
        if (osd_map_it == _osd_map.data.end()) {
            continue;
        }

        if (osd_map_it->second->isup and osd_map_it->second->isin) {
            return osd_map_it->second.get();
        }
    }

    return nullptr;
}

int client::get_pg_num(int32_t pool_id) {
    auto it = _pg_map.pool_pg_map.find(pool_id);
    if (it == _pg_map.pool_pg_map.end()) {
        SPDK_DEBUGLOG(mon, "_pg_map.pool_pg_map.size() is %lu\n", _pg_map.pool_pg_map.size());
        SPDK_ERRLOG("ERROR: Cant find this pg map of pool: %d\n", pool_id);
        return -1;
    }

    return it->second.size();
}

utils::osd_info_t* client::get_osd_info(const int32_t node_id) {
    auto it = _osd_map.data.find(node_id);
    if (it == _osd_map.data.end()) {
        return nullptr;
    }

    return it->second.get();
}

void client::send_leader_be_elected_notify_request(
  int32_t leader_id,
  uint64_t pool_id,
  uint64_t pg_id,
  std::vector<int32_t> osd_list,
  std::vector<int32_t> new_osd_list) {

    auto req = std::make_unique<msg::Request>();
    auto* leader_req = req->mutable_leader_be_elected_request();
    leader_req->set_leaderid(leader_id);
    leader_req->set_poolid(pool_id);
    leader_req->set_pgid(pg_id);
    for(int32_t osd_id : osd_list){
        leader_req->add_osdlist(osd_id);
    }
    for(int32_t new_osd_id : new_osd_list){
        leader_req->add_newosdlist(new_osd_id);
    }

    auto leader_cb = [](const response_status status, request_context*ctx){
        SPDK_DEBUGLOG(mon, "notify monitor (the leader has been elected).\n");
        if (status != response_status::ok) {
            SPDK_ERRLOG("notify monitor (the leader has been elected) failed\n");
        }
    };

    auto* req_ctx = new client::request_context{
      this, std::move(req), std::monostate{}, std::move(leader_cb)};
    enqueue_request(req_ctx);
}

void client::send_pg_member_change_finished_notify(
  int result,
  uint64_t pool_id,
  uint64_t pg_id,
  std::vector<int32_t> osd_list) {
    auto req = std::make_unique<msg::Request>();
    auto* mem_req = req->mutable_pg_member_change_finish_request();
    mem_req->set_result(result);
    mem_req->set_poolid(pool_id);
    mem_req->set_pgid(pg_id);
    for(int32_t osd_id : osd_list){
        mem_req->add_osdlist(osd_id);
    }

    auto mem_cb = [](const response_status status, request_context*ctx){
        SPDK_DEBUGLOG(mon, "notify monitor (change member of pg has been finished).\n");
        if (status != response_status::ok) {
            SPDK_ERRLOG("notify monitor (change member of pg has been finished) failed\n");
        }
    };

    auto* req_ctx = new client::request_context{
      this, std::move(req), std::monostate{}, std::move(mem_cb)};
    enqueue_request(req_ctx);
}


}
