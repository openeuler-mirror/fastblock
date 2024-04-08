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

#include "monclient/messages.pb.h"
#include "utils/utils.h"
#include "utils/simple_poller.h"
#include "utils/time_check.h"

#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/sock.h>
#include <spdk/string.h>

#include <chrono>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <list>

class partition_manager;

namespace monitor {

class client {

public:

    enum response_status {
        ok,
        created_image_exists,
        marshal_image_context_error,
        server_put_ectd_error,
        unknown_pool_name,
        unknown_server_status,
        image_not_found,
        server_error,
        fail
    };

    struct image_info {
        std::string pool_name{};
        std::string image_name{};
        size_t size{};
        size_t object_size{};
    };

    struct pools {
        struct pool {
            int32_t pool_id;
            std::string name;
            int32_t pg_size;
            int32_t pg_count;
            std::string failure_domain;
            std::string root;
        };

        size_t num_pool{0};
        std::unique_ptr<pool[]> data{nullptr};
    };

    using response_type = std::variant<
      std::monostate,
      std::unique_ptr<image_info>,
      std::unique_ptr<pools>>;

    struct request_context;
    using on_response_callback_type = std::function<void(const response_status, request_context*)>;

    struct request_context {
        client* this_client{nullptr};
        std::unique_ptr<msg::Request> req{};
        response_type response_data{};
        on_response_callback_type cb{};
    };

    struct endpoint {
        std::string host{};
        uint16_t port{};
    };

    enum cached_request_class {
        general = 1,
        internal,
        none
    };

    struct osd_map {
        using osd_id_type = int32_t;
        using version_type = int64_t;

        std::unordered_map<osd_id_type, std::unique_ptr<utils::osd_info_t>> data{};
        version_type version{-1};
    };

    struct pg_map {
        using pool_id_type = int32_t;
        using pg_id_type = int32_t;
        using version_type = int64_t;

        // struct pg_info {
            // pg_id_type pg_id{0};
            // int64_t   version{0};
            // std::vector<osd_map::osd_id_type> osds{};
        // };

        struct pool_update_info{
            version_type pool_version{0};
            /* key为pg_id
             * value的值表示状态
             *  -1  表示更新失败
             *  0   表示更新完成
             *  1   表示更新中
             */
            std::unordered_map<pg_id_type, int> pgs{};
        };

        bool pool_is_updating(pool_id_type pool_id){
            if(pool_update.find(pool_id) == pool_update.end())
                return false;
            bool ret = false;
            int err_count = 0;

            for(auto [_, state] : pool_update[pool_id].pgs){
                if(state == 1){
                    ret = true;
                    break;
                }else if(state == -1){
                    err_count++;
                }
            }
            if(!ret){
                if(err_count == 0){
                    pool_version[pool_id] = pool_update[pool_id].pool_version;
                }
                pool_update.erase(pool_id);
            }
            return ret;
        }

        void set_pool_update(pool_id_type pool_id, pg_id_type pg_id, version_type pool_version, int state){
            if(pool_update.find(pool_id) == pool_update.end()){
                if(state == 1){
                    pool_update_info update_info;
                    update_info.pool_version = pool_version;
                    update_info.pgs[pg_id] = state;
                    pool_update[pool_id] = std::move(update_info);
                }
            }else{
                pool_update[pool_id].pgs[pg_id] = state;
            }
        }

        void delete_pool_update(pool_id_type pool_id){
            pool_update.erase(pool_id);
        }

        std::unordered_map<pool_id_type, std::unordered_map<pg_id_type, std::unique_ptr<utils::pg_info_type>>> pool_pg_map{};
        std::unordered_map<pool_id_type, version_type> pool_version{};
        std::unordered_map<pool_id_type, pool_update_info> pool_update{};
    };

    using pg_op_complete = std::function<void (void *, int)>;
    using on_new_pg_callback_type = std::function<void(const msg::PGInfo&, const pg_map::pool_id_type, const osd_map&, pg_op_complete&&, void *)>;

    using on_cluster_map_initialized_type = std::function<void()>;

    struct response_stack {
        std::shared_ptr<msg::Response> response{nullptr};
        size_t un_connected_count{0};
    };

    class cluster {
    private:
        struct count_endpoint {
            endpoint ep{};
            size_t fail_count{0};
        };

    public:

        cluster(
          const std::vector<endpoint>& eps,
          const size_t max_fail,
          const std::chrono::system_clock::duration dur,
          const bool auto_reconnect = true)
          : _auto_reconnect{auto_reconnect}, _max_fail{max_fail}, _log_time_check{dur} {
            for (auto& ep : eps) {
                _eps.emplace_back(ep, 0);
            }
            _eps.shrink_to_fit();

            _opts.opts_size = sizeof(_opts);
            ::spdk_sock_get_default_opts(&_opts);
            _opts.zcopy = true;
        }

        ~cluster() noexcept {
            if (_sock) {
                auto rc = ::spdk_sock_close(&_sock);
                if (rc == 0) {
                    SPDK_INFOLOG(mon, "Closed the connection to the monitor server\n");
                } else {
                    SPDK_ERRLOG("ERROR: Close the connection to the monitor server error\n");
                }
            }
        }

    public:

        bool is_connected() noexcept { return _is_connected; }

        void connect() {
            if (_sock or (_current_ep and _current_ep->fail_count < _max_fail)) {
                SPDK_NOTICELOG("Connecting to monitor while old one is still keep connected\n");
                ::spdk_sock_close(&_sock);
            }

            for (auto& ep : _eps) {
                _sock = ::spdk_sock_connect_ext(
                  ep.ep.host.c_str(),
                  ep.ep.port,
                  _impl_name, &_opts);

                if (_sock) {
                    ep.fail_count = 0;
                    _current_ep = &ep;
                    _is_connected = true;
                    SPDK_INFOLOG(mon, "Connected to %s:%u\n", ep.ep.host.c_str(), ep.ep.port);
                    break;
                }
            }

            if (not _sock) {
                if (_log_time_check.check_and_update()) {
                    SPDK_ERRLOG("ERROR: Connect to monitor cluster fail\n");
                }
                _is_connected = false;
            }
        }

        void inc_fail() noexcept {
            if (not _current_ep and _auto_reconnect) {
                connect();
                return;
            }
            ++_current_ep->fail_count;

            if (_current_ep->fail_count >= _max_fail) {
                if (_log_time_check.check_and_update()) {
                    SPDK_NOTICELOG(
                      "Current connection to monitor(%s:%u) has exceed max fail count(%lu)\n",
                      _current_ep->ep.host.c_str(), _current_ep->ep.port, _max_fail);
                }
                _is_connected = false;

                if (_auto_reconnect) {
                    connect();
                }
            }
        }

        uint16_t current_node_port() noexcept {
            if (not _current_ep) {
                return 0;
            }

            return _current_ep->ep.port;
        }

        auto writev(::iovec* iov, int iov_cnt) noexcept {
            if (not _sock) { inc_fail(); }

            auto rc = ::spdk_sock_writev(_sock, iov, iov_cnt);
            if (rc == -1) {
                inc_fail();
            }

            return rc;
        }

        auto recv(void* buf, size_t len) noexcept {
            if (not _sock) { inc_fail(); }

            auto rc = ::spdk_sock_recv(_sock, buf, len);
            if (rc == -1) {
                if (errno == EAGAIN or errno == EWOULDBLOCK) {
                    return decltype(rc){0};
                }

                inc_fail();
            }

            return rc;
        }

    private:

        bool _auto_reconnect{true};
        bool _is_connected{false};
        std::vector<count_endpoint> _eps{};
        const size_t _max_fail{};
        count_endpoint* _current_ep{nullptr};
        ::spdk_sock_opts _opts{};
        ::spdk_sock* _sock{nullptr};
        utils::time_check _log_time_check;

        static constexpr char* _impl_name = (char*)"posix";
    };

public:

    client() = delete;

    client(
      const std::vector<endpoint>& endpoints,
      std::weak_ptr<::partition_manager> pm,
      std::optional<on_new_pg_callback_type>&& new_pg_cb = std::nullopt,
      std::optional<on_cluster_map_initialized_type>&& cluster_map_init_cb = std::nullopt,
      int osd_id = -1,
      const size_t max_fail = 5,
      const bool auto_reconnect = true,
      const std::chrono::system_clock::duration dur = std::chrono::seconds{10})
      : _cluster{std::make_unique<cluster>(endpoints, max_fail, dur, auto_reconnect)}
      , _self_osd_id{osd_id}
      , _pm{std::move(pm)}
      , _current_thread{::spdk_get_thread()}
      , _current_core{::spdk_env_get_current_core()}
      , _log_time_check{dur}
      , _new_pg_cb{std::move(new_pg_cb)}
      , _cluster_map_init_cb{std::move(cluster_map_init_cb)} {}

    client(const client&) = delete;

    client(client&&) = default;

    client& operator=(const client&) = delete;

    client& operator=(client&&) = delete;

    ~client() noexcept = default;

public:

    bool is_running() noexcept { return _is_running; }

    auto is_pg_map_empty() noexcept { return _pg_map.pool_pg_map.empty(); }

    std::pair<std::string, int> get_osd_addr(int osd_id) {
        auto it = _osd_map.data.find(osd_id);
        if (it == _osd_map.data.end()) {
            return std::make_pair<std::string, int>("", 0);
        }

        return std::pair<std::string, int>{it->second->address, it->second->port};
    }

    auto last_cluster_map_at() noexcept {
        return _last_cluster_map_at;
    }

    auto is_terminate() noexcept {
        return _is_terminate;
    }

    void stop() noexcept {
        if (_is_terminate) {
            return;
        }

        SPDK_NOTICELOG("Stop the monitor client\n");
        _is_terminate = true;
        _get_cluster_map_poller.unregister_poller();
        _core_poller.unregister_poller();
    }

public:

    void start();
    void load_pgs();
    void handle_start();

    void start_cluster_map_poller();
    void handle_start_cluster_map_poller();

    void emplace_osd_boot_request(
      const int,
      const std::string&,
      const int,
      const std::string&,
      const int64_t,
      on_response_callback_type&&);

    void emplace_create_image_request(
      const std::string,
      const std::string,
      const int64_t,
      const int64_t,
      on_response_callback_type&& cb);

    void emplace_remove_image_request(
      const std::string pool_name, const std::string image_name,
      on_response_callback_type&& cb);

    void emplace_resize_image_request(
      const std::string pool_name, const std::string image_name,
      const int64_t size, on_response_callback_type&& cb);

    void emplace_get_image_info_request(
      const std::string pool_name, const std::string image_name,
      on_response_callback_type&& cb);

    void emplace_list_pool_request(on_response_callback_type&& cb);

    void handle_emplace_request(request_context*);
    void send_cluster_map_request();
    bool core_poller_handler();
    utils::osd_info_t* get_pg_first_available_osd_info(int32_t pool_id, int32_t pg_id);
    int get_pg_num(int32_t pool_id);
    utils::osd_info_t* get_osd_info(const int32_t node_id);
    pg_map& get_pg_map(){
        return _pg_map;
    }
private:

    template<typename ResponseType>
    void process_general_response(const ResponseType& resp) {
        auto& img_info = resp.imageinfo();
        auto err_code = resp.errorcode();

        SPDK_DEBUGLOG(
          mon,
          "Received image(%s) response, error code is %d\n",
          img_info.imagename().c_str(), err_code);

        auto& req_ctx = _on_flight_requests.front();
        req_ctx->cb(to_response_status(err_code), req_ctx.get());
        _on_flight_requests.pop_front();
    }

private:

    inline response_status to_response_status(const msg::CreateImageErrorCode) noexcept;
    inline response_status to_response_status(const msg::RemoveImageErrorCode) noexcept;
    inline response_status to_response_status(const msg::ResizeImageErrorCode) noexcept;
    inline response_status to_response_status(const msg::GetImageErrorCode) noexcept;
    void process_pg_map(const msg::GetPgMapResponse& pg_map_response);
    void process_osd_map(std::shared_ptr<msg::Response> response);
    void process_clustermap_response(std::shared_ptr<msg::Response> response);
    int send_request(msg::Request*, bool);
    int send_request(std::unique_ptr<msg::Request>, bool);
    void process_response(std::shared_ptr<msg::Response> response);
    bool handle_response();
    void enqueue_request(request_context*);
    void consume_general_request(bool);
    void consume_internal_request(bool);
    bool consume_request();

    void _create_pg(pg_map::pool_id_type pool_id, pg_map::version_type pool_version, const msg::PGInfo &info);
private:

    bool _is_terminate{false};
    std::unique_ptr<cluster> _cluster{nullptr};

    int _self_osd_id{-1};

    bool _is_running{false};

    std::weak_ptr<::partition_manager> _pm{};

    utils::simple_poller _get_cluster_map_poller{};
    utils::simple_poller _core_poller{};
    ::spdk_thread* _current_thread{nullptr};
    uint32_t _current_core{0};

    osd_map _osd_map{};
    pg_map _pg_map{};

    std::unique_ptr<char[]> _response_buffer{
      std::make_unique<char[]>(_buffer_size)};

    ::iovec _request_iov{};
    size_t _last_request_serialize_size{0};
    std::unique_ptr<char[]> _request_serialize_buf{nullptr};

    cached_request_class _cached{cached_request_class::none};
    std::list<std::unique_ptr<request_context>> _requests{};
    std::list<std::unique_ptr<request_context>> _on_flight_requests{};
    std::list<std::unique_ptr<response_stack>> _responses{};

    std::list<std::unique_ptr<msg::Request>> _internal_requests{};

    size_t _request_counter{0};

    utils::time_check _log_time_check;

    std::optional<on_new_pg_callback_type> _new_pg_cb{std::nullopt};
    std::chrono::system_clock::time_point _last_cluster_map_at{};
    std::optional<on_cluster_map_initialized_type> _cluster_map_init_cb{std::nullopt};

private:

    static constexpr size_t _buffer_size{65535};
    static constexpr uint64_t _poll_period_us{3000000};
};

}
