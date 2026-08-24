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

#include "fastblock/msg/types.h"

#include <functional>
#include <string>

#include <google/protobuf/service.h>

struct ibv_pd;

namespace msg {
namespace rdma {

class rpc_controller : public google::protobuf::RpcController {

public:

    rpc_controller() = default;

public:

    virtual void Reset() override {
        _failed = false;
        _error_reason.clear();
        _error_class = rpc_error_class::ok;
        _pd = nullptr;
        _peer_address.clear();
        _canceled = false;
        _cancel_fn = nullptr;
        _notify_on_cancel = nullptr;
    };

    virtual bool Failed() const override { return _failed; }

    virtual std::string ErrorText() const override { return _error_reason; }

    virtual void StartCancel() override {
        if (_canceled) {
            return;
        }
        _canceled = true;
        if (_cancel_fn) {
            auto fn = std::move(_cancel_fn);
            fn();
        }
        if (_notify_on_cancel) {
            auto* cb = _notify_on_cancel;
            _notify_on_cancel = nullptr;
            cb->Run();
        }
    }

    virtual void SetFailed(const std::string& error) override {
        SetFailed(error, classify_rpc_error(error));
    }

    void SetFailed(const std::string& error, rpc_error_class cls) {
        _failed = true;
        _error_reason = error;
        _error_class = cls;
    }

    virtual bool IsCanceled() const override { return _canceled; }

    virtual void NotifyOnCancel(::google::protobuf::Closure* callback) override {
        _notify_on_cancel = callback;
    }

    void set_cancel_fn(std::function<void()> fn) { _cancel_fn = std::move(fn); }

    rpc_error_class error_class() const noexcept {
        return _failed ? _error_class : rpc_error_class::ok;
    }

    bool is_peer_terminating() noexcept {
        return _failed && _error_class == rpc_error_class::peer_terminating;
    }

    bool is_soft_failure() const noexcept {
        return _failed && is_retryable(_error_class);
    }

    bool is_connection_failure() const noexcept {
        return _failed && tears_connection(_error_class);
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
    rpc_error_class _error_class{rpc_error_class::ok};
    ::ibv_pd* _pd{nullptr};
    std::string _peer_address{};
    bool _canceled{false};
    std::function<void()> _cancel_fn{};
    ::google::protobuf::Closure* _notify_on_cancel{nullptr};
};

inline void fail_rpc(
  google::protobuf::RpcController* ctrlr,
  const std::string& text,
  rpc_error_class cls) {
    if (auto* rc = static_cast<rpc_controller*>(ctrlr)) {
        rc->SetFailed(text, cls);
    } else if (ctrlr) {
        ctrlr->SetFailed(text);
    }
}

} // namespace rdma
} // namespace msg
