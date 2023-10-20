#include "mon/client.h"
#include "osd/partition_manager.h"

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
    mon_cli->handle_start();
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

    spdk_thread_send_msg(_current_thread, do_start, this);
}

void client::handle_start() {
    SPDK_INFOLOG(mon, "Starting monitor client...\n");

    _cluster->connect();
    _core_poller.poller = SPDK_POLLER_REGISTER(monitor::core_poller_handler, this, 0);

    _is_running = true;
}

void client::start_cluster_map_poller() {
    ::spdk_thread_send_msg(_current_thread, do_start_cluster_map_poller, this);
}

void client::handle_start_cluster_map_poller() {
    _get_cluster_map_poller.poller = SPDK_POLLER_REGISTER(
      monitor::send_cluster_map_request, this, _poll_period_us);
}

void client::emplace_osd_boot_request(
  const int osd_id,
  const std::string& osd_addr,
  const int osd_port,
  const std::string& osd_uuid,
  const int64_t size,
  on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* boot_req = req->mutable_boot_request();
    boot_req->set_osd_id(osd_id);
    boot_req->set_uuid(osd_uuid.c_str());
    boot_req->set_address(osd_addr.c_str());
    boot_req->set_port(osd_port);
    boot_req->set_size(size);
    char hostname[1024];
    ::gethostname(hostname, sizeof(hostname));
    boot_req->set_host(hostname);

    auto* req_ctx = new client::request_context{
      this, std::move(req), std::monostate{}, std::move(cb)};
    enqueue_request(req_ctx);
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

    if (not _pg_map.pool_version.empty()) {
        auto* pg_map_req = cluster_req->mutable_gpm_request();
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

void client::process_pg_map(const msg::GetPgMapResponse& pg_map_response) {
    auto& pv = pg_map_response.poolid_pgmapversion();

    auto& ec = pg_map_response.errorcode();
    auto& pgs = pg_map_response.pgs();

    auto pool_item = _pg_map.pool_pg_map.begin();
    while(pool_item != _pg_map.pool_pg_map.end()){
        auto pool_id = pool_item->first;
        if(!ec.contains(pool_id)){
            SPDK_INFOLOG(mon, "pool %d is deleted\n", pool_id);
            //pool被删除
            auto& pg_infos = pool_item->second;
            for(auto& pg_item : pg_infos){
                auto pg_id = pg_item.first;
                _pm.lock()->delete_partition(pool_id, pg_id);
            }
            pool_item = _pg_map.pool_pg_map.erase(pool_item);
        }else{
            pool_item++;
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

        if (!pgs.contains(pool_key) or !pv.contains(pool_key)) {
            SPDK_ERRLOG("pool %d has no pgmap, that is a error\n", pool_key);
            continue;
        }

        SPDK_DEBUGLOG(mon, "going to process pg with key %d\n", pool_key);
        if (not _pg_map.pool_pg_map.contains(pool_key)) {
            _pg_map.pool_version.emplace(pool_key, pv.at(pool_key));
            SPDK_DEBUGLOG(mon, "pgs size is %ld\n", pgs.size());

            auto info_it = pgs.find(pool_key);
            if (info_it == pgs.end()) {
                SPDK_INFOLOG(mon, "Cant find the info of pg %d\n", pool_key);
                continue;
            }

            for (auto& info : info_it->second.pi()) {
                auto pit = std::make_unique<pg_map::pg_info>();
                pit->pg_id = info.pgid();
                for (auto osd_id : info.osdid()) {
                    pit->osds.push_back(osd_id);
                }
                pit->is_update = false;
                SPDK_DEBUGLOG(mon, "core [%u] pool: %d pg: %d\n",
                  ::spdk_env_get_current_core(), pool_key, pit->pg_id);

                _pg_map.pool_pg_map[pool_key].emplace(
                  info.pgid(), std::move(pit));

                if (_new_pg_cb) {
                    _new_pg_cb.value()(info, pool_key, pv.at(pool_key), _osd_map);
                }
            }

            continue;
        }

        if (_pg_map.pool_version[pool_key] < pv.at(pool_key)) {
            _pg_map.pool_version.emplace(pool_key, pv.at(pool_key));
            auto info_it = pgs.find(pool_key);
            if (info_it == pgs.end()) {
                SPDK_INFOLOG(mon, "Cant find the info of pg %d\n", pool_key);
                continue;
            }
            for (auto& info : info_it->second.pi()) {
                auto pgid = info.pgid();
                if(_pg_map.pool_pg_map[pool_key].contains(pgid)){
                    //检查pg的osd成员是否变更
                    auto& pit = _pg_map.pool_pg_map[pool_key][pgid];
                    pit->is_update = true;
                    std::vector<int32_t> osds;
                    for (auto osd_id : info.osdid()){
                        osds.push_back(osd_id);
                    }
                    if(osds.size() != pit->osds.size()){
                        //pg的osd成员已经变更，处理成员变更   todo

                    }else{
                        if(std::is_permutation(osds.begin(), osds.end(), pit->osds.begin())){
                            //pg的osd成员已经变更，处理成员变更   todo

                        }
                    }
                }else{
                    auto pit = std::make_unique<pg_map::pg_info>();
                    pit->pg_id = pgid;
                    for (auto osd_id : info.osdid()) {
                        pit->osds.push_back(osd_id);
                    }
                    pit->is_update = true;
                    _pg_map.pool_pg_map[pool_key].emplace(pit->pg_id, std::move(pit));
                }
            }

            auto& pgm = _pg_map.pool_pg_map[pool_key];
            auto pg_item = pgm.begin();
            while(pg_item != pgm.end()){
                auto pg_id = pg_item->first;
                auto& pit = pg_item->second;
                if(!pit->is_update){
                    SPDK_INFOLOG(mon, "pg %d in pool %d is deleted\n", pg_id, pool_key);
                    //pg has been deleted
                    _pm.lock()->delete_partition(pool_key, pg_id);
                    pg_item = pgm.erase(pg_item);
                }else{
                    pit->is_update = false;
                    pg_item++;
                }
            }
        }
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
        auto osd_it = _osd_map.data.find(osds[i].osdid());
        if (osd_it != _osd_map.data.end()) {
            should_create_connect =
              not osd_it->second->isup and
              osds[i].isup() and
              osd_it->second->node_id != _self_osd_id;

            SPDK_DEBUGLOG(
              mon, "osd %d found, should_create_connect is %d\n",
              osd_it->second->node_id, should_create_connect);

            osd_it->second->node_id = osds[i].osdid();
            osd_it->second->isin = osds[i].isin();
            osd_it->second->ispendingcreate = osds[i].ispendingcreate();
            osd_it->second->port = osds[i].port();
            osd_it->second->address = osds[i].address();
        } else {
            should_create_connect =
              osds[i].isup() and osds[i].osdid() != _self_osd_id;

            SPDK_DEBUGLOG(
              mon, "osd %d not found, should_create_connect is %d\n",
              osds[i].osdid(),
              should_create_connect);

            auto osd_info = std::make_unique<::osd_info_t>(
              osds[i].osdid(),
              osds[i].isin(),
              osds[i].isup(),
              osds[i].ispendingcreate(),
              osds[i].port(),
              osds[i].address());

            auto [it, _] = _osd_map.data.emplace(osd_info->node_id, std::move(osd_info));
            osd_it = it;
        }

        auto& osd_info = *(osd_it->second);
        SPDK_DEBUGLOG(
          mon, "osd id %d, is up %d, port %d, address %s\n",
          osd_info.node_id,
          osd_info.isup,
          osd_info.port,
          osd_info.address.c_str());

        if (should_create_connect) {
            SPDK_NOTICELOG(
              "Connect to osd %d(%s:%d)\n",
              osd_info.node_id,
              osd_info.address.c_str(),
              osd_info.port);

            if (not resp_stack) {
                resp_stack = std::make_unique<response_stack>(response, 0);
            }

            _pm.lock()->get_pg_group().create_connect(
              osd_info.node_id, osd_info.address, osd_info.port,
              [raw_stack = resp_stack.get()] () {
                  raw_stack->un_connected_count--;
                  SPDK_DEBUGLOG(mon, "Connected, un-connected count is %ld\n",
                  raw_stack->un_connected_count);
              }
            );
            resp_stack->un_connected_count++;
        }
    }

    for (auto& osd_info_pair : _osd_map.data) {
        auto it = std::find_if(
            osds.begin(), osds.end(),
            [k = osd_info_pair.first] (const auto& osd) {
                return osd.osdid() == k;
            }
        );

        if (it != osds.end()) { continue; }
        SPDK_NOTICELOG(
          "The osd with id %d not found in the newer osd map\n",
          osd_info_pair.first);
        _pm.lock()->get_pg_group().remove_connect(osd_info_pair.first);
    }

    if (not resp_stack) {
        SPDK_DEBUGLOG(mon, "empty response stack struct\n");
        return;
    }

    auto factor = _pm.lock()->get_pg_group().get_raft_client_proto().connect_factor();
    resp_stack->un_connected_count *= factor;
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
    if (not send_directly) {
        auto serialized_size = req->ByteSizeLong();
        if (serialized_size > _last_request_serialize_size) {
            _last_request_serialize_size = serialized_size;
            _request_serialize_buf = std::make_unique<char[]>(_last_request_serialize_size);
        }
        req->SerializeToArray(_request_serialize_buf.get(), serialized_size);
        _request_iov.iov_base = _request_serialize_buf.get();
        _request_iov.iov_len = serialized_size;
    }

    auto rc = _cluster->writev(&_request_iov, 1);
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
        if (not response->boot_response().ok()) {
            SPDK_ERRLOG("EREOR: monitor notifies boot failed\n");
            throw std::runtime_error{"monitor notifies boot failed"};
        }

        req_ctx->cb(response_status::ok, req_ctx.get());
        SPDK_NOTICELOG("osd %d is booted\n", _self_osd_id);
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
    default:
        SPDK_NOTICELOG(
          "Skipping monitor response with response case %d\n",
          static_cast<std::underlying_type_t<msg::Response::UnionCase>>(response_case));
    }
}

bool client::handle_response() {
    if (_pm.expired()) {
        SPDK_ERRLOG("Error: referenced partition_manager has been destructed\n");
        return false;
    }

    if (_request_counter == 0) { return false; }

    if (not _cluster->is_connected()) {
        return false;
    }

    auto rc = _cluster->recv(_response_buffer.get(), _buffer_size - 1);
    if (rc == 0) [[likely]] {
        return false;
    }

    if (rc < 0) [[unlikely]] {
        SPDK_ERRLOG(
          "ERROR: spdk_sock_recv() failed, errno %d: %s\n",
          errno, ::spdk_strerror(errno));

        return false;
    }

    --_request_counter;
    auto response = std::make_shared<msg::Response>();
    response->ParseFromArray(_response_buffer.get(), rc);
    process_response(response);

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
    if (_pm.expired()) {
        SPDK_ERRLOG("Error: referenced partition_manager has been destructed\n");
        return false;
    }

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

::osd_info_t* client::get_pg_first_available_osd_info(int32_t pool_id, int32_t pg_id) {
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

::osd_info_t* client::get_osd_info(const int32_t node_id) {
    auto it = _osd_map.data.find(node_id);
    if (it == _osd_map.data.end()) {
        return nullptr;
    }

    return it->second.get();
}

}
