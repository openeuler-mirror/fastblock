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
#include <limits>
#include <map>
#include <optional>
#include <string>

/**
 * Pure helpers for PG home-shard sticky routing (RTC PR-1).
 * No SPDK dependency — unit-testable without the full stack.
 *
 * Rules:
 * - Data-path ports must come from sticky home_shard, never client_shard % n.
 * - get_leader probe uses a conservative fixed shard (prefer 0).
 */
namespace fb_client_route {

inline constexpr uint32_t k_invalid_home_shard = std::numeric_limits<uint32_t>::max();

/** shard_id -> RDMA listen port */
using shard_port_map = std::map<uint32_t, uint32_t>;

inline bool is_valid_home_shard(const uint32_t home_shard) noexcept {
    return home_shard != k_invalid_home_shard;
}

/** Reverse-lookup which shard owns a listen port. */
inline std::optional<uint32_t> reverse_lookup_home_shard(
  const shard_port_map& ports,
  const int32_t port) {
    for (const auto& [shard_id, p] : ports) {
        if (static_cast<int32_t>(p) == port) {
            return shard_id;
        }
    }
    return std::nullopt;
}

/** Resolve current listen port for sticky home_shard. */
inline std::optional<int32_t> port_for_home_shard(
  const shard_port_map& ports,
  const uint32_t home_shard) {
    if (!is_valid_home_shard(home_shard)) {
        return std::nullopt;
    }
    const auto it = ports.find(home_shard);
    if (it == ports.end()) {
        return std::nullopt;
    }
    return static_cast<int32_t>(it->second);
}

/**
 * Conservative get_leader probe port (PR-1 until PR-3):
 * prefer shard 0; otherwise the lowest shard id present.
 * Never uses client-side CPU/shard index.
 */
inline std::optional<int32_t> probe_port_for_get_leader(const shard_port_map& ports) {
    if (ports.empty()) {
        return std::nullopt;
    }
    const auto it0 = ports.find(0);
    if (it0 != ports.end()) {
        return static_cast<int32_t>(it0->second);
    }
    return static_cast<int32_t>(ports.begin()->second);
}

inline std::optional<uint32_t> probe_shard_for_get_leader(const shard_port_map& ports) {
    if (ports.empty()) {
        return std::nullopt;
    }
    if (ports.find(0) != ports.end()) {
        return 0u;
    }
    return ports.begin()->first;
}

struct refresh_transport_result {
    bool changed{false};
    /** Caller should mark leader cache invalid and re-resolve. */
    bool invalidate{false};
    std::string new_addr{};
    int32_t new_port{0};
};

/**
 * Compute refresh of leader addr/port from cluster map using sticky home_shard only.
 * Never selects port via client shard modulo.
 */
inline refresh_transport_result compute_refresh_transport(
  const uint32_t home_shard,
  const int32_t old_port,
  const std::string& old_addr,
  const bool mon_up_and_in,
  const std::string& mon_addr,
  const shard_port_map& mon_ports) {
    refresh_transport_result r{};
    if (!mon_up_and_in || mon_ports.empty() || !is_valid_home_shard(home_shard)) {
        r.invalidate = true;
        return r;
    }

    const auto port = port_for_home_shard(mon_ports, home_shard);
    if (!port.has_value()) {
        r.invalidate = true;
        return r;
    }

    r.new_addr = mon_addr;
    r.new_port = *port;
    r.changed = (old_addr != mon_addr) || (old_port != *port);
    // Same osd id + new listen port is a new process (replacement boot).
    // Caller must drop cached leader identity and re-query get_leader.
    return r;
}

/**
 * Seed sticky home_shard after get_leader response.
 * Priority for PR-1: reverse-lookup of leader_port in current map.
 * (mon coreindex / proto home_shard field come in later PRs.)
 */
inline uint32_t seed_home_shard_from_leader_port(
  const shard_port_map& ports,
  const int32_t leader_port,
  const uint32_t existing_home_shard = k_invalid_home_shard) {
    if (const auto found = reverse_lookup_home_shard(ports, leader_port)) {
        return *found;
    }
    // Keep sticky if still resolvable on the new map (OSD port churn case).
    if (is_valid_home_shard(existing_home_shard) &&
        port_for_home_shard(ports, existing_home_shard).has_value()) {
        return existing_home_shard;
    }
    return k_invalid_home_shard;
}

template <typename CoreShardMap>
inline shard_port_map to_shard_port_map(
  const std::map<uint32_t, CoreShardMap>& sharded_ports) {
    shard_port_map out;
    for (const auto& [shard_id, core_port] : sharded_ports) {
        out.emplace(shard_id, core_port.port);
    }
    return out;
}

} // namespace fb_client_route
