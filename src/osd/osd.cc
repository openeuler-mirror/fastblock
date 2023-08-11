#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "raft/raft.h"
#include "osd/partition_manager.h"
#include "raft/raft_service.h"
#include "osd/osd_service.h"
#include "rpc/server.h"
#include "localstore/blob_manager.h"

static const char *g_pid_path = nullptr;
static int global_osd_id = 0;
static const char * g_bdev_disk = nullptr;
static partition_manager* global_pm = nullptr;
static const char* g_osd_addr = "127.0.0.1";
static  int g_osd_port = 8888;

// use default uuid
static const char *g_uuid = "00000000-0000-0000-0000-000000000000";

static const char *g_mon_addr = "127.0.0.1";
static int g_mon_port = 3333;

typedef struct
{
    /* the server's node ID */
    int node_id;
	std::string bdev_disk;
	std::string mon_addr;
	int mon_port;
	std::string osd_addr;
	int osd_port;
	std::string osd_uuid;
}server_t;

static void
block_usage(void)
{
	printf(" -f <path>                 save pid to file under given path\n");
	printf(" -I <id>                   save osd id\n");
	printf(" -D <bdev_disk>            bdev disk\n");
    printf(" -H <host_addr>            monitor host address\n");
	printf(" -P <port>                 monitor port number\n");
    printf(" -o <osd_addr>             osd address\n");
	printf(" -t <osd_port>             osd port\n");
	printf(" -U <osd uuid>             osd uuid\n");
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
	case 'f':
		g_pid_path = arg;
		break;
	case 'I':
	    global_osd_id = spdk_strtol(arg, 10);
		break;
	case 'D':
	    g_bdev_disk = arg;
		break;	
	case 'H':
	    g_mon_addr = arg;
		break;
	case 'P':
	    g_mon_port = spdk_strtol(arg, 10);
		break;
	case 'o':
        g_osd_addr = arg;
		break;
	case 't':
	    g_osd_port = spdk_strtol(arg, 10);
		break;
	case 'U':
	    g_uuid  = arg;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

void date_disk_init_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to initialize the date disk, rberrno %d\n", rberrno);
		spdk_app_stop(rberrno);
		return;
	}
	server_t *server = (server_t *)arg;
    SPDK_NOTICELOG("------block start, cpu count : %u  bdev_disk: %s\n", 
	        spdk_env_get_core_count(), server->bdev_disk.c_str());
    global_pm = new partition_manager(
		    server->node_id,  
			server->mon_addr, server->mon_port,
			server->osd_addr, server->osd_port, server->osd_uuid);
	if(global_pm->connect_mon() != 0){
		spdk_app_stop(-1);
		return;
	}

    rpc_server& rserver = rpc_server::get_server(global_pm->get_shard());
	auto rs = new raft_service<partition_manager>(global_pm);
	auto os = new osd_service(global_pm);
	rserver.register_service(rs);
	rserver.register_service(os);
	rserver.start(server->osd_addr, server->osd_port);
}

void log_disk_init_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to initialize the log disk, rberrno %d\n", rberrno);
		spdk_app_stop(rberrno);
		return;
	}
	server_t *server = (server_t *)arg;
      //初始化数据盘
    blobstore_init(server->bdev_disk.c_str(), date_disk_init_complete, arg);	 
}

static void
block_started(void *arg)
{
    server_t *server = (server_t *)arg;

    buffer_pool_init();
      //初始化log磁盘
    blobstore_init(server->bdev_disk.c_str(), log_disk_init_complete, arg); 
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	server_t server = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "block";
    opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;
    ::spdk_log_set_flag("rdma");
    ::spdk_log_set_flag("msg");
    ::spdk_log_set_flag("mon");

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:I:D:H:P:o:t:U:", NULL,
				      block_parse_arg, block_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	if (g_pid_path) {
		save_pid(g_pid_path);
	}

    server.node_id = global_osd_id;
    if(!g_bdev_disk){
		std::cerr << "No bdev name is specified" << std::endl;
		return -1;
	}
	server.bdev_disk = g_bdev_disk;
	server.mon_addr = g_mon_addr;
	server.mon_port = g_mon_port;
	server.osd_addr = g_osd_addr;
	server.osd_port = g_osd_port;
	server.osd_uuid = g_uuid;

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, block_started, &server);

	spdk_app_fini();
	buffer_pool_fini();

	return rc;
}