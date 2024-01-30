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

#include <cstdint>

#include "utils/fmt.h"

namespace msg {
namespace rdma {

class probe {

public:

    probe() = default;

    probe(const probe&) = default;

    probe(probe&&) = default;

    probe& operator=(const probe&) = default;

    probe& operator=(probe&&) = default;

    ~probe() noexcept = default;

public:

    void send_wr_posted(const std::size_t n = 1) noexcept {
        posted_send_wr += n;
        send_queue_depth += n;
    }

    void receive_wr_posted(const std::size_t n = 1) noexcept {
        posted_receive_wr += n;
        receive_queue_depth += n;
    }

    void cqe_received(const std::size_t n = 1) noexcept {
        received_cqe += n;
        receive_queue_depth -= n;
    }

    void send_wc_received(const std::size_t n = 1) noexcept {
        received_sent_wc += n;
    }

    std::string fmt() {
        return FMT_6(
          "{posted_send_wr: %1%, posted_receive_wr: %2%, send_queue_depth: %3%, receive_queue_depth: %4%, received_cqe: %5%, received_sent_wc: %6%}",
          posted_send_wr, posted_receive_wr, send_queue_depth, receive_queue_depth, received_cqe, received_sent_wc);
    }

public:

    int64_t posted_send_wr{0};
    int64_t posted_receive_wr{0};
    int64_t send_queue_depth{0};
    int64_t receive_queue_depth{0};
    int64_t received_cqe{0};
    int64_t received_sent_wc{0};
};

} // namespace rdma
} // namespace msg

