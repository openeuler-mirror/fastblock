/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#pragma once

#include <boost/property_tree/ptree.hpp>

#include <string>

namespace msg {

struct rnic_probe {
    bool usable{false};
    bool rxe{false};
    std::string device;
    std::string detail;
};

bool env_allow_rxe() noexcept;
rnic_probe probe_rnic(const boost::property_tree::ptree& conf, bool allow_rxe) noexcept;

} // namespace msg
