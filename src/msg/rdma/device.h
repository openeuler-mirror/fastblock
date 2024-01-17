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

#include "types.h"

#include <spdk/log.h>

#include <concepts>
#include <functional>
#include <numeric>
#include <memory>
#include <vector>
#include <string>

#include <infiniband/verbs.h>
#include <poll.h>

namespace msg {
namespace rdma {

class device {

private:

    using port_attr_ptr = std::shared_ptr<::ibv_port_attr>;

    using device_attr_ptr = std::shared_ptr<::ibv_device_attr_ex>;

public:

    struct device_context {
        ::ibv_device*     device;
        ::ibv_context*    context;
        uint8_t         port;
        port_attr_ptr   port_attr;
        device_attr_ptr device_attr;
    };

    struct ib_context {
        std::unique_ptr<::pollfd[]> event_pollfds{nullptr};
        std::unique_ptr<::ibv_context*[]> ibv_ctxs{nullptr};
    };

public:

    device(std::optional<std::function<void(::ibv_context*, ::ibv_async_event*)>>&& on_polled_ib_evt = std::nullopt)
      : _on_polled_ib_evt{std::move(on_polled_ib_evt)} {
        ::ibv_device** dev_list = ::ibv_get_device_list(nullptr);

        if (not dev_list) {
            SPDK_ERRLOG("get device list failed\n");
            throw std::runtime_error{"get device list failed"};
        }

        filter_active_port(dev_list);
        ::ibv_free_device_list(dev_list);
    }

    device(const device&) = delete;

    device& operator=(const device&) = delete;

    device(device&&) = default;

    device& operator=(device&&) = delete;

    ~device() noexcept {
        if (_contexts.empty()) {
            return;
        }

        int ret{0};
        std::accumulate(
          _contexts.begin(),
          _contexts.end(),
          reinterpret_cast<device_context*>(0),
          [&ret] (device_context* acc, device_context& ctx) {
              if (!acc or acc->context != ctx.context) {
                  ::ibv_close_device(ctx.context);
              }

              return &ctx;
          }
        );
    }

public:

    template<typename Func>
    requires requires(Func&& f, device_context* ctx) {
        { f(ctx) } -> std::same_as<iterate_tag>;
    }
    void fold_device_while(Func&& fn) {
        for (auto& ctx : _contexts) {
            if (fn(&ctx) == iterate_tag::stop) {
                break;
            }
        }
    }

    void process_ib_event() {
        auto ret = ::poll(_ib_event_pollfds.get(), _contexts.size(), 0);
        if (ret <= 0) {
            return;
        }

        ::ibv_async_event evt{};
        for (size_t i{0}; i < _contexts.size(); ++i) {
            ret = ::ibv_get_async_event(_contexts[i].context, &evt);
            if (ret) {
                SPDK_ERRLOG("ERROR: ibv_get_async_event() on '%s' return %d\n",
                  device_name(_contexts[i].device).c_str(), ret);
                continue;
            }

            if (_on_polled_ib_evt) {
                try {
                    _on_polled_ib_evt.value()(_contexts[i].context, &evt);
                } catch (...) {
                    ::ibv_ack_async_event(&evt);
                    std::rethrow_exception(std::current_exception());
                }
            }
            ::ibv_ack_async_event(&evt);
        }
    }

private:

    static std::string port_state_name(enum ::ibv_port_state pstate) {
        switch (pstate) {
        case IBV_PORT_DOWN:   return "PORT_DOWN";
        case IBV_PORT_INIT:   return "PORT_INIT";
        case IBV_PORT_ARMED:  return "PORT_ARMED";
        case IBV_PORT_ACTIVE: return "PORT_ACTIVE";
        default:              return "invalid state";
        }
    }

    static std::string device_name(::ibv_device* device) {
        return std::string(ibv_get_device_name(device));
    }

private:

    void filter_active_port(::ibv_device** devices) {
        bool should_allocate_device_attr = false;
        auto port_attr = std::make_shared<::ibv_port_attr>();
        auto device_attr = std::make_shared<::ibv_device_attr_ex>();
        ::ibv_context* context;

        while (*devices) {
            context = ::ibv_open_device(*devices);
            if (!context) {
                SPDK_ERRLOG(
                  "open device '%s' failed, will skip this device\n",
                  device_name(*devices).c_str());

                continue;
            }

            if (::ibv_query_device_ex(context, nullptr, device_attr.get())) {
                SPDK_ERRLOG(
                  "query rdma device %s failed, will skip this device\n",
                  device_name(*devices).c_str());

                ::ibv_close_device(context);
                continue;
            }

            for (uint8_t port = 1;
                 port <= device_attr->orig_attr.phys_port_cnt;
                 ++port) {
                if (::ibv_query_port(context, port, port_attr.get())) {
                    SPDK_ERRLOG(
                      "query port %u of %s failed, will skip this port",
                      port, device_name(*devices).c_str());

                    continue;
                }

                if (port_attr->state != IBV_PORT_ACTIVE) {
                    SPDK_INFOLOG(
                      msg,
                      "port %u of %s is %s, will skip this port\n",
                      port, device_name(*devices).c_str(),
                      port_state_name(port_attr->state).c_str());

                    continue;
                }

                SPDK_DEBUGLOG(
                  msg,
                  "port %u of %s is %s\n",
                  port, device_name(*devices).c_str(),
                  port_state_name(port_attr->state).c_str());

                _contexts.emplace_back(*devices, context, port, port_attr, device_attr);
                port_attr = std::make_shared<ibv_port_attr>();
                should_allocate_device_attr = true;
            }

            if (should_allocate_device_attr) {
                device_attr = std::make_shared<ibv_device_attr_ex>();
                should_allocate_device_attr = false;
            }

            ++devices;
        }

        if (_contexts.empty()) {
            SPDK_ERRLOG("no active port\n");
            throw std::runtime_error{"no active port"};
        }

        _contexts.shrink_to_fit();

        _ib_event_pollfds = std::make_unique<::pollfd[]>(_contexts.size());
        int flag;
        for (size_t i{0}; i < _contexts.size(); ++i) {
            flag = ::fcntl(_contexts[i].context->async_fd, F_GETFL);
            ::fcntl(_contexts[i].context->async_fd, F_SETFL, flag | O_NONBLOCK);
            _ib_event_pollfds[i].fd = _contexts[i].context->async_fd;
            _ib_event_pollfds[i].events = POLLIN;
        }
    }

private:

    std::optional<std::function<void(::ibv_context*, ::ibv_async_event*)>> _on_polled_ib_evt{std::nullopt};
    std::vector<device_context> _contexts{};
    std::unique_ptr<::pollfd[]> _ib_event_pollfds{nullptr};
};

}
} // namespace msg
