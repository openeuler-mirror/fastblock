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

#include <string>

#include <google/protobuf/service.h>

struct ibv_pd;

namespace msg {
namespace rdma {

class rpc_controller : public google::protobuf::RpcController {

public:

    rpc_controller() = default;

public:

    virtual void Reset() override { /*TODO:*/ };

    virtual bool Failed() const override { return _failed; }

    virtual std::string ErrorText() const override { return _error_reason; }

    virtual void StartCancel() override { /*TODO:*/ }

    virtual void SetFailed(const std::string& error) override {
        _failed = true;
        _error_reason = error;
    }

    virtual bool IsCanceled() const override { return false; /*TODO:*/ }

    virtual void NotifyOnCancel(::google::protobuf::Closure* /* callback */) override {
        /*TODO:*/
    }

    bool is_peer_terminating() noexcept {
        return _failed and _error_reason == "terminating";
    }

    void attach_pd(::ibv_pd* pd) noexcept {
        _pd = pd;
    }

    ::ibv_pd* pd() const noexcept {
        return _pd;
    }

    void attach_peer_address(std::string peer_address) {
        _peer_address = std::move(peer_address);
    }

    const std::string& peer_address() const noexcept {
        return _peer_address;
    }

private:
    bool _failed{false};
    std::string _error_reason{""};
    ::ibv_pd* _pd{nullptr};
    std::string _peer_address{};
};

} // namespace rdma
} // namespace msg
