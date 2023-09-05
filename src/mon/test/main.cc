#define BOOST_TEST_MODULE Monitor Client

#include "mon/client.h"
#include "osd/partition_manager.h"
#include "utils/utils.h"

#include <spdk/event.h>

#include <fmt/core.h>

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
std::unique_ptr<monitor::client::request_context> boot_ctx{nullptr};
std::unique_ptr<monitor::client::request_context> create_image_ctx{nullptr};
std::unique_ptr<monitor::client::request_context> get_image_ctx{nullptr};
std::unique_ptr<monitor::client::request_context> resize_image_ctx{nullptr};
std::unique_ptr<monitor::client::request_context> remove_image_ctx{nullptr};

static int parse_args() {
    auto argc = boost::unit_test::framework::master_test_suite().argc;
    BOOST_ASSERT(argc == 8 or argc == 12); // 8 is standalone mnitor, 12 is monitor cluster
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
        create_image_ctx = ctx->mon_cli->emplace_create_image_request(
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
        get_image_ctx = ctx->mon_cli->emplace_get_image_info_request(
          pool, ctx->image_name, [ctx] (const monitor::client::response_status s, [[maybe_unused]] auto _) {
              BOOST_TEST_MESSAGE("test on image info got");
              BOOST_TEST(s == monitor::client::response_status::ok);
              BOOST_TEST(ctx->image_name == get_image_ctx->img_info->image_name);
              BOOST_TEST(ctx->image_size == get_image_ctx->img_info->size);
              BOOST_TEST(ctx->object_size == get_image_ctx->img_info->object_size);
              BOOST_TEST(pool == get_image_ctx->img_info->pool_name);
              ctx->test_state = monitor_client_test_state::image_info_got;
          }
        );
        ctx->test_state = monitor_client_test_state::pending;
        break;
    case monitor_client_test_state::image_info_got: {
        BOOST_TEST_MESSAGE("enter monitor_client_test_state::image_info_got");
        resize_image_ctx = ctx->mon_cli->emplace_resize_image_request(
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
        get_image_ctx = ctx->mon_cli->emplace_get_image_info_request(
          pool, ctx->image_name, [ctx] (const monitor::client::response_status s, [[maybe_unused]] auto _) {
              BOOST_TEST_MESSAGE("test on image info got");
              BOOST_TEST(s == monitor::client::response_status::ok);
              BOOST_TEST(ctx->image_name == get_image_ctx->img_info->image_name);
              BOOST_TEST(ctx->resize_size == get_image_ctx->img_info->size);
              BOOST_TEST(ctx->object_size == get_image_ctx->img_info->object_size);
              BOOST_TEST(pool == get_image_ctx->img_info->pool_name);
              ctx->test_state = monitor_client_test_state::image_resized_info_got;
          }
        );
        ctx->test_state = monitor_client_test_state::pending;
        break;
    }
    case monitor_client_test_state::image_resized_info_got: {
        BOOST_TEST_MESSAGE("enter monitor_client_test_state::image_resized_info_got");
        remove_image_ctx = ctx->mon_cli->emplace_remove_image_request(
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
        get_image_ctx = ctx->mon_cli->emplace_get_image_info_request(
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
    ctx->pm = std::make_shared<::partition_manager>(
      osd_id, osd_host, osd_port, osd_uuid);
    std::vector<monitor::client::endpoint> eps{};
    eps.emplace_back(monitor1_host, monitor1_port);

    if (not monitor2_host.empty()) {
        eps.emplace_back(monitor2_host, monitor2_port);
        eps.emplace_back(monitor3_host, monitor3_port);
    }

    ctx->mon_cli = std::make_unique<monitor::client>(
      eps,
      ctx->pm);
    ctx->mon_cli->start();

    if (osd_id == -1) {
        ctx->test_state = monitor_client_test_state::booted;
        ctx->mon_cli->start_cluster_map_poller();
    } else {
        boot_ctx = ctx->mon_cli->emplace_osd_boot_request(
          osd_id, osd_host, osd_uuid, 1024 * 1024,
          [app_ctx = ctx] (const monitor::client::response_status s, monitor::client::request_context* ctx) {
              BOOST_ASSERT(s == monitor::client::response_status::ok);
              BOOST_TEST_MESSAGE("OSD booted");

              app_ctx->test_state = monitor_client_test_state::booted;
              ctx->this_client->start_cluster_map_poller();
          }
    );
    }


    main_poller.poller = SPDK_POLLER_REGISTER(main_poller_cb, ctx, 0);
}
}

BOOST_AUTO_TEST_CASE(monitor_client_test) {
    auto argc = parse_args();

    if (argc == 8) {
        BOOST_TEST_MESSAGE(fmt::format(
          "monitor server is {}:{}, osd is {}:{}, osd id is {}, osd uuid is {}",
          monitor1_host, monitor1_port,
          osd_host, osd_port, osd_id, osd_uuid));
    } else {
        BOOST_TEST_MESSAGE(fmt::format(
          "monitor server is [{}:{}, {}:{}, {}:{}], osd is {}:{}, osd id is {}, osd uuid is {}",
          monitor1_host, monitor1_port, monitor2_host, monitor2_port,
          monitor3_host, monitor3_port, osd_host, osd_port, osd_id, osd_uuid));
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
    BOOST_TEST_MESSAGE(fmt::format(
      "image name is {}, image size is {}, object size is {}, resize size is {}",
      ctx.image_name, ctx.image_size, ctx.object_size, ctx.resize_size));
    auto rc = ::spdk_app_start(&opts, monitor_client_test_on_app_start, &ctx);
    ::spdk_app_fini();
}
