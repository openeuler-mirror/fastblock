/* Copyright (c) 2024 ChinaUnicom
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

#include <rdma/rdma_cma.h>
#include <infiniband/mlx5dv.h>

namespace msg {
namespace rdma {

struct provider {
    virtual ::ibv_qp* create_qp(::ibv_context*, ::ibv_pd*, ::ibv_qp_init_attr*) noexcept = 0;
    virtual void complete_qp_connect(::rdma_cm_id*) = 0;
    virtual void accept(::rdma_cm_id*, rdma_conn_param*) = 0;
    virtual int disconnect(::rdma_cm_id*) noexcept = 0;
};

} // namespace msg
} // namespace rpc
