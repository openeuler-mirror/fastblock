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

    auto is_busy = mon_cli->core_poller_handler();

    return is_busy ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

int send_cluster_map_request(void* arg) {
    auto* mon_cli = reinterpret_cast<client*>(arg);
    if (not mon_cli->is_running()) {
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

[[nodiscard]] std::unique_ptr<client::request_context>
client::emplace_osd_boot_request(
  const int osd_id,
  const std::string& osd_addr,
  const std::string& osd_uuid,
  const int64_t size,
  on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* boot_req = req->mutable_boot_request();
    boot_req->set_osd_id(osd_id);
    boot_req->set_uuid(osd_uuid.c_str());
    boot_req->set_address(osd_addr.c_str());
    boot_req->set_port(_cluster->current_node_port());
    boot_req->set_size(size);
    char hostname[1024];
    ::gethostname(hostname, sizeof(hostname));
    boot_req->set_host(hostname);

    auto ret = std::make_unique<client::request_context>(
      this, std::move(req), nullptr, std::move(cb));
    enqueue_request(ret.get());

    return ret;
}

[[nodiscard]] std::unique_ptr<client::request_context>
client::emplace_create_image_request(
  const std::string pool_name, const std::string image_name,
  const int64_t size, const int64_t object_size,
  on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* create_image_req = req->mutable_create_image_request();
    create_image_req->set_poolname(std::move(pool_name));
    create_image_req->set_imagename(std::move(image_name));
    create_image_req->set_size(size);
    create_image_req->set_object_size(object_size);

    auto ret = std::make_unique<client::request_context>(
      this, std::move(req), nullptr, std::move(cb));
    enqueue_request(ret.get());

    return ret;
}

[[nodiscard]] std::unique_ptr<client::request_context>
client::emplace_remove_image_request(
  const std::string pool_name,
  const std::string image_name,
  on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* real_req = req->mutable_remove_image_request();
    real_req->set_poolname(std::move(pool_name));
    real_req->set_imagename(std::move(image_name));

    auto ret = std::make_unique<client::request_context>(
      this, std::move(req), nullptr, std::move(cb));
    enqueue_request(ret.get());

    return ret;
}

[[nodiscard]] std::unique_ptr<client::request_context>
client::emplace_resize_image_request(
  const std::string pool_name, const std::string image_name,
  const int64_t size, on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* real_req = req->mutable_resize_image_request();
    real_req->set_poolname(std::move(pool_name));
    real_req->set_imagename(std::move(image_name));
    real_req->set_size(size);

    auto ret = std::make_unique<client::request_context>(
      this, std::move(req), nullptr, std::move(cb));
    enqueue_request(ret.get());

    return ret;
}

[[nodiscard]] std::unique_ptr<client::request_context>
client::emplace_get_image_info_request(
  const std::string pool_name,
  const std::string image_name,
  on_response_callback_type&& cb) {
    auto req = std::make_unique<msg::Request>();
    auto* real_req = req->mutable_get_imageinfo_request();
    real_req->set_poolname(std::move(pool_name));
    real_req->set_imagename(std::move(image_name));

    auto ret = std::make_unique<client::request_context>(
      this, std::move(req), nullptr, std::move(cb));
    enqueue_request(ret.get());

    return ret;
}

void client::handle_emplace_request(client::request_context* ctx) {
    _requests.push_back(ctx);
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
    if (pv.empty()) {
        SPDK_NOTICELOG("Empty pg map from cluster map response\n");
        return;
    }

    auto& ec = pg_map_response.errorcode();
    auto& pgs = pg_map_response.pgs();

    SPDK_DEBUGLOG(mon, "ec count is %ld, pgs count is %ld\n", ec.size(), pgs.size());

    std::remove_cvref_t<decltype(ec)>::key_type pg_key{};
    for (auto& pair : ec) {
        pg_key = pair.first;
        SPDK_DEBUGLOG(mon, "pg map key is %d\n", pg_key);
        if (pair.second != msg::GetPgMapErrorCode::pgMapGetOk) {
            SPDK_ERRLOG("ERROR: get pg map(key: %d) return error: %d\n",
              pg_key, pair.second);
            continue;
        }

        if (!pgs.contains(pg_key) or !pv.contains(pg_key)) {
            SPDK_ERRLOG("pool %d has no pgmap, that is a error\n", pg_key);
            continue;
        }

        SPDK_DEBUGLOG(mon, "going to process pg with key %d\n", pg_key);
        if (not _pg_map.pool_pg_map.contains(pg_key)) {
            _pg_map.pool_version.emplace(pg_key, pv.at(pg_key));
            SPDK_DEBUGLOG(mon, "pgs size is %ld\n", pgs.size());

            auto info_it = pgs.find(pg_key);
            if (info_it == pgs.end()) {
                SPDK_INFOLOG(mon, "Cant find the info of pg %d\n", pg_key);
                continue;
            }

            for (auto& info : info_it->second.pi()) {
                auto pit = std::make_unique<pg_map::pg_info>();
                pit->pg_id = info.pgid();
                std::vector<::osd_info_t> osds{};
                for (auto osd_id : info.osdid()) {
                    pit->osds.push_back(osd_id);
                    osds.push_back(*(_osd_map.data.at(osd_id)));
                }
                SPDK_DEBUGLOG(mon, "core [%u] pool: %d pg: %d\n",
                  ::spdk_env_get_current_core(), pg_key, pit->pg_id);

                _pg_map.pool_pg_map[pg_key].emplace(
                  info.pgid(), std::move(pit));
                _pm.lock()->create_partition(
                  pg_key, info.pgid(), std::move(osds), pv.at(pg_key));
            }

            continue;
        }

        if (_pg_map.pool_version[pg_key] < pv.at(pg_key)) {
            _pg_map.pool_version.emplace(pg_key, pv.at(pg_key));
            _pg_map.pool_pg_map.clear();

            auto info_it = pgs.find(pg_key);
            if (info_it == pgs.end()) {
                SPDK_INFOLOG(mon, "Cant find the info of pg %d\n", pg_key);
                continue;
            }

            for (auto& info : info_it->second.pi()) {
                auto pit = std::make_unique<pg_map::pg_info>();
                pit->pg_id = info.pgid();
                for (auto osd_id : info.osdid()) {
                    pit->osds.push_back(osd_id);
                }
                _pg_map.pool_pg_map[pg_key].emplace(pit->pg_id, std::move(pit));
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
        SPDK_NOTICELOG(
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
                  SPDK_DEBUGLOG(
                  mon, "Connected, un-connected count is %ld\n",
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
        }
    }

    process_osd_map(response);
}

int client::send_request(std::unique_ptr<msg::Request> req, bool send_directly) {
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
        SPDK_DEBUGLOG(mon, "received cluster map response\n");
        process_clustermap_response(response);
        break;
    case msg::Response::UnionCase::kBootResponse: {
        SPDK_DEBUGLOG(mon, "Received osd boot response\n");

        auto* req_ctx = _on_flight_requests.front();
        _on_flight_requests.pop_front();

        if (not response->boot_response().ok()) {
            SPDK_ERRLOG("EREOR: monitor notifies boot failed\n");
            throw std::runtime_error{"monitor notifies boot failed"};
        }

        req_ctx->cb(response_status::ok, req_ctx);
        SPDK_NOTICELOG("osd %d is booted\n", _self_osd_id);
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

        auto* req_ctx = _on_flight_requests.front();
        _on_flight_requests.pop_front();
        req_ctx->img_info = std::make_unique<client::image_info>(
          img_info.poolname(), img_info.imagename(),
          img_info.size(), img_info.object_size());

        req_ctx->cb(to_response_status(err_code), req_ctx);

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
        _requests.push_back(ctx);
        return;
    }

    ::spdk_thread_send_msg(_current_thread, monitor::do_emplace_request, ctx);
}

void client::consume_general_request(bool is_cached) {
    auto* head_req = _requests.front();
    auto rc = send_request(std::move(head_req->req), is_cached);

    if (rc < 0) {
        _cached = client::cached_request_class::general;
        if (_log_time_check.check_and_update()) {
            SPDK_ERRLOG("ERROR: Send request error, return code is %d\n", rc);
        }
    } else {
        _cached = client::cached_request_class::none;
        _requests.pop_front();
        _on_flight_requests.push_back(head_req);
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

}
