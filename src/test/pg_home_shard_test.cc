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

/**
 * Regression tests for RTC PR-1 sticky home_shard routing helpers.
 *
 * Covers the bug where refresh used client_shard % n and overwrote the
 * correct PG home port from get_leader.
 *
 * No SPDK / RDMA required — pure logic tests.
 */

#include "fastblock/client/pg_home_shard.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define EXPECT_EQ(a, b)                                                        \
    do {                                                                       \
        const auto _a = (a);                                                   \
        const auto _b = (b);                                                   \
        if (!(_a == _b)) {                                                     \
            std::fprintf(                                                      \
              stderr,                                                          \
              "FAIL %s:%d: %s == %s  (got %lld vs %lld)\n",                    \
              __FILE__,                                                        \
              __LINE__,                                                        \
              #a,                                                              \
              #b,                                                              \
              static_cast<long long>(_a),                                      \
              static_cast<long long>(_b));                                     \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

fb_client_route::shard_port_map sample_ports() {
    // shard 0 -> 9100, shard 1 -> 9101, shard 2 -> 9102
    return {{0, 9100}, {1, 9101}, {2, 9102}};
}

void test_reverse_lookup_home_shard() {
    const auto ports = sample_ports();
    EXPECT_EQ(*fb_client_route::reverse_lookup_home_shard(ports, 9101), 1u);
    EXPECT_EQ(*fb_client_route::reverse_lookup_home_shard(ports, 9100), 0u);
    EXPECT_TRUE(!fb_client_route::reverse_lookup_home_shard(ports, 9999).has_value());
}

void test_port_for_home_shard() {
    const auto ports = sample_ports();
    EXPECT_EQ(*fb_client_route::port_for_home_shard(ports, 2), 9102);
    EXPECT_TRUE(!fb_client_route::port_for_home_shard(ports, 9).has_value());
    EXPECT_TRUE(
      !fb_client_route::port_for_home_shard(
         ports, fb_client_route::k_invalid_home_shard)
         .has_value());
}

void test_probe_prefers_shard_zero_not_client_modulo() {
    // Even if "client shard" would be 1 or 2, probe must prefer shard 0.
    const auto ports = sample_ports();
    EXPECT_EQ(*fb_client_route::probe_port_for_get_leader(ports), 9100);
    EXPECT_EQ(*fb_client_route::probe_shard_for_get_leader(ports), 0u);

    // No shard 0: use lowest key
    fb_client_route::shard_port_map no_zero{{1, 9201}, {3, 9203}};
    EXPECT_EQ(*fb_client_route::probe_port_for_get_leader(no_zero), 9201);
    EXPECT_EQ(*fb_client_route::probe_shard_for_get_leader(no_zero), 1u);

    EXPECT_TRUE(!fb_client_route::probe_port_for_get_leader({}).has_value());
}

void test_refresh_keeps_sticky_home_across_map_update() {
    // Regression: old code used client_shard % n and could flip 9101 -> 9100
    // when "client" was on shard 0 after map refresh.
    const uint32_t home_shard = 1;
    const int32_t old_port = 9101;
    const std::string old_addr = "10.0.0.1";

    // Map still has home shard 1; only addresses/ports for other shards change.
    fb_client_route::shard_port_map new_ports{{0, 8100}, {1, 9101}, {2, 8102}};
    const auto r = fb_client_route::compute_refresh_transport(
      home_shard,
      old_port,
      old_addr,
      true,
      "10.0.0.1",
      new_ports);

    EXPECT_TRUE(!r.invalidate);
    EXPECT_TRUE(!r.changed);
    EXPECT_EQ(r.new_port, 9101);
}

void test_refresh_updates_port_when_osd_rebinds_same_home() {
    // OSD restart: same home_shard, new listen port.
    const uint32_t home_shard = 1;
    fb_client_route::shard_port_map new_ports{{0, 9100}, {1, 19101}, {2, 9102}};
    const auto r = fb_client_route::compute_refresh_transport(
      home_shard,
      9101,
      "10.0.0.1",
      true,
      "10.0.0.1",
      new_ports);

    EXPECT_TRUE(!r.invalidate);
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(r.new_port, 19101);
    EXPECT_TRUE(r.new_addr == "10.0.0.1");
}

void test_refresh_never_uses_client_shard_modulo() {
    // Simulate the old bug input: client on shard 0 would have selected 9100.
    // Sticky home is 2 → must stay on 9102 after refresh.
    const uint32_t home_shard = 2;
    // "client_shard" would be 0; if someone took modulo they'd pick ports[0].
    const auto ports = sample_ports();
    const auto r = fb_client_route::compute_refresh_transport(
      home_shard,
      9102,
      "192.168.1.10",
      true,
      "192.168.1.10",
      ports);

    EXPECT_TRUE(!r.invalidate);
    EXPECT_EQ(r.new_port, 9102);
    EXPECT_TRUE(r.new_port != 9100);
}

void test_refresh_invalidates_without_home_or_missing_port() {
    auto r = fb_client_route::compute_refresh_transport(
      fb_client_route::k_invalid_home_shard,
      9101,
      "10.0.0.1",
      true,
      "10.0.0.1",
      sample_ports());
    EXPECT_TRUE(r.invalidate);

    r = fb_client_route::compute_refresh_transport(
      1,
      9101,
      "10.0.0.1",
      false,
      "10.0.0.1",
      sample_ports());
    EXPECT_TRUE(r.invalidate);

    // home_shard 1 missing from new map
    r = fb_client_route::compute_refresh_transport(
      1,
      9101,
      "10.0.0.1",
      true,
      "10.0.0.1",
      fb_client_route::shard_port_map{{0, 9100}, {2, 9102}});
    EXPECT_TRUE(r.invalidate);
}

void test_seed_home_shard_from_leader_port() {
    const auto ports = sample_ports();
    EXPECT_EQ(
      fb_client_route::seed_home_shard_from_leader_port(ports, 9102),
      2u);

    // Unknown port, no sticky → invalid
    EXPECT_EQ(
      fb_client_route::seed_home_shard_from_leader_port(ports, 5555),
      fb_client_route::k_invalid_home_shard);

    // Unknown port but sticky still on map → keep sticky (OSD port churn)
    EXPECT_EQ(
      fb_client_route::seed_home_shard_from_leader_port(ports, 5555, 1u),
      1u);

    // Unknown port and sticky gone from map → invalid
    fb_client_route::shard_port_map shrunk{{0, 9100}};
    EXPECT_EQ(
      fb_client_route::seed_home_shard_from_leader_port(shrunk, 5555, 1u),
      fb_client_route::k_invalid_home_shard);
}

void test_multi_refresh_stability_across_fake_client_shards() {
    // Multiple successive refreshes must not depend on any "client shard" input
    // (API has no client shard parameter — that's the point).
    const uint32_t home = 1;
    auto ports = sample_ports();
    int32_t port = 9101;
    std::string addr = "10.0.0.5";

    for (int i = 0; i < 5; ++i) {
        // Mutate non-home ports to simulate map churn / other shards rebinding.
        ports[0] = static_cast<uint32_t>(9000 + i);
        ports[2] = static_cast<uint32_t>(9300 + i);
        const auto r = fb_client_route::compute_refresh_transport(
          home, port, addr, true, addr, ports);
        EXPECT_TRUE(!r.invalidate);
        EXPECT_EQ(r.new_port, 9101);
        port = r.new_port;
    }
}

} // namespace

int main() {
    test_reverse_lookup_home_shard();
    test_port_for_home_shard();
    test_probe_prefers_shard_zero_not_client_modulo();
    test_refresh_keeps_sticky_home_across_map_update();
    test_refresh_updates_port_when_osd_rebinds_same_home();
    test_refresh_never_uses_client_shard_modulo();
    test_refresh_invalidates_without_home_or_missing_port();
    test_seed_home_shard_from_leader_port();
    test_multi_refresh_stability_across_fake_client_shards();

    if (g_failures != 0) {
        std::fprintf(stderr, "pg_home_shard_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("pg_home_shard_test: all passed\n");
    return 0;
}
