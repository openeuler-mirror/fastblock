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
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "utils/log.h"
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

#include <csignal>
#include <dirent.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <sys/stat.h>
#include <sys/types.h>
#include <regex>

enum class stop_state {
    monitor_client = 1,
    partition_manager,
    connection_cache,
    rpc_server,
    blobstore
};

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
    spdk_thread *cur_thread;
    std::string bdev_addr;
    std::string bdev_type;
    int raft_heartbeat_period_time_msec;
    int raft_lease_time_msec;
    int raft_election_timeout_msec;
} server_t;

static const char* g_json_conf{nullptr};
static bool g_mkfs{false};
static bool g_force{false};
static const char *g_uuid{nullptr};
static std::string g_bdev_type;
int g_id{-1};
int g_pid_fd;

static std::unique_ptr<::raft_service<::partition_manager>> global_raft_service{nullptr};
static std::unique_ptr<::osd_service> global_osd_service{nullptr};
static std::shared_ptr<::connect_cache> global_conn_cache{nullptr};
static std::shared_ptr<::partition_manager> global_pm{nullptr};
static std::shared_ptr<monitor::client> monitor_client{nullptr};
static server_t osd_server{};
static int osd_exit_code{0};
static stop_state cur_stop_state{stop_state::monitor_client};

static std::string g_conf_path = "/var/lib/fastblock";

static void
block_usage(void) {
    printf("--------------- osd options --------------------\n");
    printf("-C, --conf <osd json config file> path to json config file\n");
    printf("-f, --mkfs    create new osd disk\n");
    printf("-F, --force   force to mkfs\n");
    printf("-I, --id <osd id> the osd id\n");
    printf("-U, --uuid <uuid> the uuid of the osd, used with --mkfs\n");
}

static struct option g_cmdline_opts[] = {
#define BLOCK_OPTION_CONF 'C'
	{
		.name = "conf",
		.has_arg = 1,
		.flag = NULL,
		.val = BLOCK_OPTION_CONF,
	},
#define BLOCK_OPTION_MKFS 'f'
	{
		.name = "mkfs",
		.has_arg = 0,
		.flag = NULL,
		.val = BLOCK_OPTION_MKFS,
	},
#define BLOCK_OPTION_FORCE 'F'
	{
		.name = "force",
		.has_arg = 0,
		.flag = NULL,
		.val = BLOCK_OPTION_FORCE,
	},
#define BLOCK_OPTION_ID 'I'
	{
		.name = "id",
		.has_arg = 1,
		.flag = NULL,
		.val = BLOCK_OPTION_ID,
	},
#define BLOCK_OPTION_UUID 'U'
	{
		.name = "uuid",
		.has_arg = 1,
		.flag = NULL,
		.val = BLOCK_OPTION_UUID,
	},
	{
		.name = NULL
	}
};

static void
save_pid(const char *pid_path)
{
    int fd;
    struct flock lock;

    fd = open(pid_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1){
        fprintf(stderr, "Couldn't create pid file '%s': %s\n", pid_path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    lock.l_type = F_WRLCK; 
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    if(fcntl(fd, F_SETLK, &lock) == -1){
        if (errno == EACCES || errno == EAGAIN){
            std::cerr << "The osd " << g_id << " has been started." << std::endl;
        } else {
            perror("fcntl");
        }
        close(fd);
        exit(EXIT_FAILURE);
    }

    std::string pid = std::to_string(getpid());
    ssize_t bytes_written = write(fd, pid.c_str(), pid.size());
    if (bytes_written == -1)
    {
        std::cerr << "failed to write pid file" << std::endl;
        close(fd);
        exit(EXIT_FAILURE);
    }
    g_pid_fd = fd;
}

static int
block_parse_arg(int ch, char *arg)
{
	switch (ch) {
    case BLOCK_OPTION_CONF:
        g_json_conf = arg;
        break;
    case BLOCK_OPTION_MKFS:
        g_mkfs = true;
        break;
    case BLOCK_OPTION_FORCE:
        g_force = true;
        break;
    case BLOCK_OPTION_ID:
        g_id = atoi(arg);
        break;
    case BLOCK_OPTION_UUID:
        g_uuid = arg;
        break;
	default:
        block_usage();
		return -EINVAL;
	}
	return 0;
}

static void service_init(partition_manager* pm, server_t *server){
	global_raft_service = std::make_unique<::raft_service<::partition_manager>>(global_pm.get());
    global_osd_service = std::make_unique<::osd_service>(global_pm.get(), monitor_client);

    // FIXME: options from json configuration file
    auto srv_opts = msg::rdma::server::make_options(server->pt);
    srv_opts->port = server->osd_port;
    try {
        server->rpc_srv = std::make_unique<rpc_server>(
          ::spdk_env_get_current_core(),
          srv_opts);
    } catch (const std::exception& e) {
        SPDK_ERRLOG_EX("ERROR: Create rpc server failed, %s\n", e.what());
        std::raise(SIGINT);
        return;
    }
    server->osd_addr = srv_opts->bind_address;
	server->rpc_srv->register_service(global_raft_service.get());
	server->rpc_srv->register_service(global_osd_service.get());
}

void start_monitor(server_t* ctx) {
    monitor_client->start();
    monitor::client::on_response_callback_type cb =
      [] (const monitor::client::response_status s, monitor::client::request_context* req_ctx) {
          int result = s;
          if (result != err::E_SUCCESS) {
              SPDK_ERRLOG_EX("ERROR: OSD boot failed: %s\n", err::string_status(result));
              osd_exit_code = result;
              std::raise(SIGINT);
              return;
          }

          req_ctx->this_client->start_cluster_map_poller();
      };

    monitor_client->emplace_osd_boot_request(
      ctx->node_id, ctx->osd_addr, ctx->osd_port, ctx->osd_uuid,
      1024 * 1024, std::move(cb));
}

static void pm_init(void *arg){
	server_t *server = (server_t *)arg;
    SPDK_INFOLOG_EX(osd,
                       "Block start, cpu count: %u, bdev_disk: %s\n",
                       spdk_env_get_core_count(),
                       server->bdev_disk.c_str());

    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
    auto opts = msg::rdma::client::make_options(server->pt);
    global_conn_cache = std::make_shared<::connect_cache>(&cpumask, opts);
    global_pm = std::make_shared<partition_manager>(server->node_id, global_conn_cache, 
      server->raft_heartbeat_period_time_msec, server->raft_lease_time_msec, server->raft_election_timeout_msec);
    monitor::client::on_new_pg_callback_type pg_map_cb =
      [pm = global_pm] (const msg::PGInfo& pg_info, const monitor::client::pg_map::pool_id_type pool_id, const monitor::client::osd_map& osd_map,
        monitor::client::pg_op_complete&& cb_fn, void *arg) {
          std::vector<utils::osd_info_t> osds{};
          for (auto osd_id : pg_info.osdid()) {
              osds.push_back(*(osd_map.data.at(osd_id)));
          }
          pm->create_partition(pool_id, pg_info.pgid(), std::move(osds), 0, std::move(cb_fn), arg);
      };
    monitor_client = std::make_shared<monitor::client>(
      server->monitors, global_pm, std::move(pg_map_cb), std::nullopt, server->node_id);
}

void storage_init_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_ERRLOG_EX("Failed to initialize the storage system: %s. thread id %lu\n",
            spdk_strerror(rberrno), utils::get_spdk_thread_id());
        osd_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    SPDK_NOTICELOG_EX("mkfs done, thread id %lu\n", utils::get_spdk_thread_id());
    osd_exit_code = 0;
    std::raise(SIGINT);
}

void disk_init_complete(void *arg, int rberrno) {
    if(rberrno != 0){
		SPDK_NOTICELOG_EX("Failed to initialize the disk: %s. thread id %lu\n",
            err::string_status(rberrno), utils::get_spdk_thread_id());
        osd_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    SPDK_INFOLOG_EX(osd, "Initialize the disk completed, thread id %lu\n",
                       utils::get_spdk_thread_id());
    storage_init(storage_init_complete, arg);
}

struct oad_load_ctx{
    server_t *server;
    partition_manager* pm;
};

class osd_load;
struct load_op_ctx{
    oad_load_ctx* ctx;
    osd_load* load;
    pm_complete func;
    uint32_t shard_id;
};

class osd_load{
public:
    osd_load(std::map<std::string, struct spdk_blob*>&& log_blobs,
            std::map<std::string, object_store::container>&& object_blobs)
    : _log_blobs(std::move(log_blobs))
    , _object_blobs(std::move(object_blobs)) {}

    void start_load(uint32_t shard_id, pm_complete func, oad_load_ctx* ctx){
        iter_start();
        if(iter_is_end()){
            func(ctx, 0);
            return;
        }
        auto pg = iter->first;
        struct spdk_blob* blob = iter->second;
        uint64_t pool_id;
        uint64_t pg_id;
        if(!pg_to_id(pg, pool_id, pg_id)){
            func(ctx, err::E_INVAL);
            return;
        }

        object_store::container objects;
        auto iter = _object_blobs.find(pg);
        if(iter != _object_blobs.end()){
            objects = std::move(iter->second);
        }

        auto blob_size = spdk_blob_get_num_clusters(blob) * spdk_bs_get_cluster_size(global_blobstore());
        SPDK_INFOLOG_EX(osd, "load blob, blob id %ld blob size %lu\n", spdk_blob_get_id(blob), blob_size);
        load_op_ctx *op_ctx = new load_op_ctx{.ctx = ctx, .load = this,
                                            .func = std::move(func), .shard_id = shard_id};
        ctx->pm->load_partition(shard_id, pool_id, pg_id, blob, std::move(objects), start_continue, op_ctx);
    }

    static void start_continue(void *arg, int lerrno){
        load_op_ctx* ctx = (load_op_ctx *)arg;
        if(lerrno){
            ctx->func(ctx->ctx, lerrno);
            delete ctx;
            return;
        }

        auto iter = ctx->load->iter_next();
        if(!ctx->load->iter_is_end()){
            auto pg = iter->first;
            struct spdk_blob* blob = iter->second;
            uint64_t pool_id;
            uint64_t pg_id;
            if(!ctx->load->pg_to_id(pg, pool_id, pg_id)){
                ctx->func(ctx->ctx, err::E_INVAL);
                delete ctx;
                return;
            }

            SPDK_INFOLOG_EX(osd, "pg %s\n", pg.c_str());
            object_store::container objects;
            auto it = ctx->load->_object_blobs.find(pg);
            if(it != ctx->load->_object_blobs.end()){
                objects = std::move(it->second);
            }

            ctx->ctx->pm->load_partition(ctx->shard_id, pool_id, pg_id, blob, std::move(objects), start_continue, ctx);
            return;
        }

        ctx->func(ctx->ctx, 0);
        delete ctx;
    }

    void iter_start() {
        iter = _log_blobs.begin();
    }

    std::map<std::string, struct spdk_blob*>::iterator iter_next(){
        iter++;
        return iter;
    }
    bool iter_is_end() { return iter == _log_blobs.end(); }

    std::map<std::string, object_store::container>::iterator find_objects(std::string pg){
        return _object_blobs.find(pg);
    }

    bool pg_to_id(std::string pg, uint64_t& pool_id, uint64_t& pg_id){
        auto pos = pg.find(".");
        if(pos == std::string::npos)
            return false;

        try{
            std::string val1 = pg.substr(0, pos);
            std::string val2 = pg.substr(pos + 1, pg.size());
            pool_id = stoull(val1);
            pg_id = stoull(val2);
        }
        catch (std::invalid_argument){
            return false;
        }
        return true;
    }
public:

    std::map<std::string, struct spdk_blob*> _log_blobs;
    std::map<std::string, object_store::container> _object_blobs;
    std::map<std::string, struct spdk_blob*>::iterator iter;
};

struct pm_load_context : public utils::context{
    server_t *server;
    partition_manager* pm;

    pm_load_context(server_t *_server, partition_manager* _pm)
    : server(_server)
    , pm(_pm) {}

    void finish(int r) override {
		if(r != 0){
            SPDK_ERRLOG_EX("load osd failed: %s\n", spdk_strerror(r));
            osd_exit_code = r;
            std::raise(SIGINT);
			return;
		}

        // SPDK_WARNLOG_EX("pm start done\n");
        auto& blobs = global_blob_tree();

        //这里只处理单核的
        uint32_t shard_id = 0;
        std::map<std::string, struct spdk_blob*> log_blobs = std::exchange(blobs.on_shard(shard_id).log_blobs, {});
        std::map<std::string, object_store::container> object_blobs = std::exchange(blobs.on_shard(shard_id).object_blobs, {});
        osd_load* load = new osd_load(std::move(log_blobs), std::move(object_blobs));

        auto load_done = [load](void *arg, int lerrno){
            oad_load_ctx* ctx = (oad_load_ctx* )arg;
            if(lerrno != 0){
                SPDK_ERRLOG_EX("load osd failed: %s\n", spdk_strerror(lerrno));
                delete ctx;
                delete load;
                osd_exit_code = lerrno;
                std::raise(SIGINT);
                return;
            }

            service_init(ctx->pm, ctx->server);
            start_monitor(ctx->server);
            delete ctx;
            delete load;
        };
        oad_load_ctx* ctx = new oad_load_ctx{.server = server, .pm = pm};
        load->start_load(shard_id, std::move(load_done), ctx);
    }
};

static void osd_service_load(void *arg){
    server_t *server = (server_t *)arg;
    pm_init(arg);

    pm_load_context *ctx = new pm_load_context{server, global_pm.get()};
	global_pm->start(ctx, monitor_client);
}

void storage_load_complete(void *arg, int rberrno){
    SPDK_INFOLOG_EX(osd, "storage load done, thread id %lu\n", utils::get_spdk_thread_id());
    if(rberrno != 0){
		SPDK_ERRLOG_EX("Failed to initialize the storage system: %s\n", spdk_strerror(rberrno));
        osd_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    osd_service_load(arg);
}

void disk_load_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG_EX("Failed to initialize the disk: %s. thread id %lu\n",
            err::string_status(rberrno), utils::get_spdk_thread_id());
        osd_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    server_t *server = (server_t *)arg;
    SPDK_INFOLOG_EX(osd, "load blobstore done, uuid %s, thread id %lu\n",
                       server->osd_uuid.c_str(), utils::get_spdk_thread_id());

    storage_load(storage_load_complete, arg);
}

static inline std::string get_bdev_json_file_name(){
    std::string file_name = "/var/tmp/osd_bdev_" + std::to_string(getpid()) + ".json";
    return file_name;
}

static inline std::string get_bdev_disk_addr_file() {
    //部署集群时创建此文件，并把磁盘地址写入
    std::string file_name = g_conf_path + "/osd-" + std::to_string(g_id) + "/disk";
    return  file_name;   
}

static inline std::string get_bdev_type_file() {
    //部署集群时创建此文件，并把磁盘地址写入
    std::string file_name = g_conf_path + "/osd-" + std::to_string(g_id) + "/bdev_type";
    return  file_name;   
}


static std::optional<std::string> get_file_data(std::string &file_name){
    if(!std::filesystem::exists(file_name)){
        return std::nullopt;
    }
    std::ifstream ifs(file_name);

    if(!ifs.is_open()){
        return std::nullopt;
    }

    std::string data;
    if(!getline(ifs, data)){
        ifs.close();
        return std::nullopt;
    }

    ifs.close();
    return data;
}

static void remove_bdev_json_file(){
    std::string bdev_json_file = get_bdev_json_file_name();
    if(std::filesystem::exists(bdev_json_file)){
        remove(bdev_json_file.c_str());
    }
}

static void
block_started(void *arg)
{
    server_t *server = (server_t *)arg;

    remove_bdev_json_file();
    if(server->bdev_type == "nvme")
        server->bdev_disk = server->bdev_disk + "n1";

    buffer_pool_init();
    if(g_mkfs){
        //初始化log磁盘
        blobstore_init(server->bdev_disk, server->osd_uuid, g_force,
                disk_init_complete, arg);
        return;
    }else{
        blobstore_load(server->bdev_disk, disk_load_complete, arg, &(server->osd_uuid));
    }
}

static void save_bdev_json(std::string& bdev_json_file, server_t& server){
    std::ofstream ofs(bdev_json_file);

    if(ofs.is_open()){
        ofs << "{\n";
        ofs << "    \"subsystems\": [\n";
        ofs << "       {\n";
        ofs << "         \"subsystem\": \"bdev\",\n";
        ofs << "         \"config\": [\n";
        ofs << "             {\n";
      if(server.bdev_type == "nvme"){
        ofs << "               \"method\": \"bdev_nvme_attach_controller\",\n";
        ofs << "               \"params\": {\n";
        ofs << "                  \"name\": \"";
        ofs << server.bdev_disk;
        ofs << "\",\n";
        ofs << "                  \"trtype\": \"pcie\",\n";
        ofs << "                  \"traddr\": \"";
      }else{
        ofs << "               \"method\": \"bdev_aio_create\",\n";
        ofs << "               \"params\": {\n";
        ofs << "                  \"name\": \"";
        ofs << server.bdev_disk;
        ofs << "\",\n";
        ofs << "                  \"block_size\": 512,\n";
        ofs << "                  \"filename\": \"";
      }
        ofs << server.bdev_addr;
        ofs << "\"\n";
        ofs << "               }\n";
        ofs << "             }\n";
        ofs << "         ]\n";
        ofs << "       }\n";
        ofs << "    ]\n";
        ofs << "}\n";
        ofs.close();
    }
}

static int from_configuration(server_t* server) {
    auto& pt = server->pt;

    if(g_mkfs){
        server->osd_uuid = g_uuid;
    }
    auto current_osd_id = std::to_string(g_id);
    server->node_id = g_id;

    std::string rdma_device_name = pt.get_child("rdma_device_name").get_value<std::string>();

    server->bdev_disk = g_bdev_type + current_osd_id;
    server->osd_port = utils::get_random_port();
    server->bdev_type = g_bdev_type;

    if (pt.count("mon_host") == 0) {
        std::cerr << "No monitor cluster is specified" << std::endl;
        return -1;
    }

    auto& monitors = pt.get_child("mon_host");
    auto pos = monitors.begin();
    for(; pos != monitors.end(); pos++){
        auto mon_addr = pos->second.get_value<std::string>();
        server->monitors.emplace_back(std::move(mon_addr), utils::default_monitor_port);
    }

    if (server->monitors.empty()) {
        std::cerr << "No monitor cluster is specified" << std::endl;
        return -1;
    }

    return 0;
}

static std::string get_pid_path() {
    auto current_osd_id = std::to_string(g_id);
    std::string pid_path = "/var/tmp/osd" + current_osd_id + ".pid";
    return  pid_path;   
}

static void clean_file(){
    std::string pid_path = get_pid_path();

    struct flock lock;
    lock.l_type = F_UNLCK; 
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    fcntl(g_pid_fd, F_SETLK, &lock);
    close(g_pid_fd);
    unlink(pid_path.c_str());
}

static void on_blob_unloaded([[maybe_unused]] void *cb_arg, int bserrno) {
    SPDK_NOTICELOG_EX("The blob has been unloaded, return code is %d, thread id %lu\n",
            bserrno, utils::get_spdk_thread_id());
    auto& sharded_service = core_sharded::get_core_sharded();
    SPDK_NOTICELOG_EX("Start stopping sharded service\n");
    sharded_service.stop();
    clean_file();
    SPDK_NOTICELOG_EX("Stop the spdk app\n");
    ::spdk_app_stop(osd_exit_code);
}

static void on_blob_closed([[maybe_unused]] void *cb_arg, int bserrno) {
    SPDK_NOTICELOG_EX(
      "The bdev has been closed, return code is %d, thread id %lu\n",
      bserrno, utils::get_spdk_thread_id());
    SPDK_NOTICELOG_EX("Start unloading bdev\n");
    ::blobstore_fini(on_blob_unloaded, nullptr);
}

static void on_osd_stop() noexcept;

static void on_pm_closed([[maybe_unused]] void *cb_arg, int bserrno) {
    SPDK_NOTICELOG_EX("The partition manager has been closed, return code is %d\n", bserrno);
    on_osd_stop();
}

static void on_osd_stop() noexcept {
    SPDK_NOTICELOG_EX("Stop the osd service, thread id %lu\n", utils::get_spdk_thread_id());
    switch (cur_stop_state) {
    case stop_state::monitor_client: {
        cur_stop_state = stop_state::partition_manager;
        if (monitor_client) {
            SPDK_NOTICELOG_EX("Stopping the monitor client\n");
            monitor_client->stop(on_osd_stop);
            return;
        }
        [[fallthrough]];
    }
    case stop_state::partition_manager: {
        cur_stop_state = stop_state::connection_cache;
        if (global_pm) {
            SPDK_NOTICELOG_EX("Stopping the partition manager\n");
            global_pm->stop(on_pm_closed, nullptr);
            return;
        }
        [[fallthrough]];
    }
    case stop_state::connection_cache: {
        cur_stop_state = stop_state::rpc_server;
        if (global_conn_cache) {
            SPDK_NOTICELOG_EX("Stopping the connection cache\n");
            global_conn_cache->stop(on_osd_stop);
            return;
        }
        [[fallthrough]];
    }
    case stop_state::rpc_server: {
        cur_stop_state = stop_state::blobstore;
        if (osd_server.rpc_srv) {
            SPDK_NOTICELOG_EX("Stopping the rpc server\n");
            osd_server.rpc_srv->stop(on_osd_stop);
            return;
        }
        [[fallthrough]];
    }
    case stop_state::blobstore: {
        SPDK_NOTICELOG_EX("Stopping the blobstore\n");
        ::storage_fini(on_blob_closed, nullptr);
        return;
    }
    default:
        return;
    }
}

static bool check_disk_addr_valid(std::string type, std::string addr){
    if(type == "aio"){
         std::regex pattern("^/.+");
         if(!std::regex_match(addr, pattern)){
             std::cerr << "disk path: " << addr << " is not absolute path" << std::endl;
             return false;
         }
         struct stat fstat;
         if(stat(addr.c_str(), &fstat) != 0){
             std::cerr << "disk path: " << addr << " is not exist" << std::endl;
             return false;
         }
         if(!S_ISBLK(fstat.st_mode) && !S_ISREG(fstat.st_mode)){
             std::cerr << "disk path: " << addr << " is not a block device or file" << std::endl;
             return false;
         }
    }else{
        std::regex pattern("^[a-z0-9]{4}:[a-z0-9]{2}:[a-z0-9]{2}\\.[a-z0-9]{1}$");
        if(!std::regex_match(addr, pattern)){
             std::cerr << "disk BDF " << addr << " is invalid" << std::endl;
             return false;
        }
    }
    return true;
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "block";

	// disable tracing because it's memory consuming
	opts.num_entries = 0;
    opts.shutdown_cb = on_osd_stop;
	opts.print_level = ::spdk_log_level::SPDK_LOG_DEBUG;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:I:U:fF", g_cmdline_opts,
				      block_parse_arg, block_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

    if (g_id == -1) {
        std::cerr << "--id <osd id> or -I <osd id> must be set\n";
        return -EINVAL;
    }

    if (g_mkfs){
        if(!g_uuid) {
            std::cerr << "--uuid <uuid> must be set when --mkfs is set\n";
            return -EINVAL;
        }
    }

    auto disk_addr_file = get_bdev_disk_addr_file();
    auto disk_addr = get_file_data(disk_addr_file);
    if(!disk_addr){
        std::cerr << "Can not get disk addr from file " << disk_addr_file << std::endl;
        return -EINVAL;
    }

    auto bdev_type_file = get_bdev_type_file();
    auto bdev_type = get_file_data(bdev_type_file);
    if(!bdev_type){
        std::cerr << "Can not get bdev type from file " << bdev_type_file << std::endl;
        return -EINVAL;
    }  

    g_bdev_type =  bdev_type.value();
    if((g_bdev_type != "aio") &&  (g_bdev_type != "nvme")){
        std::cerr << "the type muset be 'aio' or 'nvme'" << std::endl;
        return -EINVAL;
    }

    if(!check_disk_addr_valid(g_bdev_type, disk_addr.value())){
        return -EINVAL;
    }

    std::string bdev_json_file = get_bdev_json_file_name();

    osd_server.bdev_addr = disk_addr.value();

    try {
        boost::property_tree::read_json(std::string(g_json_conf), osd_server.pt);
    } catch (const std::logic_error& e) {
        std::string err_reason{e.what()};
        std::cerr << "ERROR: Parse json configuration file error, reason is " << err_reason << std::endl;
        block_usage();
        return -EINVAL;
    }

    static int min_raft_time_msec = 500;
    static int max_raft_time_msec = 30000;
    if(osd_server.pt.count("raft_heartbeat_period_time_msec") > 0){
        int value = osd_server.pt.get_child("raft_heartbeat_period_time_msec").get_value<int>();
        if(value < min_raft_time_msec || value > max_raft_time_msec){
            std::cerr << "raft_heartbeat_period_time_msec must be in [" << 
              min_raft_time_msec << "," <<  max_raft_time_msec << "], which is measured in milliseconds" << std::endl;
            return -EINVAL;
        }
        osd_server.raft_heartbeat_period_time_msec = value;
    }
    if(osd_server.pt.count("raft_lease_time_msec") > 0){
        int value = osd_server.pt.get_child("raft_lease_time_msec").get_value<int>();
        if(value < min_raft_time_msec || value > max_raft_time_msec){
            std::cerr << "raft_lease_time_msec must be in [" <<
              min_raft_time_msec << "," <<  max_raft_time_msec << "], which is measured in milliseconds" << std::endl;
            return -EINVAL;
        }
        osd_server.raft_lease_time_msec = value;
    }
    if(osd_server.pt.count("raft_election_timeout_msec") > 0){
        int value = osd_server.pt.get_child("raft_election_timeout_msec").get_value<int>();
        if(value < min_raft_time_msec || value > max_raft_time_msec){
            std::cerr << "raft_election_timeout_msec must be in [" <<
              min_raft_time_msec << "," <<  max_raft_time_msec << "], which is measured in milliseconds" << std::endl;
            return -EINVAL;
        }
        osd_server.raft_election_timeout_msec = value;
    }

    if(from_configuration(&osd_server) != 0){
        return -EINVAL;
    }
    
    std::string pid_path = get_pid_path();
    save_pid(pid_path.c_str());

    remove_bdev_json_file();
    save_bdev_json(bdev_json_file, osd_server);
    
    opts.json_config_file = bdev_json_file.c_str();

    auto current_osd_id = std::to_string(g_id);
    std::string sock_path = "/var/tmp/fastblock-osd-" + current_osd_id + ".sock";
    opts.rpc_addr = sock_path.c_str();
	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, block_started, &osd_server);
    if(rc == err::E_INVAL){
        std::cerr << "the disk " << osd_server.bdev_addr << " is probably invalid" << std::endl;
    }

    if(rc != 0)
        remove_bdev_json_file();

	spdk_app_fini();
	buffer_pool_fini();

	return rc;
}
