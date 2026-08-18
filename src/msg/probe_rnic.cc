/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/probe_rnic.h"

#include <cstdlib>
#include <filesystem>
#include <string_view>

#ifdef FASTBLOCK_WITH_RDMA
#include <infiniband/verbs.h>
#endif

namespace msg {
namespace {

bool looks_like_rxe_name(std::string_view name) noexcept {
    if (name == "rdmanic") {
        return true;
    }
    return name.size() >= 3 && name.substr(0, 3) == "rxe";
}

bool looks_like_rxe_sysfs(const std::string& name) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path ib = fs::path("/sys/class/infiniband") / name;
    const fs::path device = ib / "device";
    if (!fs::exists(device, ec)) {
        return looks_like_rxe_name(name);
    }
    const auto drv = fs::read_symlink(device / "driver", ec);
    if (ec) {
        return false;
    }
    const auto s = drv.string();
    return s.find("rdma_rxe") != std::string::npos;
}

} // namespace

bool env_allow_rxe() noexcept {
    const char* v = std::getenv("FASTBLOCK_USE_RXE");
    return v != nullptr && v[0] == '1' && v[1] == '\0';
}

#ifdef FASTBLOCK_WITH_RDMA
rnic_probe probe_rnic(const boost::property_tree::ptree& conf, bool allow_rxe) noexcept {
    rnic_probe out;
    std::string want;
    const auto it = conf.find("rdma_device_name");
    if (it != conf.not_found()) {
        try {
            want = it->second.get_value<std::string>();
        } catch (...) {
            want.clear();
        }
    }

    int n = 0;
    ::ibv_device** list = ::ibv_get_device_list(&n);
    if (!list) {
        out.detail = "ibv_get_device_list failed";
        return out;
    }

    std::string seen;
    for (int i = 0; list[i] != nullptr; ++i) {
        const std::string name = ::ibv_get_device_name(list[i]);
        if (!seen.empty()) {
            seen += ',';
        }
        seen += name;
        if (!want.empty() && name != want) {
            continue;
        }
        const bool rxe = looks_like_rxe_name(name) || looks_like_rxe_sysfs(name);
        if (rxe && !allow_rxe) {
            continue;
        }
        ::ibv_context* ctx = ::ibv_open_device(list[i]);
        if (!ctx) {
            continue;
        }
        ::ibv_device_attr_ex attr{};
        if (::ibv_query_device_ex(ctx, nullptr, &attr) != 0) {
            ::ibv_close_device(ctx);
            continue;
        }
        bool active = false;
        for (uint8_t port = 1; port <= attr.orig_attr.phys_port_cnt; ++port) {
            ::ibv_port_attr pa{};
            if (::ibv_query_port(ctx, port, &pa) != 0) {
                continue;
            }
            if (pa.state == IBV_PORT_ACTIVE) {
                active = true;
                break;
            }
        }
        ::ibv_close_device(ctx);
        if (!active) {
            continue;
        }
        ::ibv_free_device_list(list);
        out.usable = true;
        out.rxe = rxe;
        out.device = name;
        out.detail = rxe ? "active rxe (opt-in)" : "active RNIC";
        return out;
    }
    ::ibv_free_device_list(list);
    if (seen.empty()) {
        out.detail = "no ibv devices";
    } else if (!want.empty()) {
        out.detail = "no active usable RNIC matching rdma_device_name=" + want +
                     " (seen " + seen + ")";
    } else {
        out.detail = "no active hardware RNIC (seen " + seen + ")";
    }
    return out;
}
#else
rnic_probe probe_rnic(const boost::property_tree::ptree& /*conf*/, bool /*allow_rxe*/) noexcept {
    rnic_probe out;
    out.detail = "FASTBLOCK_WITH_RDMA=OFF";
    return out;
}
#endif

} // namespace msg
