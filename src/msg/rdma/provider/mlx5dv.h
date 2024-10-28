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

struct mlx5dv : public provider {

    static inline int ipv6_addr_v4mapped(const struct in6_addr *a) {
        return ((a->s6_addr32[0] | a->s6_addr32[1]) |
                (a->s6_addr32[2] ^ htonl(0x0000ffff))) == 0UL ||
                /* IPv4 encoded multicast addresses */
                (a->s6_addr32[0] == htonl(0xff0e0000) &&
                ((a->s6_addr32[1] |
                (a->s6_addr32[2] ^ htonl(0x0000ffff))) == 0UL));
    }

    static void get_best_gid_index (::ibv_context* ctx, ::ibv_qp_attr* qp_attr, int port = 1) {
        int gid_index = 3, i;
        union ::ibv_gid temp_gid, temp_gid_rival;
        int is_ipv4, is_ipv4_rival;

        ibv_query_gid(ctx, port, gid_index, &temp_gid);

        SPDK_DEBUGLOG(msg, "best gid index is %d\n", gid_index);
        std::memcpy(qp_attr->ah_attr.grh.dgid.raw, temp_gid.raw, 16);
        qp_attr->ah_attr.grh.sgid_index = gid_index;

        return;


        ::ibv_port_attr attr{};
        ::ibv_query_port(ctx, port, &attr);

        for (i = 1; i < attr.gid_tbl_len; i++) {
            if (ibv_query_gid(ctx, port, gid_index, &temp_gid)) {
                return;
            }

            if (ibv_query_gid(ctx, port, i, &temp_gid_rival)) {
                return;
            }

            is_ipv4 = ipv6_addr_v4mapped((::in6_addr *)temp_gid.raw);
            is_ipv4_rival = ipv6_addr_v4mapped((::in6_addr *)temp_gid_rival.raw);

            if (is_ipv4_rival && !is_ipv4)
                gid_index = i;
            else if (!is_ipv4_rival && is_ipv4)
                gid_index = i;

            if (gid_index == 3) {
                break;
            }
#ifdef HAVE_GID_TYPE
            else {
#ifdef HAVE_GID_TYPE_DECLARED
                struct ibv_gid_entry roce_version, roce_version_rival;

                if (ibv_query_gid_ex(ctx->context, port, gid_index, &roce_version, 0))
                    continue;

                if (ibv_query_gid_ex(ctx->context, port, i, &roce_version_rival, 0))
                    continue;

                //coverity[uninit_use_in_call]
                if (check_better_roce_version(roce_version.gid_type, roce_version_rival.gid_type) == RIGHT_IS_BETTER) {
                    gid_index = i;
                }

#else
                enum ibv_gid_type roce_version, roce_version_rival;

                if (ibv_query_gid_type(ctx->context, port, gid_index, &roce_version))
                    continue;

                if (ibv_query_gid_type(ctx->context, port, i, &roce_version_rival))
                    continue;

                if (check_better_roce_version(roce_version, roce_version_rival) == RIGHT_IS_BETTER) {
                    gid_index = i;
                }
#endif
            }
#endif
        }

        SPDK_DEBUGLOG(msg, "best gid index is %d\n", gid_index);
        std::memcpy(qp_attr->ah_attr.grh.dgid.raw, temp_gid.raw, 16);
        qp_attr->ah_attr.grh.sgid_index = gid_index;
    }

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

        auto* ibv_ctx = reinterpret_cast<::ibv_context*>(ctx);
        ::ibv_qp_init_attr_ex dv_qp_attr = {
            .qp_context = attr->qp_context,
            .send_cq = attr->send_cq,
            .recv_cq = attr->recv_cq,
            .srq = attr->srq,
            .cap = attr->cap,
            .qp_type = attr->qp_type,
            .comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
            .pd = pd
        };

        auto* qp = ::mlx5dv_create_qp(ibv_ctx, &dv_qp_attr, nullptr);

        if (!qp) {
            return nullptr;
        }

        attr->cap = dv_qp_attr.cap;

        return qp;
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

    int accept(::rdma_cm_id* id, ::rdma_conn_param* param) final {
        init_qp(id);
        return ::rdma_accept(id, param);
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
