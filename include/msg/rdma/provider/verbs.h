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

#include "msg/rdma/provider.h"
#include "utils/fmt.h"

#include <cstring>
#include <exception>

#include <spdk/log.h>

#include <infiniband/mlx5dv.h>
#include <limits>

namespace msg {
namespace rdma {

struct verbs : public provider {

    static void init_qp(::rdma_cm_id* id) {
        ::ibv_qp_attr qp_attr{};
        qp_attr.qp_state = IBV_QPS_INIT;

        int ret{0};

        int qp_attr_mask{0};
        ret = ::rdma_init_qp_attr(id, &qp_attr, &qp_attr_mask);
        if (ret) {
            throw std::runtime_error{
              FMT_1("failed to init attr IBV_QPS_INIT, error: %1%", std::strerror(errno))};
        }

        ret = ::ibv_modify_qp(id->qp, &qp_attr, qp_attr_mask);
        if (ret) {
            throw std::runtime_error{
              FMT_1("failed to modify qp state to IBV_QPS_INIT, error: %1%", std::strerror(ret))};
        }

        qp_attr.qp_state = IBV_QPS_RTR;
        ret = ::rdma_init_qp_attr(id, &qp_attr, &qp_attr_mask);
        if (ret) {
            throw std::runtime_error{
              FMT_1("failed to init attr IBV_QPS_RTR, error: %1%", std::strerror(errno))};
        }

        ret = ::ibv_modify_qp(id->qp, &qp_attr, qp_attr_mask);
        if (ret) {
            throw std::runtime_error{
              FMT_1("failed to modify qp state to IBV_QPS_RTR, error: %1%", std::strerror(ret))};
        }

        qp_attr.qp_state = IBV_QPS_RTS;
        ret = ::rdma_init_qp_attr(id, &qp_attr, &qp_attr_mask);
        if (ret) {
            throw std::runtime_error{
              FMT_1("failed to init attr IBV_QPS_RTS, error: %1%", std::strerror(errno))};
        }

        ret = ::ibv_modify_qp(id->qp, &qp_attr, qp_attr_mask);
        if (ret) {
            throw std::runtime_error{
              FMT_1("failed to modify qp state to IBV_QPS_RTS, error: %1%", std::strerror(ret))};
        }
    }

    ::ibv_qp*
    create_qp(void* ctx, ::ibv_pd* pd, ::ibv_qp_init_attr* attr) noexcept final {
        assert(ctx && "null id");
        assert(pd && "null pd");
        assert(attr && "null qp init attr");

        auto* cm_id = reinterpret_cast<::rdma_cm_id*>(ctx);
        assert(cm_id->pd == pd && "error pd");
        auto rc = ::rdma_create_qp(cm_id, pd, attr);

        if (rc) {
            SPDK_ERRLOG("ERROR: Create qp failed: %s\n", std::strerror(errno));
            return nullptr;
        }

        return cm_id->qp;
    }

    void complete_qp_connect(::rdma_cm_id* id) final {
        assert(id && "null id");
        assert(id->qp && "null qp");

        init_qp(id);

        int ret = ::rdma_establish(id);
        if (ret) {
            throw std::runtime_error{"rdma establish connection failed"};
        }

    }

    void accept(::rdma_cm_id* id, ::rdma_conn_param* param) final {
        init_qp(id);
        if (auto rc = ::rdma_accept(id, param); rc != 0) {
            throw std::runtime_error{"accept error"};
        }
    }

    int disconnect(::rdma_cm_id* id) noexcept final {
        assert(id && "null id");

        int ret{0};
        if (id->qp) {
            ::ibv_qp_attr qp_attr = {.qp_state = IBV_QPS_ERR};

            ret = ::ibv_modify_qp(id->qp, &qp_attr, IBV_QP_STATE);
            if (ret) {
                SPDK_ERRLOG("failed to modify qp state to IBV_QPS_ERR, error: %s\n", std::strerror(ret));
                errno = ret;

                return ret;
            }
        }

        ret = ::rdma_disconnect(id);
        if (ret) {
            SPDK_ERRLOG("rdma_disconnect() failed, error: %s\n", std::strerror(errno));
            return ret;
        }

        return 0;
    }
};

} // namespace rdma
} // namespace msg

