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

#include "osd_client.h"

#include <spdk/accel.h>
#include <spdk/bdev.h>
#include <spdk/thread.h>

enum {
    SNAP_WRITE = 1,
    SNAP_READ
};

static const char* g_json_conf{nullptr};
static std::string g_object_name{"snapshot_pg_obj"};
static std::string g_payload{};
static uint32_t g_op_type = SNAP_WRITE;
static uint32_t g_write_count = 64;
static uint32_t g_done_count = 0;

static void usage()
{
    printf(" -C <osd json config file> path to json config file\n");
    printf(" -I <id>                   initial osd id\n");
    printf(" -o <osd_addr>             initial osd address\n");
    printf(" -t <osd_port>             initial osd port\n");
    printf(" -P <pool_id>              pool id\n");
    printf(" -G <pg_id>                pg id\n");
    printf(" Environment variables:\n");
    printf("   FB_PG_OBJECT            target object name, default snapshot_pg_obj\n");
    printf("   FB_PG_WRITES            number of writes in write mode, default 64\n");
    printf("   FB_PG_IO_SIZE           payload size in bytes, default 4096\n");
    printf("   FB_PG_MODE              write or read, default write\n");
}

static int parse_arg(int ch, char* arg)
{
    switch (ch) {
    case 'C':
        g_json_conf = arg;
        break;
    case 'I':
        global_osd_id = spdk_strtol(arg, 10);
        break;
    case 'o':
        g_osd_addr = arg;
        break;
    case 't':
        g_osd_port = spdk_strtol(arg, 10);
        break;
    case 'P':
        g_pool_id = spdk_strtoll(arg, 10);
        break;
    case 'G':
        g_pg_id = spdk_strtoll(arg, 10);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static void issue_next_write(osd_client* cli, server_t* server);

static void on_read_finished(int state, const std::string& data)
{
    if (state != err::E_SUCCESS) {
        SPDK_ERRLOG("read object failed: %s(%d)\n", err::string_status(state), state);
        std::raise(SIGINT);
        return;
    }

    if (data != g_payload) {
        SPDK_ERRLOG("read verify failed, size %lu expect %lu\n", data.size(), g_payload.size());
        std::raise(SIGINT);
        return;
    }

    SPDK_NOTICELOG("snapshot_pg_stress read verify success, object=%s size=%lu\n",
      g_object_name.c_str(), data.size());
}

static void on_write_finished(osd_client* cli, server_t* server, int state)
{
    if (state != err::E_SUCCESS) {
        SPDK_ERRLOG("write object failed: %s(%d)\n", err::string_status(state), state);
        std::raise(SIGINT);
        return;
    }

    g_done_count++;
    if (g_done_count % 10 == 0 || g_done_count == g_write_count) {
        SPDK_NOTICELOG("snapshot_pg_stress write progress %u/%u object=%s pg=%lu.%lu\n",
          g_done_count, g_write_count, g_object_name.c_str(), server->pool_id, server->pg_id);
    }

    if (g_done_count >= g_write_count) {
        SPDK_NOTICELOG("snapshot_pg_stress write finished object=%s pg=%lu.%lu\n",
          g_object_name.c_str(), server->pool_id, server->pg_id);
        return;
    }

    issue_next_write(cli, server);
}

static void issue_next_write(osd_client* cli, server_t* server)
{
    auto rc = cli->write_object(
      server->pool_id,
      server->pg_id,
      g_object_name,
      0,
      g_payload,
      [cli, server](int state) {
          on_write_finished(cli, server, state);
      });
    if (rc != err::E_SUCCESS) {
        SPDK_ERRLOG("write_object submit failed: %s(%d)\n", err::string_status(rc), rc);
        std::raise(SIGINT);
    }
}

static void snapshot_pg_stress(void* arg1)
{
    server_t* server = (server_t*)arg1;

    SPDK_NOTICELOG("snapshot_pg_stress start, cpu count : %u\n", spdk_env_get_core_count());
    core_sharded::construct(core_sharded::system::begin(), spdk_env_get_core_count(), "snapshot_pg_stress");
    auto core_no = spdk_env_get_current_core();
    spdk_cpuset cpumask{};
    spdk_cpuset_zero(&cpumask);
    spdk_cpuset_set_cpu(&cpumask, core_no, true);
    auto opts = msg::rdma::client::make_options(server->pt);

    auto* cli = new osd_client(server, &cpumask, opts);
    auto connect_done = [cli, server](void*, int res) {
        if (res != err::E_SUCCESS) {
            SPDK_ERRLOG("connect failed: %s(%d)\n", err::string_status(res), res);
            throw std::runtime_error{"connect failed"};
        }

        auto op = [cli, server]() {
            if (g_op_type == SNAP_WRITE) {
                issue_next_write(cli, server);
            } else {
                auto rc = cli->read_object(
                  server->pool_id,
                  server->pg_id,
                  g_object_name,
                  0,
                  g_payload.size(),
                  [](int state, const std::string& data) {
                      on_read_finished(state, data);
                  });
                if (rc != err::E_SUCCESS) {
                    SPDK_ERRLOG("read_object submit failed: %s(%d)\n", err::string_status(rc), rc);
                    std::raise(SIGINT);
                }
            }
        };
        cli->get_leader(server->node_id, server->pool_id, server->pg_id, op);
    };

    cli->create_connect(server->osd_addr, server->osd_port, server->node_id, std::move(connect_done));
}

int main(int argc, char* argv[])
{
    spdk_app_opts opts = {};
    server_t server = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "snapshot_pg_stress";
    opts.num_entries = 0;

    if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:I:o:t:P:G:", NULL,
                                  parse_arg, usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        return rc;
    }

    spdk_iobuf_opts iobuf_opts{};
    spdk_iobuf_get_opts(&iobuf_opts, sizeof(iobuf_opts));
    iobuf_opts.small_pool_count = 64;
    iobuf_opts.large_pool_count = 8;
    spdk_iobuf_set_opts(&iobuf_opts);

    spdk_bdev_opts bdev_opts{};
    spdk_bdev_get_opts(&bdev_opts, sizeof(bdev_opts));
    bdev_opts.iobuf_small_cache_size = 0;
    bdev_opts.iobuf_large_cache_size = 0;
    spdk_bdev_set_opts(&bdev_opts);

    spdk_accel_opts accel_opts{};
    spdk_accel_get_opts(&accel_opts, sizeof(accel_opts));
    accel_opts.small_cache_size = 0;
    accel_opts.large_cache_size = 0;
    spdk_accel_set_opts(&accel_opts);

    if (const char* obj = std::getenv("FB_PG_OBJECT")) {
        g_object_name = obj;
    }
    if (const char* writes = std::getenv("FB_PG_WRITES")) {
        g_write_count = spdk_strtol(writes, 10);
    }
    if (const char* size = std::getenv("FB_PG_IO_SIZE")) {
        g_payload.assign(spdk_strtol(size, 10), 0x5a);
    }
    if (const char* mode = std::getenv("FB_PG_MODE")) {
        if (!strcmp(mode, "write")) {
            g_op_type = SNAP_WRITE;
        } else if (!strcmp(mode, "read")) {
            g_op_type = SNAP_READ;
        } else {
            return -EINVAL;
        }
    }

    server.node_id = global_osd_id;
    server.osd_addr = g_osd_addr;
    server.osd_port = g_osd_port;
    server.pool_id = g_pool_id;
    server.pg_id = g_pg_id;

    if (g_payload.empty()) {
        g_payload.assign(4096, 0x5a);
    }

    boost::property_tree::read_json(std::string(g_json_conf), server.pt);
    return spdk_app_start(&opts, snapshot_pg_stress, &server);
}
