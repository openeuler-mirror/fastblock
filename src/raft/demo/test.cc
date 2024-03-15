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

#include "spdk/log.h"
#include "spdk/string.h"
#include "localstore/blob_manager.h"
#include "localstore/storage_manager.h"
#include "localstore/disk_log.h"
#include "utils/md5.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

static const char* g_json_conf{nullptr};
static bool g_mkfs{false};

typedef struct
{
	std::string bdev_disk;
}server_t;

uint64_t global_index = 1;

log_entry_t generate_log_entry(uint64_t type, const std::string meta, const std::string data){
    log_entry_t entry;

    entry.index = global_index++;
    entry.term_id = 1;
    entry.size = data.size();
    entry.type = type;
    entry.meta = meta;

    if (entry.size % 4096 != 0) {
        SPDK_ERRLOG("data size:%lu not align.\n", entry.size);
        return log_entry_t{};
    }	

	entry.data = make_buffer_list(entry.size / 4096);
    int i = 0;
    for (auto sbuf : entry.data) {
        sbuf.append(data.c_str() + i * 4096, 4096);
        i++;
    }

    auto data_md5 = utils::md5((char *)data.c_str(), data.size());
	SPDK_WARNLOG("index: %lu term_id %lu type: %lu size: %lu meta: %s data md5: %s\n",
	        entry.index, entry.term_id, entry.type, entry.size, entry.meta.c_str(), data_md5.c_str());
    return entry;	
}

static void storage_init_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to storage_init. %s\n", spdk_strerror(rberrno));
		spdk_app_stop(rberrno);
		return;
	}

	SPDK_WARNLOG("storage_init done\n");
}

static void append_done(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to append. %s\n", spdk_strerror(rberrno));
		spdk_app_stop(rberrno);
		return;
	}	  

	disk_log* dlog = (disk_log*)arg;
	dlog->load(
	  [](void *arg, int rberrno){
    	  if(rberrno != 0){
		  	SPDK_NOTICELOG("Failed to load. %s\n", spdk_strerror(rberrno));
		  	spdk_app_stop(rberrno);
		  	return;
		  }	            
	  },
	  nullptr);  
}

static void make_log_done(void *arg, struct disk_log* dlog, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to make_disk_log. %s\n", spdk_strerror(rberrno));
		spdk_app_stop(rberrno);
		return;
	}	

    std::vector<log_entry_t> entries;
	for(int i = 1; i <= 10; i++){
		auto entry = generate_log_entry(1, std::to_string(i), utils::random_string(4096 + 4096 * (i % 2)));
		entries.emplace_back(std::move(entry));
	}
	dlog->append(entries, append_done, dlog);
}

static void test_disk_log_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to storage_init. %s\n", spdk_strerror(rberrno));
		spdk_app_stop(rberrno);
		return;
	}

	SPDK_WARNLOG("storage_init done\n");

	make_disk_log(global_blobstore(), global_io_channel(), make_log_done, nullptr);    
}

static void disk_init_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to initialize the disk. %s\n", spdk_strerror(rberrno));
		spdk_app_stop(rberrno);
		return;
	}

	storage_init(storage_init_complete, arg);
}

static void storage_load_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to storage_load. %s\n", spdk_strerror(rberrno));
		spdk_app_stop(rberrno);
		return;
	}

}

static void disk_load_complete(void *arg, int rberrno){
    if(rberrno != 0){
		SPDK_NOTICELOG("Failed to initialize the disk. %s\n", spdk_strerror(rberrno));
		spdk_app_stop(rberrno);
		return;
	}

    storage_load(storage_load_complete, arg);
}

static void
hello_start(void *arg)
{
    server_t *server = (server_t *)arg;

    SPDK_WARNLOG("bdev_disk is %s\n", server->bdev_disk.c_str());
    buffer_pool_init();
      //初始化log磁盘
    if(g_mkfs){
        //初始化log磁盘
        blobstore_init(server->bdev_disk.c_str(), disk_init_complete, arg);
    }else{
        blobstore_load(server->bdev_disk.c_str(), disk_load_complete, arg);
    }
}

static void
block_usage(void)
{
	printf(" -C <osd json config file> path to json config file\n");
	printf(" -f <mkfs> create new disk [ture, false]\n");
}

static bool mkfs_arg(char *arg){
    if(strcasecmp(arg, "true") == 0 || strcmp(arg, "1") == 0)
       return true;
    return false;
}

static int
block_parse_arg(int ch, char *arg)
{
	switch (ch) {
    case 'C':
        g_json_conf = arg;
        break;
    case 'f':
        g_mkfs = mkfs_arg(arg);
        break;
	default:
		return -EINVAL;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	server_t server = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "hello";

	// disable tracing because it's memory consuming
	opts.num_entries = 0;
	opts.print_level = ::spdk_log_level::SPDK_LOG_WARN;	

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:f:", NULL,
				      block_parse_arg, block_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

    SPDK_WARNLOG("Osd config file is %s\n", g_json_conf);
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(std::string(g_json_conf), pt);

    server.bdev_disk = pt.get_child("bdev_disk").get_value<std::string>();
    if (server.bdev_disk.empty()) {
        std::cerr << "No bdev name is specified" << std::endl;
		return -1;
    }

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, hello_start, &server);

	spdk_app_fini();
	buffer_pool_fini();

	return rc;
}