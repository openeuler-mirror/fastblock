#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "raft/raft.h"
#include "osd/partition_manager.h"
#include "raft/raft_service.h"
#include "osd/osd_service.h"

static const char *g_pid_path = nullptr;
static int global_osd_id = 0;
static const char * g_log_dir = nullptr;
static const char * g_date_dir = nullptr;
static partition_manager* global_pm = nullptr;
static raft_service<partition_manager>* global_rs = nullptr;
static osd_service* global_os = nullptr;

typedef struct
{
    /* the server's node ID */
    int node_id;
	std::string log_dir;
	std::string date_dir;
}server_t;

static void
block_usage(void)
{
	printf(" -f <path>                 save pid to file under given path\n");
	printf(" -I <id>                   save osd id\n");
	printf(" -l <logdir>               the log directory\n");
	printf(" -D <datedir>              the date directory\n");
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
	    global_osd_id = atoi(arg);
		break;
	case 'l':
	    g_log_dir = arg;
		break;
	case 'D':
	    g_date_dir = arg;
		break;	
	default:
		return -EINVAL;
	}
	return 0;
}

static void
block_started(void *arg1)
{
    server_t *server = (server_t *)arg1;
	
    SPDK_NOTICELOG("------block start, cpu count : %u  log_dir : %s date_dir: %s\n", 
	        spdk_env_get_core_count(), server->log_dir.c_str(), server->date_dir.c_str());
    global_pm = new partition_manager(server->node_id, spdk_env_get_core_count(), server->log_dir, server->date_dir);
    global_pm->create_spdk_threads();

	global_rs = new raft_service<partition_manager>(global_pm);
	global_os = new osd_service(global_pm);
    
	// std::vector<osd_info_t> osds;
	// osds.push_back(osd_info_t{1, "192.168.1.11", 8888});
	// global_pm->create_partition(1, 1, std::move(osds), 1);
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	server_t server = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "block";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:I:l:D:", NULL,
				      block_parse_arg, block_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	if (g_pid_path) {
		save_pid(g_pid_path);
	}

    server.node_id = global_osd_id;
    if(g_log_dir)
	    server.log_dir = g_log_dir;
	if(g_date_dir)
	    server.date_dir = g_date_dir;

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, block_started, &server);

	spdk_app_fini();

	return rc;
}