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

#define BOOST_TEST_MODULE Monitor Client

#include "monclient/client.h"
#include "osd/partition_manager.h"
#include "utils/utils.h"

#include <spdk/event.h>

#include <boost/format.hpp>
#include <boost/test/included/unit_test.hpp>

#include <memory>

namespace {

enum monitor_client_test_state {
    booted = 1,
    image_created,
    image_info_got,
    image_resized,
    image_resized_info_got,
    image_removed,
    pending,
    terminate
};

struct monitor_client_test_context {
    std::shared_ptr<::connect_cache> conn_cache{nullptr};
    std::shared_ptr<::partition_manager> pm{nullptr};
    std::unique_ptr<monitor::client> mon_cli{nullptr};
    std::string image_name{};
    int64_t image_size{};
    int64_t resize_size{};
    int64_t object_size{};
    monitor_client_test_state test_state{monitor_client_test_state::pending};
    bool is_terminated{false};
};

std::string monitor1_host{};
uint16_t monitor1_port{};
std::string monitor2_host{};
uint16_t monitor2_port{};
std::string monitor3_host{};
uint16_t monitor3_port{};
int osd_id{};
std::string osd_host{};
uint16_t osd_port{};
std::string osd_uuid{};
std::string pool{};

utils::simple_poller main_poller{};

static int parse_args() {
    auto argc = boost::unit_test::framework::master_test_suite().argc;
    BOOST_ASSERT(argc == 7 or argc == 11); // 8 is standalone mnitor, 12 is monitor cluster
    auto argv = boost::unit_test::framework::master_test_suite().argv;
    int argv_idx{1};

    monitor1_host = std::string(argv[argv_idx++]);
    std::string mon_port_str(argv[argv_idx++]);
    monitor1_port = static_cast<decltype(monitor1_port)>(std::stoul(mon_port_str));

    if (argc == 12) {
        monitor2_host = std::string(argv[argv_idx++]);
        std::string mon2_port_str(argv[argv_idx++]);
        monitor2_port = static_cast<decltype(monitor2_port)>(std::stoul(mon2_port_str));

        monitor3_host = std::string(argv[argv_idx++]);
        std::string mon3_port_str(argv[argv_idx++]);
        monitor3_port = static_cast<decltype(monitor3_port)>(std::stoul(mon3_port_str));
    }

    std::string osd_id_str(argv[argv_idx++]);
    osd_id = static_cast<decltype(osd_id)>(std::stoul(osd_id_str));
    osd_host = std::string(argv[argv_idx++]);
    std::string osd_port_str(argv[argv_idx++]);
    osd_port = static_cast<decltype(osd_port)>(std::stoul(osd_port_str));
    osd_uuid = std::string(argv[argv_idx++]);

    pool = std::string(argv[argv_idx++]);

    return argc;
}

int main_poller_cb(void* arg) {
    auto* ctx = reinterpret_cast<monitor_client_test_context*>(arg);
    switch (ctx->test_state) {
    case monitor_client_test_state::booted: {
        BOOST_TEST_MESSAGE("enter monitor_client_test_state::booted");
        ctx->mon_cli->emplace_create_image_request(
          pool, ctx->image_name, ctx->image_size,
          ctx->object_size, [ctx] (auto status, [[maybe_unused]] auto _) {
              BOOST_TEST(status == monitor::client::response_status::ok);
              ctx->test_state = monitor_client_test_state::image_created;
          }
        );
        ctx->test_state = monitor_client_test_state::pending;
        break;
    }
    case monitor_client_test_state::image_created: {
        BOOST_TEST_MESSAGE("enter monitor_client_test_state::image_created");
        ctx->mon_cli->emplace_get_image_info_request(
          pool, ctx->image_name, [ctx] (const monitor::client::response_status s, monitor::client::request_context* req_ctx) {
              BOOST_TEST_MESSAGE("test on image info got");
              BOOST_TEST(s == monitor::client::response_status::ok);
              auto& img_info = std::get<std::unique_ptr<monitor::client::image_info>>(req_ctx->response_data);
              BOOST_TEST(ctx->image_name == img_info->image_name);
              BOOST_TEST(ctx->image_size == img_info->size);
              BOOST_TEST(ctx->object_size == img_info->object_size);
              BOOST_TEST(pool == img_info->pool_name);
              ctx->test_state = monitor_client_test_state::image_info_got;
          }
        );
        ctx->test_state = monitor_client_test_state::pending;
        break;
    case monitor_client_test_state::image_info_got: {
        BOOST_TEST_MESSAGE("enter monitor_client_test_state::image_info_got");
        ctx->mon_cli->emplace_resize_image_request(
          pool, ctx->image_name, ctx->resize_size,
          [ctx] (auto s, [[maybe_unused]] auto _) {
              BOOST_TEST_MESSAGE("test on image info resized");
              BOOST_TEST(s == monitor::client::response_status::ok);
              ctx->test_state = monitor_client_test_state::image_resized;
          }
        );
        ctx->test_state = monitor_client_test_state::pending;
        break;
    }
    case monitor_client_test_state::image_resized: {
        BOOST_TEST_MESSAGE("enter monitor_client_test_state::image_resized");
        ctx->mon_cli->emplace_get_image_info_request(
          pool, ctx->image_name, [ctx] (const monitor::client::response_status s, monitor::client::request_context* req_ctx) {
              BOOST_TEST_MESSAGE("test on image info got");
              BOOST_TEST(s == monitor::client::response_status::ok);
              auto& img_info = std::get<std::unique_ptr<monitor::client::image_info>>(req_ctx->response_data);
              BOOST_TEST(ctx->image_name == img_info->image_name);
              BOOST_TEST(ctx->resize_size == img_info->size);
              BOOST_TEST(ctx->object_size == img_info->object_size);
              BOOST_TEST(pool == img_info->pool_name);
              ctx->test_state = monitor_client_test_state::image_resized_info_got;
          }
        );
        ctx->test_state = monitor_client_test_state::pending;
        break;
    }
    case monitor_client_test_state::image_resized_info_got: {
        BOOST_TEST_MESSAGE("enter monitor_client_test_state::image_resized_info_got");
        ctx->mon_cli->emplace_remove_image_request(
          pool, ctx->image_name, [ctx] (auto s, [[maybe_unused]] auto _) {
              BOOST_TEST_MESSAGE("test on image removed");
              BOOST_TEST(s == monitor::client::response_status::ok);
              ctx->test_state = monitor_client_test_state::image_removed;
          }
        );
        ctx->test_state = monitor_client_test_state::pending;
        break;
    }
    case monitor_client_test_state::image_removed: {
        BOOST_TEST_MESSAGE("enter monitor_client_test_state::image_removed");
        ctx->mon_cli->emplace_get_image_info_request(
          pool, ctx->image_name, [ctx] (const monitor::client::response_status s, [[maybe_unused]] auto _) {
              BOOST_TEST_MESSAGE("test on image info got");
              BOOST_TEST(s == monitor::client::response_status::image_not_found);
              ctx->test_state = monitor_client_test_state::terminate;
          }
        );
        ctx->test_state = monitor_client_test_state::pending;
        break;
    }
    case monitor_client_test_state::pending: {
        break;
    }
    case monitor_client_test_state::terminate: {
        if (ctx->is_terminated) {
            break;
        }
        BOOST_TEST_MESSAGE("enter monitor_client_test_state::terminate");
        ::spdk_app_stop(0);
        ctx->is_terminated = true;
        break;
    }
    default:
        break;
    }
    }

    return SPDK_POLLER_BUSY;
}

void monitor_client_test_on_app_start(void* arg) {
    auto* ctx = reinterpret_cast<monitor_client_test_context*>(arg);
    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
    auto opts = std::make_shared<msg::rdma::client::options>();
    opts->ep = std::make_unique<msg::rdma::endpoint>();
    ctx->conn_cache = std::make_shared<::connect_cache>(&cpumask, opts);
    ctx->pm = std::make_shared<::partition_manager>(osd_id, ctx->conn_cache);
    std::vector<monitor::client::endpoint> eps{};
    eps.emplace_back(monitor1_host, monitor1_port);

    if (not monitor2_host.empty()) {
        eps.emplace_back(monitor2_host, monitor2_port);
        eps.emplace_back(monitor3_host, monitor3_port);
    }

    monitor::client::on_new_pg_callback_type pg_map_cb =
      [pm = ctx->pm] (const msg::PGInfo& pg_info, const int32_t pool_id, const monitor::client::osd_map& osd_map,
        monitor::client::pg_op_complete&& cb_fn, void *arg) {
          BOOST_TEST_MESSAGE("call pg_map_cb()");

          std::vector<utils::osd_info_t> osds{};
          for (auto osd_id : pg_info.osdid()) {
              osds.push_back(*(osd_map.data.at(osd_id)));
          }
          pm->create_partition(pool_id, pg_info.pgid(), std::move(osds), 0, std::move(cb_fn), arg);
      };

    ctx->mon_cli = std::make_unique<monitor::client>(eps, ctx->pm, std::move(pg_map_cb));
    ctx->mon_cli->start();

    if (osd_id == -1) {
        ctx->test_state = monitor_client_test_state::booted;
        ctx->mon_cli->start_cluster_map_poller();
    } else {
        ctx->mon_cli->emplace_osd_boot_request(
          osd_id, osd_host, osd_port, osd_uuid, 1024 * 1024,
          [app_ctx = ctx](const monitor::client::response_status s, monitor::client::request_context *ctx)
          {
              BOOST_ASSERT(s == monitor::client::response_status::ok);
              BOOST_TEST_MESSAGE("OSD booted");
              app_ctx->test_state = monitor_client_test_state::booted;
              ctx->this_client->start_cluster_map_poller();
          });
    }


    main_poller.register_poller(main_poller_cb, ctx, 0);
}
}

BOOST_AUTO_TEST_CASE(monitor_client_test) {
    auto argc = parse_args();

    if (argc == 8) {
        BOOST_TEST_MESSAGE(boost::format(
          "monitor server is %1%:%2%, osd is %3%:%4%, osd id is %5%, osd uuid is %6%")
          % monitor1_host % monitor1_port % osd_host % osd_port % osd_id % osd_uuid);
    } else {
        BOOST_TEST_MESSAGE(boost::format(
          "monitor server is [%1%:%2%, %3%:%4%, %5%:%6%], osd is %7%:%8%, osd id is %9%, osd uuid is %10%")
          % monitor1_host % monitor1_port % monitor2_host % monitor2_port
          % monitor3_host % monitor3_port % osd_host % osd_port % osd_id % osd_uuid);
    }

    ::spdk_app_opts opts{};
    ::spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "monitor client test";
    opts.shutdown_cb = nullptr;
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;
    opts.rpc_addr = "/var/tmp/fb_mon_cli_test.sock";
    ::spdk_log_set_flag("mon");

    monitor_client_test_context ctx{};

    const size_t image_name_size = utils::random_int(7, 17);
    ctx.image_name = utils::random_string(image_name_size);
    ctx.image_size = utils::random_int<decltype(ctx.image_size)>(4096 * 3, 4096 * 7);
    ctx.object_size = utils::random_int<decltype(ctx.object_size)>(4096, 4096 * 3);
    ctx.resize_size = utils::random_int<decltype(ctx.resize_size)>(ctx.image_size, ctx.image_size * 3);
    BOOST_TEST_MESSAGE(boost::format(
      "image name is %1%, image size is %2%, object size is %3%, resize size is %4%")
      % ctx.image_name % ctx.image_size % ctx.object_size % ctx.resize_size);
    auto rc = ::spdk_app_start(&opts, monitor_client_test_on_app_start, &ctx);
    ::spdk_app_fini();
}
