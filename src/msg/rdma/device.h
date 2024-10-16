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


#include <spdk/log.h>

#include <concepts>
#include <cstring>
#include <filesystem>
#include <functional>
#include <numeric>
#include <memory>
#include <vector>
#include <string>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include <poll.h>

namespace fs = std::filesystem;

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
        construct_dev_ip_map();
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
            // In non-blocking mode: -1 means there isn't any async event to read
            if (ret == -1) { continue; }
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

    auto default_device() noexcept {
        return _default_device;
    }

    std::optional<std::string> get_ipv4() noexcept {
        return _default_ipv4;
    }

    std::optional<std::string> get_ipv4(const std::optional<std::string>& dev) {
        if (not dev) {
            return _default_ipv4;
        }

        auto it = _dev_ipv4_map.find(*dev);
        if (it == _dev_ipv4_map.end()) {
            return _default_ipv4;
        }

        return it->second;
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
        case IBV_LINK_LAYER_UNSPECIFIED: return "Unspecified";
        case IBV_LINK_LAYER_INFINIBAND: return "InfiniBand";
        case IBV_LINK_LAYER_ETHERNET: return "Ethernet";
        default: return "Unknown";
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

private:

    std::string read_ipv4(const fs::path& home_path, const fs::path& rel_dev_path, fs::path ifr_path) {

        auto ifr_abs_path = home_path / rel_dev_path / "net" / ifr_path;
        auto ifr_soft_master_path = ifr_abs_path / "master";
        bool is_bridge = fs::exists(ifr_soft_master_path);

        if (is_bridge) {
            auto bridge_abs_path = fs::read_symlink(ifr_soft_master_path);
            ifr_path = bridge_abs_path.filename();
            SPDK_DEBUGLOG(
              msg,
              "bridge path: %s, target ifr is: %s\n",
              bridge_abs_path.c_str(), ifr_path.c_str());
        }

        ::ifreq ifr{};
        auto fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        ifr.ifr_addr.sa_family = AF_INET;
        ::sprintf(ifr.ifr_name, "%s", ifr_path.c_str());
        ::ioctl(fd, SIOCGIFADDR, &ifr);
        auto* sock_in = reinterpret_cast<::sockaddr_in*>(&ifr.ifr_addr);
        auto ipv4 = ::inet_ntoa(sock_in->sin_addr);
        ::close(fd);

        return ipv4;
    }

    void construct_dev_ip_map() {
        static constexpr char ib_dev_home[]{"/sys/class/infiniband"};
        static constexpr char interface_home[]{"/sys/class/net"};
        static constexpr int pci_addr_index{5};

        namespace fs = std::filesystem;
        fs::path ib_dev_path{ib_dev_home};
        fs::path interface_path{interface_home};
        for (const auto& entry : fs::directory_iterator(ib_dev_path)) {
            auto dev_name = entry.path().filename();
            auto ib_pci_path = fs::relative(entry.path(), ib_dev_path).parent_path().parent_path();
            SPDK_DEBUGLOG(
              msg,
              "device name: %s, pci path: %s\n",
              dev_name.c_str(), ib_pci_path.c_str());

            for (const auto& interface : fs::directory_iterator(interface_path)) {
                auto ipci_path = fs::relative(interface.path(), interface_path).parent_path().parent_path();
                SPDK_DEBUGLOG(
                  msg,
                  "interface name: %s, ipci path: %s\n",
                  interface.path().filename().c_str(),
                  ipci_path.c_str());

                if (ipci_path != ib_pci_path) { continue; }
                auto ipv4 = read_ipv4(ib_dev_path, ib_pci_path, interface.path().filename());
                SPDK_INFOLOG(msg, "device %s ipv4: %s\n", dev_name.c_str(), ipv4.c_str());
                _dev_ipv4_map.emplace(dev_name.string(), ipv4);
            }
        }
    }

    void filter_active_port(::ibv_device** devices) {
        bool should_allocate_device_attr = false;
        auto port_attr = std::make_shared<::ibv_port_attr>();
        auto device_attr = std::make_shared<::ibv_device_attr_ex>();
        ::ibv_context* context;

        while (*devices) {
            context = ::ibv_open_device(*devices);
            auto dev_name = device_name(*devices);
            if (!context) {
                SPDK_ERRLOG(
                  "open device '%s' failed, will skip this device\n",
                  dev_name.c_str());

                continue;
            }

            if (::ibv_query_device_ex(context, nullptr, device_attr.get())) {
                SPDK_ERRLOG(
                  "query rdma device %s failed, will skip this device\n",
                  dev_name.c_str());

                ::ibv_close_device(context);
                continue;
            }

            for (uint8_t port = 1;
                 port <= device_attr->orig_attr.phys_port_cnt;
                 ++port) {
                if (::ibv_query_port(context, port, port_attr.get())) {
                    SPDK_ERRLOG(
                      "query port %u of %s failed, will skip this port",
                      port, dev_name.c_str());

                    continue;
                }

                if (port_attr->state != IBV_PORT_ACTIVE) {
                    SPDK_INFOLOG(
                      msg,
                      "port %u of %s is %s, will skip this port\n",
                      port, dev_name.c_str(),
                      port_state_name(port_attr->state).c_str());

                    continue;
                }

                SPDK_NOTICELOG(
                  "port %u of %s is %s, link layer is %s\n",
                  port, dev_name.c_str(),
                  port_state_name(port_attr->state).c_str(),
                  link_layer_str(port_attr->link_layer));

                std::vector<::ibv_gid> gids{};
                for (int i{0}; i < port_attr->gid_tbl_len; ++i) {
                    ::ibv_gid cur_gid{};
                    auto rc = ::ibv_query_gid(context, port, i, &(cur_gid));
                    if (rc != 0) {
                        SPDK_ERRLOG(
                          "ERROR: Query gid on device %s, port %u, index %d failed, return code %d\n",
                          dev_name.c_str(), port, i, rc);
                        continue;
                    }

                    if (port_attr->link_layer == IBV_LINK_LAYER_ETHERNET and is_empty_gid(cur_gid.raw) or is_link_local(cur_gid.raw)) {
                        continue;
                    }

                    auto dev_ip_it = _dev_ipv4_map.find(dev_name);
                    if (not _default_device and dev_ip_it != _dev_ipv4_map.end()) {
                        _default_device = dev_name;
                        _default_ipv4 = dev_ip_it->second;
                        SPDK_NOTICELOG(
                          "default rdma device is %s, default ip address is %s\n",
                          _default_device->c_str(), dev_ip_it->second.c_str());
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
    std::optional<std::string> _default_ipv4{std::nullopt};
    std::optional<std::string> _default_device{std::nullopt};
    std::unordered_map<std::string, std::string> _dev_ipv4_map{};

    static constexpr size_t _raw_gid_len{16};
    static constexpr size_t _raw_gid_ipv4_begin{12};
};

}
} // namespace msg
