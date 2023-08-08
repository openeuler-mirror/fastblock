#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "rpc/connect_cache.h"
#include "rpc/osd_msg.pb.h"
#include "msg/rpc_controller.h"
#include <google/protobuf/stubs/callback.h>
#include <random>


static const char *g_pid_path = nullptr;
static int global_osd_id = 0;
static const char* g_osd_addr = "127.0.0.1";
static  int g_osd_port = 8888;
typedef struct
{
    /* the server's node ID */
    int node_id;
	std::string osd_addr;
	int osd_port;
}server_t;

static void
block_usage(void)
{
	printf(" -f <path>                 save pid to file under given path\n");
    printf(" -I <id>                   save osd id\n");
    printf(" -o <osd_addr>             osd address\n");
	printf(" -t <osd_port>             osd port\n");	
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
	case 'o':
        g_osd_addr = arg;
		break;
	case 't':
	    g_osd_port = spdk_strtol(arg, 10);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

class write_request_source{
public:
    write_request_source(osd::write_request* request)
    : _request(request) {}

    ~write_request_source(){
        if(_request)
            delete _request;
        if(_done)
            delete _done;        
    }

    void process_response(){
        SPDK_NOTICELOG("write_request pool: %lu pg:%lu object:%s offset:%lu done. state:%d\n",
                _request->pool_id(), _request->pg_id(), _request->object_name().c_str(), _request->offset(), response.state());
    }

    void set_done(google::protobuf::Closure* done){
        _done = done;
    }

    msg::rdma::rpc_controller ctrlr;
    osd::write_reply response;
private:
    osd::write_request* _request;
    google::protobuf::Closure* _done;
};

class client{
public:
    client() 
    : _cache(connect_cache::get_connect_cache())
    , _shard_cores(get_shard_cores()) {
        uint32_t i = 0;
        auto shard_num = _shard_cores.size();
        for(i = 0; i < shard_num; i++){
            _stubs.push_back(std::map<int, std::shared_ptr<osd::rpc_service_osd_Stub>>());
        }
    }

    void create_connect(std::string& addr, int port, int node_id){
        uint32_t shard_id = 0;
        for(shard_id = 0; shard_id < _shard_cores.size(); shard_id++){
            SPDK_NOTICELOG("create connect to node %d (address %s, port %d) in core %u\n", 
                    node_id, addr.c_str(), port, _shard_cores[shard_id]);        
            auto connect = _cache.create_connect(0, node_id, addr, port);
            auto &stub = _stubs[shard_id];
            stub[node_id] = std::make_shared<osd::rpc_service_osd_Stub>(connect.get());
        }
    }

    void send_write_request(int32_t target_node_id, osd::write_request* request){
        SPDK_NOTICELOG("send write request\n");
        auto shard_id = 0;
        write_request_source * source = new write_request_source(request);

        auto done = google::protobuf::NewCallback(source, &write_request_source::process_response);
        source->set_done(done);
        auto stub = _get_stub(shard_id, target_node_id);
        stub->process_write(&source->ctrlr, request, &source->response, done);
    }
private:
    std::shared_ptr<osd::rpc_service_osd_Stub> _get_stub(uint32_t shard_id, int32_t  node_id) {
        auto &stubs = _stubs[shard_id];
        return stubs[node_id];
    }
    connect_cache& _cache;
    std::vector<uint32_t> _shard_cores;
    //每个cpu核上有一个map
    std::vector<std::map<int, std::shared_ptr<osd::rpc_service_osd_Stub>>> _stubs;
};

std::string random_string(const size_t length) {
    static std::string chars{
      "abcdefghijklmnopqrstuvwxyz"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "1234567890"
      "!@#$%^&*()"
      "`~-_=+[{]}\\|;:'\",<.>/? "};

    std::random_device rd{};
    std::uniform_int_distribution<decltype(chars)::size_type> index_dist{0, chars.size() - 1};
    std::string ret(length, ' ');
    for (size_t i{0}; i < length; ++i) {
        ret[i] = chars[index_dist(rd)];
    }

    return ret;
}

static void
block_started(void *arg1)
{
    server_t *server = (server_t *)arg1;
    SPDK_NOTICELOG("------block start, cpu count : %u \n", spdk_env_get_core_count());
	client *cli = new client();
    cli->create_connect(server->osd_addr, server->osd_port, server->node_id);
    osd::write_request* request = new osd::write_request();
    request->set_pool_id(1);
    request->set_pg_id(0);
    request->set_object_name("obj1");
    request->set_offset(0);
    request->set_data(random_string(4096));
    cli->send_write_request(server->node_id, request);
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	server_t server = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "block";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:I:o:t:", NULL,
				      block_parse_arg, block_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	if (g_pid_path) {
		save_pid(g_pid_path);
	}

    server.node_id = global_osd_id;
	server.osd_addr = g_osd_addr;
	server.osd_port = g_osd_port;	

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, block_started, &server);

	spdk_app_fini();

	return rc;
}