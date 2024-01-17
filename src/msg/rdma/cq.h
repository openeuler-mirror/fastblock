/* Copyright (c) 2023 ChinaUnicom
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

#include "msg/rdma/pd.h"
#include "msg/rdma/work_request_id.h"
#include "utils/fmt.h"
#include "utils/simple_poller.h"

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/thread.h>

#include <infiniband/verbs.h>

#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>

namespace msg {
namespace rdma {

class completion_queue {

public:

    struct wc_category : std::error_category {
        const char* name() const noexcept override {
            return "work complete";
        }

        std::string message(int ev) const override {
            switch (static_cast<::ibv_wc_status>(ev)) {
            case ::IBV_WC_LOC_LEN_ERR: return "IBV_WC_LOC_LEN_ERR";
            case ::IBV_WC_LOC_QP_OP_ERR: return "IBV_WC_LOC_QP_OP_ERR";
            case ::IBV_WC_LOC_EEC_OP_ERR: return "IBV_WC_LOC_EEC_OP_ERR";
            case ::IBV_WC_LOC_PROT_ERR: return "IBV_WC_LOC_PROT_ERR";
            case ::IBV_WC_WR_FLUSH_ERR: return "IBV_WC_WR_FLUSH_ERR";
            case ::IBV_WC_MW_BIND_ERR: return "IBV_WC_MW_BIND_ERR";
            case ::IBV_WC_BAD_RESP_ERR: return "IBV_WC_BAD_RESP_ERR";
            case ::IBV_WC_LOC_ACCESS_ERR: return "IBV_WC_LOC_ACCESS_ERR";
            case ::IBV_WC_REM_INV_REQ_ERR: return "IBV_WC_REM_INV_REQ_ERR";
            case ::IBV_WC_REM_ACCESS_ERR: return "IBV_WC_REM_ACCESS_ERR";
            case ::IBV_WC_REM_OP_ERR: return "IBV_WC_REM_OP_ERR";
            case ::IBV_WC_RETRY_EXC_ERR: return "IBV_WC_RETRY_EXC_ERR";
            case ::IBV_WC_RNR_RETRY_EXC_ERR: return "IBV_WC_RNR_RETRY_EXC_ERR";
            case ::IBV_WC_LOC_RDD_VIOL_ERR: return "IBV_WC_LOC_RDD_VIOL_ERR";
            case ::IBV_WC_REM_INV_RD_REQ_ERR: return "IBV_WC_REM_INV_RD_REQ_ERR";
            case ::IBV_WC_REM_ABORT_ERR: return "IBV_WC_REM_ABORT_ERR";
            case ::IBV_WC_INV_EECN_ERR: return "IBV_WC_INV_EECN_ERR";
            case ::IBV_WC_INV_EEC_STATE_ERR: return "IBV_WC_INV_EEC_STATE_ERR";
            case ::IBV_WC_FATAL_ERR: return "IBV_WC_FATAL_ERR";
            case ::IBV_WC_RESP_TIMEOUT_ERR: return "IBV_WC_RESP_TIMEOUT_ERR";
            case ::IBV_WC_GENERAL_ERR: return "IBV_WC_GENERAL_ERR";
            case ::IBV_WC_TM_ERR: return "IBV_WC_TM_ERR";
            case ::IBV_WC_TM_RNDV_INCOMPLETE: return "IBV_WC_TM_RNDV_INCOMPLETE";
            default: return "UNKNOWN_WC_STATUS";
            }
        }
    };

public:

    static std::error_code make_error_code(::ibv_wc_status s) {
        return {static_cast<int>(s), wc_category{}};
    }

    static std::string op_name(::ibv_wc_opcode v) noexcept {
    switch (v) {
    case IBV_WC_SEND:
        return "IBV_WC_SEND";
	case IBV_WC_RDMA_WRITE:
        return "IBV_WC_RDMA_WRITE";
	case IBV_WC_RDMA_READ:
        return "IBV_WC_RDMA_READ";
	case IBV_WC_COMP_SWAP:
        return "IBV_WC_COMP_SWAP";
	case IBV_WC_FETCH_ADD:
        return "IBV_WC_FETCH_ADD";
	case IBV_WC_BIND_MW:
        return "IBV_WC_BIND_MW";
	case IBV_WC_LOCAL_INV:
        return "IBV_WC_LOCAL_INV";
	case IBV_WC_TSO:
        return "IBV_WC_TSO";
	case IBV_WC_RECV:
        return "IBV_WC_RECV";
	case IBV_WC_RECV_RDMA_WITH_IMM:
        return "IBV_WC_RECV_RDMA_WITH_IMM";
	case IBV_WC_TM_ADD:
        return "IBV_WC_TM_ADD";
	case IBV_WC_TM_DEL:
        return "IBV_WC_TM_DEL";
	case IBV_WC_TM_SYNC:
        return "IBV_WC_TM_SYNC";
	case IBV_WC_TM_RECV:
        return "IBV_WC_TM_RECV";
	case IBV_WC_TM_NO_TAG:
        return "IBV_WC_TM_NO_TAG";
	case IBV_WC_DRIVER1:
        return "IBV_WC_DRIVER1";
    default:
        return "unknown";
    }
    }

    static std::string wc_status_name(::ibv_wc_status s) noexcept {
        switch (s) {
        case ::ibv_wc_status::IBV_WC_SUCCESS:
            return "IBV_WC_SUCCESS";
	    case ::ibv_wc_status::IBV_WC_LOC_LEN_ERR:
            return "IBV_WC_LOC_LEN_ERR";
	    case ::ibv_wc_status::IBV_WC_LOC_QP_OP_ERR:
            return "IBV_WC_LOC_QP_OP_ERR";
	    case ::ibv_wc_status::IBV_WC_LOC_EEC_OP_ERR:
            return "IBV_WC_LOC_EEC_OP_ERR";
	    case ::ibv_wc_status::IBV_WC_LOC_PROT_ERR:
            return "IBV_WC_LOC_PROT_ERR";
	    case ::ibv_wc_status::IBV_WC_WR_FLUSH_ERR:
            return "IBV_WC_WR_FLUSH_ERR";
	    case ::ibv_wc_status::IBV_WC_MW_BIND_ERR:
            return "IBV_WC_MW_BIND_ERR";
	    case ::ibv_wc_status::IBV_WC_BAD_RESP_ERR:
            return "IBV_WC_BAD_RESP_ERR";
	    case ::ibv_wc_status::IBV_WC_LOC_ACCESS_ERR:
            return "IBV_WC_LOC_ACCESS_ERR";
	    case ::ibv_wc_status::IBV_WC_REM_INV_REQ_ERR:
            return "IBV_WC_REM_INV_REQ_ERR";
	    case ::ibv_wc_status::IBV_WC_REM_ACCESS_ERR:
            return "IBV_WC_REM_ACCESS_ERR";
	    case ::ibv_wc_status::IBV_WC_REM_OP_ERR:
            return "IBV_WC_REM_OP_ERR";
	    case ::ibv_wc_status::IBV_WC_RETRY_EXC_ERR:
            return "IBV_WC_RETRY_EXC_ERR";
	    case ::ibv_wc_status::IBV_WC_RNR_RETRY_EXC_ERR:
            return "IBV_WC_RNR_RETRY_EXC_ERR";
	    case ::ibv_wc_status::IBV_WC_LOC_RDD_VIOL_ERR:
            return "IBV_WC_LOC_RDD_VIOL_ERR";
	    case ::ibv_wc_status::IBV_WC_REM_INV_RD_REQ_ERR:
            return "IBV_WC_REM_INV_RD_REQ_ERR";
	    case ::ibv_wc_status::IBV_WC_REM_ABORT_ERR:
            return "IBV_WC_REM_ABORT_ERR";
	    case ::ibv_wc_status::IBV_WC_INV_EECN_ERR:
            return "IBV_WC_INV_EECN_ERR";
	    case ::ibv_wc_status::IBV_WC_INV_EEC_STATE_ERR:
            return "IBV_WC_INV_EEC_STATE_ERR";
	    case ::ibv_wc_status::IBV_WC_FATAL_ERR:
            return "IBV_WC_FATAL_ERR";
	    case ::ibv_wc_status::IBV_WC_RESP_TIMEOUT_ERR:
            return "IBV_WC_RESP_TIMEOUT_ERR";
	    case ::ibv_wc_status::IBV_WC_GENERAL_ERR:
            return "IBV_WC_GENERAL_ERR";
	    case ::ibv_wc_status::IBV_WC_TM_ERR:
            return "IBV_WC_TM_ERR";
	    case ::ibv_wc_status::IBV_WC_TM_RNDV_INCOMPLETE:
            return "IBV_WC_TM_RNDV_INCOMPLETE";
        default:
            return "error status";
        }
    }

public:

    completion_queue() = delete;

    completion_queue(const int cqe, protection_domain& pd)
      : _cq{::ibv_create_cq(pd.device_context(), cqe, nullptr, nullptr, 0)} {
        if (not _cq) {
            throw std::runtime_error{"create cq failed"};
        }
    }

    completion_queue(const completion_queue&) = delete;

    completion_queue(completion_queue&&) = default;

    completion_queue& operator=(const completion_queue&) = delete;

    completion_queue& operator=(completion_queue&&) = delete;

    ~completion_queue() noexcept {
        SPDK_DEBUGLOG(msg, "call ~completion_queue()\n");
        destroy_cq();
    }

public:

    ::ibv_cq* cq() noexcept { return _cq; }

    int poll(::ibv_wc* wcs, const size_t poll_size) noexcept {
        int rc = ::ibv_poll_cq(_cq, poll_size, wcs);
        if (rc < 0) {
            errno = rc;
            return 0;
        } else if (rc > 0) {
            return rc;
        }

        return 0;
    }

private:

    void drain_cq() noexcept {
        int rc{1};
        ::ibv_wc wc{};
        while (rc > 0) {
            rc = 0;
            rc = ::ibv_poll_cq(_cq, 1, &wc);
            SPDK_INFOLOG(msg, "drain cq, polled %d cqe(s)\n", rc);
        }
    }

    void destroy_cq() noexcept {
        drain_cq();
        SPDK_INFOLOG(msg, "going to destroy cq\n");
        auto rc = ::ibv_destroy_cq(_cq);

        if (rc == EBUSY) {
            SPDK_ERRLOG("destroy cq failed: %s, will drain cq and try again\n", std::strerror(rc));
            drain_cq();
            rc = ::ibv_destroy_cq(_cq);

            if (rc != 0) {
                SPDK_ERRLOG("destroy cq failed: %s\n", std::strerror(rc));
            }
        } else if (rc != 0) {
            SPDK_ERRLOG("destroy cq failed: %s\n", std::strerror(rc));
        } else {
            SPDK_INFOLOG(msg, "cq destroyed\n");
        }
    }

private:

    ::ibv_cq* _cq{nullptr};
};

} // namespace rdma
} // namespace msg
