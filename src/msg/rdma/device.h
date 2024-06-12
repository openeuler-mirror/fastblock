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

#include "types.h"
#include "utils/fmt.h"
#include "utils/log.h"

#include <spdk/log.h>

#include <concepts>
#include <cstring>
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
        std::vector<::ibv_gid> gids;
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
            SPDK_ERRLOG_EX("get device list failed\n");
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
                SPDK_ERRLOG_EX("ERROR: ibv_get_async_event() on '%s' return %d\n",
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

    // 默认参数表示返回第一个 active 且有 ipv4 的
    std::optional<std::string> query_ipv4(
      std::optional<std::string> dev_name = std::nullopt,
      std::optional<uint8_t> dev_port = std::nullopt,
      std::optional<int> gid_index = std::nullopt) {
        if (not dev_name and not dev_port and not gid_index) {
            return _default_ipv4;
        }

        device_context* target_device{nullptr};
        if (dev_name) {
            auto ctx_it = std::find_if(
              _contexts.begin(),
              _contexts.end(),
              [&dev_name, &dev_port] (const device_context& ctx) {
                  if (*dev_name == std::string(::ibv_get_device_name(ctx.device))) {
                      if (dev_port) {
                          return ctx.port == *dev_port;
                      } else {
                          return true;
                      }
                  } else {
                      return false;
                  }
              }
            );

            if (ctx_it == _contexts.end()) {
                SPDK_ERRLOG_EX(
                  "ERROR: Query the ipv4 of %s on port %d failed, no such device or port\n",
                  dev_name->c_str(), dev_port ? *dev_port : 1);
                return std::nullopt;
            }

            target_device = &(*ctx_it);
        } else {
            target_device = &(_contexts[0]);
        }

        ::ibv_gid* target_gid{nullptr};
        if (gid_index) {
            if (static_cast<size_t>(*gid_index) > target_device->gids.size()) {
                SPDK_ERRLOG_EX(
                  "ERROR: Specified gid index %d out of range, max index is %ld\n",
                  *gid_index, target_device->gids.size() - 1);

                return std::nullopt;
            }

            if (is_gid_contain_ipv4(target_device->gids[*gid_index].raw)) {
                return ipv4_from_gid(target_device->gids[*gid_index].raw);
            }

            return std::nullopt;
        }

        for (auto& gid : target_device->gids) {
            if (not is_gid_contain_ipv4(gid.raw)) {
                continue;
            }

            return ipv4_from_gid(gid.raw);
        }

        return std::nullopt;
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

    static const char* link_layer_str(uint8_t link_layer) {
        switch (link_layer) {
        case IBV_LINK_LAYER_UNSPECIFIED:
            return "Unspecified";
        case IBV_LINK_LAYER_INFINIBAND:
            return "InfiniBand";
        case IBV_LINK_LAYER_ETHERNET:
            return "Ethernet";
        default:
            return "Unknown";
        }
    }

    static std::string device_name(::ibv_device* device) {
        return std::string(::ibv_get_device_name(device));
    }

    static bool is_empty_gid(uint8_t* gid_raw) noexcept {
        static constexpr uint8_t raw_empty_gid[_raw_gid_len]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        return std::memcmp(gid_raw, raw_empty_gid, _raw_gid_len) == 0;
    }

    static bool is_link_local(uint8_t* gid_raw) noexcept {
        static constexpr uint8_t link_local_gid[_raw_gid_len]{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        return std::memcmp(gid_raw, link_local_gid, _raw_gid_len) == 0;
    }

    static bool is_gid_contain_ipv4(uint8_t* gid_raw) noexcept {
        static constexpr size_t raw_gid_prefix_len{2};
        static constexpr uint8_t tgt_gid_prefix[raw_gid_prefix_len]{0, 0};
        return std::memcmp(gid_raw, tgt_gid_prefix, raw_gid_prefix_len) == 0;
    }

    std::string ipv4_from_gid(uint8_t* raw_gid) {
        return FMT_4(
          "%1%.%2%.%3%.%4%",
          std::to_string(raw_gid[_raw_gid_ipv4_begin]),
          std::to_string(raw_gid[_raw_gid_ipv4_begin + 1]),
          std::to_string(raw_gid[_raw_gid_ipv4_begin + 2]),
          std::to_string(raw_gid[_raw_gid_ipv4_begin + 3]));
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
                SPDK_ERRLOG_EX(
                  "open device '%s' failed, will skip this device\n",
                  device_name(*devices).c_str());

                continue;
            }

            if (::ibv_query_device_ex(context, nullptr, device_attr.get())) {
                SPDK_ERRLOG_EX(
                  "query rdma device %s failed, will skip this device\n",
                  device_name(*devices).c_str());

                ::ibv_close_device(context);
                continue;
            }

            for (uint8_t port = 1;
                 port <= device_attr->orig_attr.phys_port_cnt;
                 ++port) {
                if (::ibv_query_port(context, port, port_attr.get())) {
                    SPDK_ERRLOG_EX(
                      "query port %u of %s failed, will skip this port",
                      port, device_name(*devices).c_str());

                    continue;
                }

                if (port_attr->state != IBV_PORT_ACTIVE) {
                    SPDK_INFOLOG_EX(
                      msg,
                      "port %u of %s is %s, will skip this port\n",
                      port, device_name(*devices).c_str(),
                      port_state_name(port_attr->state).c_str());

                    continue;
                }

                SPDK_NOTICELOG_EX(
                  "port %u of %s is %s, link layer is %s\n",
                  port, device_name(*devices).c_str(),
                  port_state_name(port_attr->state).c_str(),
                  link_layer_str(port_attr->link_layer));

                std::vector<::ibv_gid> gids{};
                for (int i{0}; i < port_attr->gid_tbl_len; ++i) {
                    ::ibv_gid cur_gid{};
                    auto rc = ::ibv_query_gid(context, port, i, &(cur_gid));
                    if (rc != 0) {
                        SPDK_ERRLOG_EX(
                          "ERROR: Query gid on device %s, port %u, index %d failed, return code %d\n",
                          device_name(*devices).c_str(), port, i, rc);
                        continue;
                    }

                    if (is_empty_gid(cur_gid.raw) or is_link_local(cur_gid.raw)) {
                        continue;
                    }

                    if (not _default_ipv4 and is_gid_contain_ipv4(cur_gid.raw)) {
                        _default_ipv4 = ipv4_from_gid(cur_gid.raw);
                        SPDK_DEBUGLOG_EX(msg, "default ipv4 is %s\n", _default_ipv4->c_str());
                    }

                    gids.push_back(cur_gid);
                }

                gids.shrink_to_fit();
                _contexts.emplace_back(*devices, context, port, port_attr, device_attr, std::move(gids));
                port_attr = std::make_shared<::ibv_port_attr>();
                should_allocate_device_attr = true;
            }

            if (should_allocate_device_attr) {
                device_attr = std::make_shared<::ibv_device_attr_ex>();
                should_allocate_device_attr = false;
            }

            ++devices;
        }

        if (_contexts.empty()) {
            SPDK_ERRLOG_EX("no active port\n");
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
    std::optional<std::string> _default_ipv4{std::nullopt};

    static constexpr size_t _raw_gid_len{16};
    static constexpr size_t _raw_gid_ipv4_begin{12};
};

}
} // namespace msg
