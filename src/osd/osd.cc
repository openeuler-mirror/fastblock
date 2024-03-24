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
} server_t;

static const char* g_json_conf{nullptr};
static bool g_mkfs{false};
static const char* g_uuid{nullptr};;
static int  g_id{-1};
static std::unique_ptr<::raft_service<::partition_manager>> global_raft_service{nullptr};
static std::unique_ptr<::osd_service> global_osd_service{nullptr};
static std::shared_ptr<::connect_cache> global_conn_cache{nullptr};
static std::shared_ptr<::partition_manager> global_pm{nullptr};
static std::shared_ptr<monitor::client> monitor_client{nullptr};
static server_t osd_server{};
static int osd_exit_code{0};

static void
block_usage(void) {
    printf("--------------- osd options --------------------\n");
    printf("-C, --conf <osd json config file> path to json config file\n");
    printf("-f, --mkfs create new osd disk\n");
    printf("-I, --id <osd id> the osd id\n");
    printf("-U, --uuid <uuid> the uuid of the osd\n");
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
    case BLOCK_OPTION_CONF:
        g_json_conf = arg;
        break;
    case BLOCK_OPTION_MKFS:
        g_mkfs = true;
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
    srv_opts->bind_address = server->osd_addr;
    srv_opts->port = server->osd_port;
    server->rpc_srv = std::make_unique<rpc_server>(
      ::spdk_env_get_current_core(),
      srv_opts);
	server->rpc_srv->register_service(global_raft_service.get());
	server->rpc_srv->register_service(global_osd_service.get());
}

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

static void pm_init(void *arg){
	server_t *server = (server_t *)arg;
    SPDK_INFOLOG(osd, 
      "Block start, cpu count: %u, bdev_disk: %s\n",
      spdk_env_get_core_count(),
      server->bdev_disk.c_str());

    auto core_no = ::spdk_env_get_current_core();
    ::spdk_cpuset cpumask{};
    ::spdk_cpuset_zero(&cpumask);
    ::spdk_cpuset_set_cpu(&cpumask, core_no, true);
    auto opts = msg::rdma::client::make_options(server->pt);
    global_conn_cache = std::make_shared<::connect_cache>(&cpumask, opts);
    global_pm = std::make_shared<partition_manager>(server->node_id, global_conn_cache);
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
		SPDK_ERRLOG("Failed to initialize the storage system: %s\n", spdk_strerror(rberrno));
        osd_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    SPDK_NOTICELOG("mkfs done.\n");
    osd_exit_code = 0;
    std::raise(SIGINT);
}

void disk_init_complete(void *arg, int rberrno) {
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to initialize the disk. %s\n", err::string_status(rberrno));
        osd_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    SPDK_INFOLOG(osd,  "Initialize the disk completed\n");
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
        SPDK_INFOLOG(osd, "load blob, blob id %ld blob size %lu\n", spdk_blob_get_id(blob), blob_size);
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
                return;
            } 

            SPDK_INFOLOG(osd, "pg %s\n", pg.c_str());
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
            SPDK_ERRLOG("load osd failed: %s\n", spdk_strerror(r));
            osd_exit_code = r;
            std::raise(SIGINT);
			return;
		}
		
        // SPDK_WARNLOG("pm start done\n");
        auto& blobs = global_blob_tree();

        //这里只处理单核的
        uint32_t shard_id = 0;
        // for(auto &[pg_name, log_blob] : blobs.on_shard(shard_id).log_blobs){
            // SPDK_WARNLOG("pg_name %s blob id %lu\n", pg_name.c_str(), spdk_blob_get_id(log_blob));
        // }
        std::map<std::string, struct spdk_blob*> log_blobs = std::exchange(blobs.on_shard(shard_id).log_blobs, {});
        std::map<std::string, object_store::container> object_blobs = std::exchange(blobs.on_shard(shard_id).object_blobs, {});
        osd_load* load = new osd_load(std::move(log_blobs), std::move(object_blobs));
        
        auto load_done = [](void *arg, int lerrno){
            if(lerrno != 0){
                SPDK_ERRLOG("load osd failed: %s\n", spdk_strerror(lerrno));
                osd_exit_code = lerrno;
                std::raise(SIGINT);
                return;
            }
            // SPDK_WARNLOG("load osd done\n");
            oad_load_ctx* ctx = (oad_load_ctx* )arg;
            service_init(ctx->pm, ctx->server);
            start_monitor(ctx->server);
        };
        oad_load_ctx* ctx = new oad_load_ctx{.server = server, .pm = pm};
        load->start_load(shard_id, std::move(load_done), ctx);
    }
};

static void osd_service_load(void *arg){
    server_t *server = (server_t *)arg;
    pm_init(arg);

    pm_load_context *ctx = new pm_load_context{server, global_pm.get()};
	global_pm->start(ctx);
}

void storage_load_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_ERRLOG("Failed to initialize the storage system: %s\n", spdk_strerror(rberrno));
        osd_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    server_t *server = (server_t *)arg;
    auto cur_thread = spdk_get_thread();
    if(cur_thread != server->cur_thread){
        spdk_thread_send_msg(server->cur_thread, osd_service_load, arg);
    }else{
        osd_service_load(arg);
    }
}

void disk_load_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to initialize the disk. %s\n", spdk_strerror(rberrno));
        osd_exit_code = rberrno;
        std::raise(SIGINT);
		return;
	}

    server_t *server = (server_t *)arg;
    SPDK_INFOLOG(osd, "load blobstore done, uuid %s\n", server->osd_uuid.c_str());

    storage_load(storage_load_complete, arg);
}

static std::string get_bdev_json_file_name(){
    std::string file_name = "/var/tmp/osd_bdev_" + std::to_string(getpid()) + ".json";
    return file_name;
}

static void
block_started(void *arg)
{
    server_t *server = (server_t *)arg;
    std::string bdev_json_file = get_bdev_json_file_name();
    remove(bdev_json_file.c_str());
    
    buffer_pool_init();
    if(g_mkfs){
        //初始化log磁盘
        blobstore_init(server->bdev_disk.c_str(), server->osd_uuid, 
                disk_init_complete, arg);
        return;
    }

    server->cur_thread = spdk_get_thread();
    auto &shard = core_sharded::get_core_sharded();
    /*
      在spdk的函数bs_open_blob中有一个行“assert(spdk_get_thread() == bs->md_thread)”会检测当前spdk线程与blobstore的spdk线程是否相等，
      raft在读写log核对象数据之前会先使用bs_open_blob打开blob，为了保证在debug模式下正常运行，应该让raft的spdk线程与blobstore的spdk线程
      相同。
    */
    //这里先支持单核
    shard.invoke_on(
      0,
      [arg](){
        server_t *server = (server_t *)arg;
        blobstore_load(server->bdev_disk.c_str(), disk_load_complete, arg, &(server->osd_uuid));
      }
    );
}

std::string get_rdma_addr(std::string rdma_device_name){
    std::string dir_name = "/sys/class/infiniband/" + rdma_device_name + "/device/net/";
    DIR *dir;
    struct dirent *ent;
    std::string interface_name{};
    std::string ip_addr{};

    if ((dir = opendir(dir_name.c_str())) != nullptr){
        while ((ent = readdir(dir)) != nullptr){
            std::string filename = ent->d_name;
            if (filename != "." && filename != ".."){
                interface_name = filename;
                break;
            }
        }
    }else{
        std::cerr << "can not open dir " << dir_name << " to get interface name" << std::endl;
        return ip_addr;
    }

    if(interface_name.size() == 0){
        std::cerr << "can not get interface name" << std::endl;
        return ip_addr;
    }

    struct ifaddrs *interfaces = nullptr;
    int ret = getifaddrs(&interfaces);
    if(ret == 0){
        struct ifaddrs *ifaddr = nullptr;
        for (ifaddr = interfaces; ifaddr != nullptr; ifaddr = ifaddr->ifa_next) {
            if (ifaddr->ifa_addr && ifaddr->ifa_name && interface_name == ifaddr->ifa_name) {
                if (ifaddr->ifa_addr->sa_family == AF_INET){
                    struct sockaddr_in *ipAddr = reinterpret_cast<struct sockaddr_in *>(ifaddr->ifa_addr);
                    // ip_addr = inet_ntoa(ipAddr->sin_addr);
                   char addr[INET_ADDRSTRLEN];
                   inet_ntop(AF_INET, &(ipAddr->sin_addr), addr, INET_ADDRSTRLEN);
                   ip_addr = addr;
                   break;
                }
            }
        }
        freeifaddrs(interfaces);
    }
    closedir(dir);

    return ip_addr;
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

static void from_configuration(server_t* server, std::string& bdev_json_file) {
    auto& pt = server->pt;
    if(g_mkfs && !g_uuid) {
        std::cerr << "--uuid <uuid> must be set when --mkfs is set\n";
        std::raise(SIGINT);
        return;        
    }

    if(g_mkfs){
        server->osd_uuid = g_uuid;
    }
    auto current_osd_id = std::to_string(g_id);
    server->node_id = g_id;
    
    auto& osds = pt.get_child("osds");
    
    if(osds.count(current_osd_id) == 0){
        std::cerr << "The value of configuration key: " << current_osd_id << "in osds must be set\n";
        std::raise(SIGINT);
        return;
    }
    std::string bdev_addr = osds.get_child(current_osd_id).get_value<std::string>();
    std::string bdev_type = pt.get_child("bdev_type").get_value<std::string>();
    std::string rdma_device_name = pt.get_child("rdma_device_name").get_value<std::string>();

    if(bdev_type != "aio" &&  bdev_type != "nvme"){
        std::cerr << "the bdev_type in config file muset be 'aio' or 'nvme'" << std::endl;
        std::raise(SIGINT);
        return;
    }
    server->bdev_disk = bdev_type + current_osd_id;
    server->osd_port = utils::get_random_port();
    server->bdev_addr = bdev_addr;
    server->bdev_type = bdev_type;
    
    auto ip_addr = get_rdma_addr(rdma_device_name);
    if(ip_addr.size() == 0){
        std::raise(SIGINT);
        return;
    }
    server->osd_addr = ip_addr;

    if (pt.count("mon_host") == 0) {
        std::cerr << "No monitor cluster is specified" << std::endl;
        std::raise(SIGINT);
        return;
    }

    auto& monitors = pt.get_child("mon_host");
    auto pos = monitors.begin();
    for(; pos != monitors.end(); pos++){
        auto mon_addr = pos->second.get_value<std::string>();
        server->monitors.emplace_back(std::move(mon_addr), utils::default_monitor_port);
    }

    if (server->monitors.empty()) {
        std::cerr << "No monitor cluster is specified" << std::endl;
        std::raise(SIGINT);
        return;
    }

    std::string pid_path = "/var/tmp/osd" + current_osd_id + ".pid";
    save_pid(pid_path.c_str());
}

static void on_blob_unloaded([[maybe_unused]] void *cb_arg, int bserrno) {
    SPDK_NOTICELOG("The blob has been unloaded, return code is %d\n", bserrno);
    auto& sharded_service = core_sharded::get_core_sharded();
    SPDK_NOTICELOG("Start stopping sharded service\n");
    sharded_service.stop();
    SPDK_NOTICELOG("Stop the spdk app\n");
    ::spdk_app_stop(osd_exit_code);
}

static void on_blob_closed([[maybe_unused]] void *cb_arg, int bserrno) {
    SPDK_NOTICELOG("The bdev has been closed, return code is %d\n", bserrno);
    SPDK_NOTICELOG("Start unloading bdev\n");
    ::blobstore_fini(on_blob_unloaded, nullptr);
}

static void on_pm_closed([[maybe_unused]] void *cb_arg, int bserrno) {
    SPDK_NOTICELOG("The partition manager has been closed, return code is %d\n", bserrno);
}

static void on_osd_stop() noexcept {
    SPDK_NOTICELOG("Stop the osd service\n");

    if (monitor_client) {
        monitor_client->stop();
    }

    if (global_pm) {
        global_pm->stop(on_pm_closed, nullptr);
    }

    if (global_conn_cache) {
        global_conn_cache->stop();
    }

    if (osd_server.rpc_srv) {
        osd_server.rpc_srv->stop();
    }

    ::storage_fini(on_blob_closed, nullptr);
}

static void on_osd_stop(int signo) noexcept {
    SPDK_NOTICELOG("Catch signal %d\n", signo);
    on_osd_stop();
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
    std::signal(SIGINT, on_osd_stop);
    std::signal(SIGTERM, on_osd_stop);

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:I:U:f", g_cmdline_opts,
				      block_parse_arg, block_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

    if (g_id == -1) {
        std::cerr << "--id or -I <osd id> must be set\n";
        std::raise(SIGINT);
        return -EINVAL;
    }

    std::string bdev_json_file = get_bdev_json_file_name();

    SPDK_INFOLOG(osd, "Osd config file is %s\n", g_json_conf);
    try {
        boost::property_tree::read_json(std::string(g_json_conf), osd_server.pt);
    } catch (const std::logic_error& e) {
        std::string err_reason{e.what()};
        SPDK_ERRLOG(
          "ERROR: Parse json configuration file error, reason is '%s'\n",
          err_reason.c_str());
        block_usage();
        return -EINVAL;
    }
    from_configuration(&osd_server, bdev_json_file);

    save_bdev_json(bdev_json_file, osd_server);
    opts.json_config_file = bdev_json_file.c_str();
	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, block_started, &osd_server);

	spdk_app_fini();
	buffer_pool_fini();

	return rc;
}
