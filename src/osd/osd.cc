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
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "monclient/client.h"
#include "raft/raft.h"
#include "osd/partition_manager.h"
#include "raft/raft_service.h"
#include "osd/osd_service.h"
#include "rpc/server.h"
#include "localstore/blob_manager.h"
#include "localstore/storage_manager.h"

#include <spdk/string.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

static const char* g_json_conf{nullptr};
static std::unique_ptr<::raft_service<::partition_manager>> global_raft_service{nullptr};
static std::unique_ptr<::osd_service> global_osd_service{nullptr};
static std::shared_ptr<::connect_cache> global_conn_cache{nullptr};
static std::shared_ptr<::partition_manager> global_pm{nullptr};
static std::shared_ptr<monitor::client> monitor_client{nullptr};

typedef struct
{
    /* the server's node ID */
    int node_id;
	std::string bdev_disk;
	std::string osd_addr;
	int osd_port;
	std::string osd_uuid;
    std::vector<monitor::client::endpoint> monitors{};
    std::unique_ptr<rpc_server> rpc_srv{nullptr};
    boost::property_tree::ptree pt{};
} server_t;

static void
block_usage(void)
{
	printf(" -C <osd json config file> path to json config file\n");
}

static void
save_pid(const char *pid_path)
{
	FILE *pid_file;

	pid_file = fopen(pid_path, "w");
	if (pid_file == NULL) {
		fprintf(stderr, "Couldn't create pid file '%s': %s\n", pid_path, strerror(errno));
		exit(EXIT_FAILURE);
	}

	fprintf(pid_file, "%d\n", getpid());
	fclose(pid_file);
}

static int
block_parse_arg(int ch, char *arg)
{
	switch (ch) {
    case 'C':
        g_json_conf = arg;
        break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void service_init(partition_manager* pm, server_t *server){
	global_raft_service = std::make_unique<::raft_service<::partition_manager>>(global_pm.get());
    global_osd_service = std::make_unique<::osd_service>(global_pm.get(), monitor_client);

    // FIXME: options from json configuration file
    auto srv_opts = msg::rdma::server::make_options(
      server->pt.get_child("msg").get_child("server"),
      server->pt.get_child("msg").get_child("rdma"));
    srv_opts->bind_address = server->osd_addr;
    srv_opts->port = server->osd_port;
    server->rpc_srv = std::make_unique<rpc_server>(
      ::spdk_env_get_current_core(),
      srv_opts);
	server->rpc_srv->register_service(global_raft_service.get());
	server->rpc_srv->register_service(global_osd_service.get());
}

struct pm_start_context : public context{
    server_t *server;
    partition_manager* pm;

    pm_start_context(server_t *_server, partition_manager* _pm)
    : server(_server)
    , pm(_pm) {}

    void finish(int r) override {
		if(r != 0){
            spdk_app_stop(r);
			return;
		}
		service_init(pm, server);
    }
};

void start_monitor(server_t* ctx) {
    monitor_client->start();
    monitor::client::on_response_callback_type cb =
      [] (const monitor::client::response_status s, monitor::client::request_context* req_ctx) {
          if (s != monitor::client::response_status::ok) {
              SPDK_ERRLOG("ERROR: OSD boot failed\n");
              throw std::runtime_error{"OSD boot failed"};
          }

          req_ctx->this_client->start_cluster_map_poller();
      };

	monitor_client->emplace_osd_boot_request(
		ctx->node_id, ctx->osd_addr, ctx->osd_port, ctx->osd_uuid,
		1024 * 1024, std::move(cb));
}

void storage_init_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_ERRLOG("Failed to initialize the storage system: \n", spdk_strerror(rberrno));
		spdk_app_stop(rberrno);
		return;
	}
	server_t *server = (server_t *)arg;
    SPDK_INFOLOG(osd, "------block start, cpu count : %u  bdev_disk: %s\n",
	        spdk_env_get_core_count(), server->bdev_disk.c_str());

    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
    auto opts = msg::rdma::client::make_options(
      server->pt.get_child("msg").get_child("client"),
      server->pt.get_child("msg").get_child("rdma"));
    global_conn_cache = std::make_shared<::connect_cache>(&cpumask, opts);
    global_pm = std::make_shared<partition_manager>(server->node_id, global_conn_cache);
    monitor::client::on_new_pg_callback_type pg_map_cb =
      [pm = global_pm] (const msg::PGInfo& pg_info, const int32_t pg_key, const int32_t pg_map_ver, const monitor::client::osd_map& osd_map) {
          SPDK_DEBUGLOG(osd, "enter pg_map_cb()\n");
          std::vector<::osd_info_t> osds{};
          for (auto osd_id : pg_info.osdid()) {
              osds.push_back(*(osd_map.data.at(osd_id)));
          }
          pm->create_partition(pg_key, pg_info.pgid(), std::move(osds), pg_map_ver);
      };
    monitor_client = std::make_shared<monitor::client>(
      server->monitors, global_pm, std::move(pg_map_cb), std::nullopt, server->node_id);

    start_monitor(server);

    pm_start_context *ctx = new pm_start_context{server, global_pm.get()};
	global_pm->start(ctx);
}

void disk_init_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to initialize the disk. %s\n", spdk_strerror(rberrno));
		spdk_app_stop(rberrno);
		return;
	}

	storage_init(storage_init_complete, arg);
}

static void
block_started(void *arg)
{
    server_t *server = (server_t *)arg;

    buffer_pool_init();
      //初始化log磁盘
    blobstore_init(server->bdev_disk.c_str(), disk_init_complete, arg);
}

static void from_configuration(server_t& server) {
    auto& pt = server.pt;
    auto current_osd_id = pt.get_child("current_osd_id").get_value<decltype(server.node_id)>();
    auto& osds = pt.get_child("osds");
    for (auto& osd : osds) {
        auto osd_id = osd.second.get_child("osd_id").get_value<decltype(server.node_id)>();
        if (osd_id != current_osd_id) {
            continue;
        }

        auto& osd_pt = osd.second;
        auto pid_path = osd_pt.get_child("pid_path").get_value<std::string>();
        if (not pid_path.empty()) {
            save_pid(pid_path.c_str());
        }

        server.node_id = osd_id;
        server.bdev_disk = osd_pt.get_child("bdev_disk").get_value<std::string>();
        if (server.bdev_disk.empty()) {
            std::cerr << "No bdev name is specified" << std::endl;
            throw std::invalid_argument{"empty bdev disk path"};
        }

        server.osd_addr = osd_pt.get_child("address").get_value<std::string>();
        server.osd_port = osd_pt.get_child("port").get_value<decltype(server.osd_port)>();
        server.osd_uuid = osd_pt.get_child("uuid").get_value<std::string>();

        auto& monitors = osd_pt.get_child("monitor");
        for (auto& monitor_node : monitors) {
            auto mon_host = monitor_node.second.get_child("host").get_value<std::string>();
            auto mon_port = monitor_node.second.get_child("port").get_value<uint16_t>();
            server.monitors.emplace_back(std::move(mon_host), mon_port);
        }
    }
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	server_t server = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "block";

	// disable tracing because it's memory consuming
	opts.num_entries = 0;
	opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:f:I:D:H:P:o:t:U:", NULL,
				      block_parse_arg, block_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

    SPDK_INFOLOG(osd, "Osd config file is %s\n", g_json_conf);
    boost::property_tree::read_json(std::string(g_json_conf), server.pt);
    from_configuration(server);

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, block_started, &server);

	spdk_app_fini();
	buffer_pool_fini();

	return rc;
}
