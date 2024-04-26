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

#include "msg/rdma/endpoint.h"
#include "msg/rdma/event_channel.h"
#include "msg/rdma/provider/mlx5dv.h"
#include "msg/rdma/provider/verbs.h"
#include "msg/rdma/pd.h"
#include "msg/rdma/provider.h"
#include "utils/fmt.h"

#include <functional>
#include <memory>
#include <optional>

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include <poll.h>

namespace msg {
namespace rdma {

class socket {

public:

    socket() = delete;

    socket(endpoint ep, protection_domain& pd, ::rdma_event_channel* channel, bool reuseaddr = true)
      : _ep{ep}
      , _pd{pd.value()}
      , _mlx5dv_is_supported{::mlx5dv_is_supported(pd.deivce())} {
        if (_mlx5dv_is_supported) {
            SPDK_NOTICELOG_EX("mlx5dv is supported\n");
            _provider = std::make_unique<mlx5dv>();
        } else {
            SPDK_NOTICELOG_EX("mlx5dv is not supported\n");
            _provider = std::make_unique<verbs>();
        }

        std::memset(&_hints, 0, sizeof(_hints));
        _hints.ai_port_space = RDMA_PS_TCP;
        _hints.ai_qp_type = ::IBV_QPT_RC;

        if (_ep.passive) {
            _hints.ai_flags = RAI_PASSIVE;
        }

        int ret = rdma_getaddrinfo(
          _ep.addr.c_str(),
          std::to_string(_ep.port).c_str(),
          &_hints, &_res);

        if (ret or !_res) [[unlikely]] {
            SPDK_ERRLOG_EX(
              "ERROR(%s): Attempting to get information of address: %s:%d\n",
              std::strerror(errno), _ep.addr.c_str(), _ep.port);
        }

        ret = ::rdma_create_id(channel, &_id, nullptr, ::RDMA_PS_TCP);
        assert(_id->channel == channel);
        if (ret) {
            SPDK_ERRLOG_EX("ERROR: Create rdma cm id failed\n");
            throw std::runtime_error{"create rdma cm id failed"};
        }
        _id->context = this;
        _id->pd = _pd;

        if (_ep.passive) {
            ret = ::rdma_bind_addr(_id, _res->ai_src_addr);
            if (ret) [[unlikely]] {
                SPDK_ERRLOG_EX(
                  "ERROR: rdma_bind_addr() failed to bind on %s, error: %s\n",
                  host(_res->ai_src_addr).c_str(), std::strerror(errno));

                throw std::runtime_error{"rdma_bind_addr() failed"};
            }

            SPDK_INFOLOG_EX(
              msg,
              "bind %s:%d to rdma_cm_id %p\n",
              _ep.addr.c_str(), _ep.port, _id);
            return;
        }

        ret = ::rdma_resolve_addr(
          _id,
          _res->ai_src_addr,
          _res->ai_dst_addr,
          _ep.resolve_timeout_us);

        if (ret) [[unlikely]] {
            SPDK_ERRLOG_EX(
              "ERROR: Resolve address(src: %s, dst: %s) failed: %s\n",
              host(_res->ai_src_addr).c_str(),
              host(_res->ai_dst_addr).c_str(),
              std::strerror(errno));

            throw std::runtime_error{"resolve address failed"};
        }

        if (reuseaddr) {
            ret = ::rdma_set_option(
              _id,
              RDMA_OPTION_ID,
              RDMA_OPTION_ID_REUSEADDR,
              const_cast<int*>(&_reuse_addr),
              sizeof(_reuse_addr));

            if (ret) {
                SPDK_ERRLOG_EX("ERROR: Set reuse address failed\n");
                throw std::runtime_error{"set reuse address failed"};
            }
        }

        _id->context = reinterpret_cast<void*>(this);
    }

    socket(rdma_cm_id* id, ibv_cq* cq, protection_domain& pd)
      : _ep{host(id), ::rdma_get_dst_port(id)}
      , _id{id}
      , _pd{_id->pd}
      , _mlx5dv_is_supported{::mlx5dv_is_supported(pd.deivce())} {
        assert(_id && "argument 'id' must not be nullptr");

        if (_mlx5dv_is_supported) {
            SPDK_NOTICELOG_EX("mlx5dv is supported\n");
            _provider = std::make_unique<mlx5dv>();
        } else {
            SPDK_NOTICELOG_EX("mlx5dv is not supported\n");
            _provider = std::make_unique<verbs>();
        }

        _id->verbs = _pd->context;
        create_qp(cq, reinterpret_cast<void*>(this));
        _id->context = reinterpret_cast<void*>(this);
        _connected = true;
    }

    ~socket() noexcept {
        SPDK_DEBUGLOG_EX(msg, "call ~socket() of rdma cm id %p\n", _id);
        close();
    }

    socket(const socket&) = delete;

    socket(socket&&) = delete;

    socket& operator=(const socket&) = delete;

    socket& operator=(socket&&) = delete;

public:

    static std::string qp_state_str(::ibv_qp_state s) {
        switch (s) {
        case ibv_qp_state::IBV_QPS_RESET:
            return "IBV_QPS_RESET";
        case ibv_qp_state::IBV_QPS_INIT:
            return "IBV_QPS_INIT";
        case ibv_qp_state::IBV_QPS_RTR:
            return "IBV_QPS_RTR";
        case ibv_qp_state::IBV_QPS_RTS:
            return "IBV_QPS_RTS";
        case ibv_qp_state::IBV_QPS_SQD:
            return "IBV_QPS_SQD";
        case ibv_qp_state::IBV_QPS_SQE:
            return "IBV_QPS_SQE";
        case ibv_qp_state::IBV_QPS_ERR:
            return "IBV_QPS_ERR";
        case ibv_qp_state::IBV_QPS_UNKNOWN:
            return "IBV_QPS_UNKNOWN";
        default:
            return FMT_1("error state(%1%)", s);
        }
    }

    static std::string wc_status_name(::ibv_wc_status s) {
        switch (s) {
        case ibv_wc_status::IBV_WC_SUCCESS:
            return "IBV_WC_SUCCESS";
        case ibv_wc_status::IBV_WC_LOC_LEN_ERR:
            return "IBV_WC_LOC_LEN_ERR";
        case ibv_wc_status::IBV_WC_LOC_QP_OP_ERR:
            return "IBV_WC_LOC_QP_OP_ERR";
        case ibv_wc_status::IBV_WC_LOC_EEC_OP_ERR:
            return "IBV_WC_LOC_EEC_OP_ERR";
        case ibv_wc_status::IBV_WC_LOC_PROT_ERR:
            return "IBV_WC_LOC_PROT_ERR";
        case ibv_wc_status::IBV_WC_WR_FLUSH_ERR:
            return "IBV_WC_WR_FLUSH_ERR";
        case ibv_wc_status::IBV_WC_MW_BIND_ERR:
            return "IBV_WC_MW_BIND_ERR";
        case ibv_wc_status::IBV_WC_BAD_RESP_ERR:
            return "IBV_WC_BAD_RESP_ERR";
        case ibv_wc_status::IBV_WC_LOC_ACCESS_ERR:
            return "IBV_WC_LOC_ACCESS_ERR";
        case ibv_wc_status::IBV_WC_REM_INV_REQ_ERR:
            return "IBV_WC_REM_INV_REQ_ERR";
        case ibv_wc_status::IBV_WC_REM_ACCESS_ERR:
            return "IBV_WC_REM_ACCESS_ERR";
        case ibv_wc_status::IBV_WC_REM_OP_ERR:
            return "IBV_WC_REM_OP_ERR";
        case ibv_wc_status::IBV_WC_RETRY_EXC_ERR:
            return "IBV_WC_RETRY_EXC_ERR";
        case ibv_wc_status::IBV_WC_RNR_RETRY_EXC_ERR:
            return "IBV_WC_RNR_RETRY_EXC_ERR";
        case ibv_wc_status::IBV_WC_LOC_RDD_VIOL_ERR:
            return "IBV_WC_LOC_RDD_VIOL_ERR";
        case ibv_wc_status::IBV_WC_REM_INV_RD_REQ_ERR:
            return "IBV_WC_REM_INV_RD_REQ_ERR";
        case ibv_wc_status::IBV_WC_REM_ABORT_ERR:
            return "IBV_WC_REM_ABORT_ERR";
        case ibv_wc_status::IBV_WC_INV_EECN_ERR:
            return "IBV_WC_INV_EECN_ERR";
        case ibv_wc_status::IBV_WC_INV_EEC_STATE_ERR:
            return "IBV_WC_INV_EEC_STATE_ERR";
        case ibv_wc_status::IBV_WC_FATAL_ERR:
            return "IBV_WC_FATAL_ERR";
        case ibv_wc_status::IBV_WC_RESP_TIMEOUT_ERR:
            return "IBV_WC_RESP_TIMEOUT_ERR";
        case ibv_wc_status::IBV_WC_GENERAL_ERR:
            return "IBV_WC_GENERAL_ERR";
        case ibv_wc_status::IBV_WC_TM_ERR:
            return "IBV_WC_TM_ERR";
        case ibv_wc_status::IBV_WC_TM_RNDV_INCOMPLETE:
            return "IBV_WC_TM_RNDV_INCOMPLETE";
        default:
            return FMT_1("error status(%1%)", s);
        }
    }

    static std::string host(::sockaddr* socket_addr) {
        if (not socket_addr) {
            return "empty host";
        }

        switch (socket_addr->sa_family) {
        case AF_INET: {
            auto addr_in = reinterpret_cast<sockaddr_in*>(socket_addr);
            char tmp[INET_ADDRSTRLEN] = {'\0'};
            inet_ntop(AF_INET, &(addr_in->sin_addr), tmp, INET_ADDRSTRLEN);

            return tmp;
        }

        case AF_INET6: {
            auto addr_in6 = reinterpret_cast<sockaddr_in6*>(socket_addr);
            char tmp[INET6_ADDRSTRLEN] = {'\0'};
            inet_ntop(AF_INET6, &(addr_in6->sin6_addr), tmp, INET6_ADDRSTRLEN);

            return tmp;
        }
        }

        return "unknown host";
    }

    static std::string host(::rdma_cm_id* id) {
        return host(::rdma_get_peer_addr(id));
    }

    static uint32_t
    cap_from_attr(ibv_device_attr* dev_attr, size_t field_off, uint32_t custom) {
        if (!dev_attr) {
            return custom;
        }

        auto begin = reinterpret_cast<uintptr_t>(dev_attr);
        auto dev_attr_v = reinterpret_cast<uint32_t*>(begin + field_off);
        return std::min(custom, *dev_attr_v);
    }

public:

    bool is_closed() noexcept {
        return _closed;
    }

    ::rdma_cm_id* id() noexcept {
        return _id;
    }

    ::ibv_pd* pd() noexcept {
        return _pd;
    }

    uint64_t guid() noexcept {
        return ::ibv_get_device_guid(_id->verbs->device);
    }

    std::string qp_state_str() {
        return qp_state_str(_id->qp->state);
    }

    [[gnu::always_inline]] auto max_slient_wr() noexcept {
        return _ep.max_send_wr;
    }

    uint16_t src_port() noexcept {
        if (!_id) {
            return 0;
        }

        return ::rdma_get_src_port(_id);
    }

    inline auto& get_endpoint() noexcept {
        return _ep;
    }

    std::string peer_address() noexcept {
        return FMT_2(
          "%1%:%2%",
          host(::rdma_get_peer_addr(_id)),
          ::rdma_get_dst_port(_id));
    }

    std::string local_address() noexcept {
        return FMT_2(
          "%1%:%2%",
          host(::rdma_get_local_addr(_id)),
          ::rdma_get_src_port(_id));
    }

    std::optional<std::error_code> listen(const int backlog) noexcept {
        if (::rdma_listen(_id, backlog)) [[unlikely]] {
            return std::error_code{errno, std::system_category()};
        }

        return std::nullopt;
    }

    void stop_listen() noexcept {
        if (_id) {
            ::rdma_destroy_id(_id);
            _id = nullptr;
        }

        _closed = true;
    }

    std::optional<std::error_code> send(::ibv_send_wr* wr) noexcept {
        ibv_send_wr *bad;
        auto rc = ::ibv_post_send(_id->qp, wr, &bad);

        if (rc) [[unlikely]] {
            errno = rc;
            return std::error_code{errno, std::system_category()};
        }
        SPDK_DEBUGLOG_EX(msg, "posted send wr with id %ld\n", wr->wr_id);
        return std::nullopt;
    }

    std::optional<std::error_code> receive(::ibv_recv_wr* wr) noexcept {
        ::ibv_recv_wr* bad_wr;
        auto ret = ::ibv_post_recv(_id->qp, wr, &bad_wr);
        if (ret) [[unlikely]] {
            errno = ret;
            return std::error_code{ret, std::system_category()};
        }

        return std::nullopt;
    }

    ::rdma_cm_id* process_event_directly(::rdma_cm_event* evt) {
        std::exception_ptr eptr;
        ::rdma_cm_id* ret{nullptr};

        switch (evt->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
        case RDMA_CM_EVENT_ROUTE_ERROR:
            // 我们使用 rdma_create_ep，不会碰上这几个 event
            break;

        case RDMA_CM_EVENT_CONNECT_REQUEST: {
            SPDK_DEBUGLOG_EX(
              msg,
              "received connect request on name %s, deceive name %s",
              evt->id->verbs->device->name,
              evt->id->verbs->device->dev_name);

            ret = evt->id;
            break;
        }

        case RDMA_CM_EVENT_CONNECT_RESPONSE:
            /*
             * active side 会生成该事件，但仅针对无 qp 关联的 rdma_cm_id，
             * 目前我们不会在 active side 用到无 qp 的 rdma_cm_id
             */
            break;

        case RDMA_CM_EVENT_CONNECT_ERROR:
            break;

        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
            // active side only
            break;

        case RDMA_CM_EVENT_ESTABLISHED:
            break;

        case RDMA_CM_EVENT_DISCONNECTED:
            SPDK_ERRLOG_EX("ERROR: Got event 'RDMA_CM_EVENT_DISCONNECTED'\n");
            eptr = std::make_exception_ptr(std::runtime_error{"got event 'RDMA_CM_EVENT_DISCONNECTED'"});
            break;

        case RDMA_CM_EVENT_DEVICE_REMOVAL:
            SPDK_ERRLOG_EX("ERROR: Got event 'RDMA_CM_EVENT_DEVICE_REMOVAL'\n");
            eptr = std::make_exception_ptr(std::runtime_error{"got event 'RDMA_CM_EVENT_DEVICE_REMOVAL'"});
            break;

        case RDMA_CM_EVENT_MULTICAST_JOIN:
        case RDMA_CM_EVENT_MULTICAST_ERROR:
            // 用不到
            break;

        case RDMA_CM_EVENT_ADDR_CHANGE:
            SPDK_ERRLOG_EX("ERROR: Got event 'RDMA_CM_EVENT_ADDR_CHANGE'\n");
            eptr = std::make_exception_ptr(std::runtime_error{"got event 'RDMA_CM_EVENT_ADDR_CHANGE'"});
            break;

        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
            // 我们不会重用 qp
            break;
        default:
            SPDK_ERRLOG_EX("ERROR: Unexpected rdma cm event: %d\n", evt->event);
            eptr = std::make_exception_ptr(std::runtime_error{"unknown event"});
            break;
        }

        if (eptr) [[unlikely]] {
            std::rethrow_exception(eptr);
        }

        return ret;
    }

    ::rdma_cm_event* poll_event() noexcept {
        if (_closed) { return nullptr; }

        int ret = ::poll(_cm_poll_fd.get(), 1, 0);

        if (ret <= 0 or !(_cm_poll_fd->revents & POLLIN)) {
            return nullptr;
        }

        ::rdma_cm_event* event;

        ret = ::rdma_get_cm_event(_id->channel, &event);
        if (ret) [[unlikely]] {
            if (errno != EAGAIN and errno != EWOULDBLOCK) {
                SPDK_ERRLOG_EX(
                  "ERROR: Poll rdma event error: %s\n",
                  std::strerror(errno));
            }

            return nullptr;
        }

        return event;
    }

    int process_active_cm_event(::rdma_cm_event_type expected_evt, ::rdma_cm_event* evt) noexcept {
        SPDK_DEBUGLOG_EX(
          msg,
          "process active side event: %s(id: %p)\n",
          ::rdma_event_str(evt->event), evt->id);

        auto is_valid = validate_cm_event(expected_evt, evt);
        if (not is_valid) {
            ::rdma_ack_cm_event(evt);
            return -1;
        }

        bool is_bad_evt{false};
        switch (evt->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_REQUEST:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_ESTABLISHED:
        case RDMA_CM_EVENT_MULTICAST_JOIN:
        case RDMA_CM_EVENT_MULTICAST_ERROR:
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
            break;
        case RDMA_CM_EVENT_CONNECT_RESPONSE:
            _provider->complete_qp_connect(_id);
            break;
        case RDMA_CM_EVENT_DISCONNECTED:
            SPDK_ERRLOG_EX("ERROR: Got event 'RDMA_CM_EVENT_DISCONNECTED'\n");
            is_bad_evt = true;
            break;
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
            SPDK_ERRLOG_EX("ERROR: Got event 'RDMA_CM_EVENT_DEVICE_REMOVAL'\n");
            is_bad_evt = true;
            break;
        case RDMA_CM_EVENT_ADDR_CHANGE:
            SPDK_ERRLOG_EX("ERROR: Got event 'RDMA_CM_EVENT_ADDR_CHANGE'\n");
            is_bad_evt = true;
            break;
        default:
            SPDK_ERRLOG_EX("ERROR: Got unknown event '%d'\n", evt->event);
            is_bad_evt = true;
            break;
        }
        ::rdma_ack_cm_event(evt);

        return is_bad_evt ? -1 : 0;
    }

    std::optional<std::error_code> accept() noexcept {
        int ret = ::rdma_accept(_id, nullptr);

        if (ret) [[unlikely]] {
            return std::make_error_code(static_cast<std::errc>(errno));
        }

        return std::nullopt;
    }

    void close() noexcept {
        if (_closed) { return; }
        _closed = true;
        disconnect();
        destroy_resource();
    }

    std::optional<std::error_code>
    start_connect(rdma_conn_param* conn_param = nullptr) noexcept {
        int ret = ::rdma_connect(_id, conn_param);

        if (ret) [[unlikely]] {
            return std::make_error_code(static_cast<std::errc>(errno));
        }

        return std::nullopt;
    }

    auto is_resolve_address_done(::rdma_cm_event* evt) {
        _id->pd = _pd;
        _id->verbs = _pd->context;
        return process_active_cm_event(::RDMA_CM_EVENT_ADDR_RESOLVED, evt);
    }

    auto is_resolve_route_done(::rdma_cm_event* evt) {
        _id->pd = _pd;
        _id->verbs = _pd->context;
        return process_active_cm_event(::RDMA_CM_EVENT_ROUTE_RESOLVED, evt);
    }

    auto is_established(::rdma_cm_event* evt) {
        return process_active_cm_event(::RDMA_CM_EVENT_ESTABLISHED, evt);
    }

    void resolve_route() {
        auto ret = ::rdma_resolve_route(_id, _ep.resolve_timeout_us);
        if (ret) [[unlikely]] {
            SPDK_ERRLOG_EX(
              "resolve route(src: %s, dst: %s) failed: %s\n",
              host(_res->ai_src_addr).c_str(),
              host(_res->ai_dst_addr).c_str(),
              std::strerror(errno));
            throw std::runtime_error{"resolve route failed"};
        }
    }

    void create_qp(protection_domain& pd, ::ibv_cq* cq) {
        SPDK_DEBUGLOG_EX(msg, "_id->pd is %p, _pd is %p, pd is %p\n", _id->pd, _pd, pd.value());
        create_qp(pd, cq, this);
    }

    int update_qp_attr() {
        if (!_id->qp) {
            return IBV_QPS_ERR + 1;
        }

        auto old_state = _qp_attr->qp_state;

        int ret = ibv_query_qp(
          _id->qp,
          _qp_attr.get(),
          _ibv_query_mask,
          _qp_init_attr.get());

        if (ret) [[unlikely]] {
            SPDK_ERRLOG_EX("ibv_query_qp() failed: %s\n", std::strerror(errno));
            return IBV_QPS_ERR + 1;
        }

        if (!known_qp_state()) {
            SPDK_ERRLOG_EX("bad qp state: %d\n", _qp_attr->qp_state);
            return IBV_QPS_ERR + 1;
        }

        if (old_state != _qp_attr->qp_state) {
            SPDK_INFOLOG_EX(
              msg,
              "qp state changed, %s -> %s\n",
              qp_state_str(old_state).c_str(),
              qp_state_str(_qp_attr->qp_state).c_str());
        }

        return _qp_attr->qp_state;
    }

    int disconnect() noexcept {
        if (not _connected) {
            SPDK_INFOLOG_EX(msg, "the connection has been disconnected.\n");
            return -1;
        }

        auto peer_addr = peer_address();
        auto local_addr = local_address();

        SPDK_NOTICELOG_EX(
          "disconnect the connection(%s => %s)\n",
          local_addr.c_str(), peer_addr.c_str());

        _connected = false;

        if (not _id) {
            SPDK_NOTICELOG_EX("rdma cm id is null when disconnect\n");
            return -1;
        }

        /*
         * 不去掉 O_NONBLOCK，rdma_disconnect 会返回 EAGAIN,
         * 这种情况下，对端不会收到 RDMA_CM_EVENT_DISCONNECTED 事件
         */
        int ret{0};

        ret = _provider->disconnect(_id);
        if (ret) {
            SPDK_ERRLOG_EX(
              "disconnect connection(%s => %s) failed\n",
              local_addr.c_str(), peer_addr.c_str());

            return -1;
        }

        return 0;
    }

    void destroy_resource() noexcept {
        auto peer_addr = peer_address();
        auto local_addr = local_address();

        SPDK_INFOLOG_EX(
          msg,
          "destroy the resource of the socket(%s => %s)\n",
          local_addr.c_str(), peer_addr.c_str());

        int ret;
        if (not _id) {
            SPDK_NOTICELOG_EX("rdma cm id is null when destroy resource\n");
        } else {
            if (_id->qp) {
                ret = ::ibv_destroy_qp(_id->qp);
                if (ret) {
                    SPDK_ERRLOG_EX(
                      "ERROR: Destroy qp(%s => %s)  failed, error: %s\n",
                      local_addr.c_str(), peer_addr.c_str(), strerror(errno));
                }
            } else {
                SPDK_NOTICELOG_EX("qp is null when destroy resource\n");
            }

            ::rdma_destroy_id(_id);
            _id = nullptr;
        }

        if (_res) {
            ::rdma_freeaddrinfo(_res);
            _res = nullptr;
        }

        _pd = nullptr;
    }

private:

    bool validate_cm_event(
      ::rdma_cm_event_type expected_event,
      ::rdma_cm_event* reaped_event) noexcept {
        switch (expected_event) {
        case RDMA_CM_EVENT_ESTABLISHED: {
            /*
             * There is an enum ib_cm_rej_reason in the kernel headers that sets 10 as
             * IB_CM_REJ_STALE_CONN. I can't find the corresponding userspace but we get
             * the same values here.
             */
            if (reaped_event->event == RDMA_CM_EVENT_REJECTED and
                reaped_event->status == 10) {
                SPDK_ERRLOG_EX("ERROR: Stale connection");
                return false;
            } else if (reaped_event->event == RDMA_CM_EVENT_CONNECT_RESPONSE) {
                /*
                 *  If we are using a qpair which is not created using rdma cm API
                 *  then we will receive RDMA_CM_EVENT_CONNECT_RESPONSE instead of
                 *  RDMA_CM_EVENT_ESTABLISHED.
                 */
                _connected = true;
            } else {
                _connected = true;
            }

            return true;
        }
        default: {
            if (expected_event == reaped_event->event) {
                return true;
            }
        }
        }

        SPDK_ERRLOG_EX(
          "ERROR: Expected %s but received %s (%d) from cm event channel (status = %d)\n",
          ::rdma_event_str(expected_event),
          ::rdma_event_str(reaped_event->event),
          reaped_event->event, reaped_event->status);

        return false;
    }


    ::ibv_cq* create_cq(::ibv_device_attr* dev_attr) {
        auto cqe = cap_from_attr(
          dev_attr,
          offsetof(ibv_device_attr, max_cqe),
          _ep.cq_num_entries * 2);
        auto* cq = ::ibv_create_cq(_id->verbs, cqe, nullptr, nullptr, 0);
        if (!cq) {
            SPDK_ERRLOG_EX("create cq failed: %s\n", std::strerror(errno));
            throw std::runtime_error{"creare cq failed"};
        }

        return cq;
    }

    void create_qp(::ibv_pd* pd, ::ibv_device_attr* dev_attr, ::ibv_cq* cq, void* ctx) {
        cq = cq ? cq : create_cq(dev_attr);
        _id->send_cq = cq;
        _id->recv_cq = cq;
        _id->send_cq_channel = cq->channel;
        _id->recv_cq_channel = cq->channel;

        std::memset(_qp_init_attr.get(), 0, sizeof(ibv_qp_init_attr));

        _qp_init_attr->send_cq = cq;
        _qp_init_attr->recv_cq = cq;

        _qp_init_attr->cap.max_send_wr = cap_from_attr(
          dev_attr,
          offsetof(::ibv_device_attr, max_qp_wr),
          _ep.max_send_wr);
        _qp_init_attr->cap.max_recv_wr = cap_from_attr(
          dev_attr,
          offsetof(::ibv_device_attr, max_qp_wr),
          _ep.max_recv_wr);
        _qp_init_attr->cap.max_send_sge = cap_from_attr(
          dev_attr,
          offsetof(::ibv_device_attr, max_sge),
          _ep.max_send_sge);
        _qp_init_attr->cap.max_recv_sge = cap_from_attr(
          dev_attr,
          offsetof(::ibv_device_attr, max_sge),
          _ep.max_recv_sge);

        _qp_init_attr->cap.max_inline_data = _ep.max_inline_data;
        _qp_init_attr->sq_sig_all = _ep.qp_sig_all;
        _qp_init_attr->qp_type = ::IBV_QPT_RC;
        _qp_init_attr->qp_context = reinterpret_cast<void*>(ctx);

        if (_mlx5dv_is_supported) {
            _id->qp = _provider->create_qp(pd->context, pd, _qp_init_attr.get());
        } else {
            _provider->create_qp(_id, _pd, _qp_init_attr.get());
        }

        if (!_id->qp) {
            SPDK_ERRLOG_EX("ERROR: Create qp failed\n");
            throw std::runtime_error{"create qp failed"};
        }

        [[maybe_unused]] auto _ = update_qp_attr();
    }

    void create_qp(protection_domain& pd, ibv_cq* cq, void* ctx) {
        auto& dev_attr = pd.device_attr();
        create_qp(pd.value(), &dev_attr, cq, ctx);
    }

    void create_qp(ibv_cq* cq, void* ctx) {
        ibv_device_attr dev_attr;
        if (::ibv_query_device(_id->verbs, &dev_attr)) {
            create_qp(_id->pd, nullptr, cq, ctx);
        } else {
            create_qp(_id->pd, &dev_attr, cq, ctx);
        }
    }

    bool known_qp_state() noexcept {
        switch (_qp_attr->qp_state) {
        case IBV_QPS_RESET:
        case IBV_QPS_INIT:
        case IBV_QPS_RTR:
        case IBV_QPS_RTS:
        case IBV_QPS_SQD:
        case IBV_QPS_SQE:
        case IBV_QPS_ERR:
        	return true;
        default:
        	return false;
        }
    }

private:

    endpoint _ep;
    ::rdma_cm_id* _id{nullptr};
    ::rdma_addrinfo* _res{nullptr};
    ::rdma_addrinfo _hints{};
    ::ibv_pd* _pd{nullptr};
    std::unique_ptr<provider> _provider{nullptr};
    std::unique_ptr<ibv_qp_init_attr> _qp_init_attr{new ibv_qp_init_attr{}};
    std::unique_ptr<ibv_qp_attr> _qp_attr{new ibv_qp_attr{}};
    std::unique_ptr<::pollfd> _cm_poll_fd{nullptr};
    bool _closed{false};
    bool _connected{false};
    /*
     * 通过 RDMA_CM_EVENT_CONNECT_REQUEST 创建的 rdma_socket 会和
     * 作为 listener 的 rdma_socket 共用一个 event channel，
     * 所以需要区分哪些 rdma_socket 需要处理 channel
     */
    bool _process_channel_when_close{false};
    bool _mlx5dv_is_supported{false};

private:

    static constexpr int _reuse_addr{1};

    static constexpr int _ibv_query_mask =
      IBV_QP_STATE |
      IBV_QP_PKEY_INDEX |
      IBV_QP_PORT |
      IBV_QP_ACCESS_FLAGS |
      IBV_QP_AV |
      IBV_QP_PATH_MTU |
      IBV_QP_DEST_QPN |
      IBV_QP_RQ_PSN |
      IBV_QP_MAX_DEST_RD_ATOMIC |
      IBV_QP_MIN_RNR_TIMER |
      IBV_QP_SQ_PSN |
      IBV_QP_TIMEOUT |
      IBV_QP_RETRY_CNT |
      IBV_QP_RNR_RETRY |
      IBV_QP_MAX_QP_RD_ATOMIC;
};

} // namespace rdma
} // namespace msg
