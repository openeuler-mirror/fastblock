#ifndef MON_CLIENT_H_
#define MON_CLIENT_H_
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/sock.h"
#include "osd/mon_msg.pb.h"
#include "utils/utils.h"

struct osd_map_t
{
	std::map<int32_t, osd_info_t> osd_map;
	int64_t osdmap_version{-1};
};

struct pg_info_t
{
	int32_t pgid;
	std::vector<int32_t> osd_list;
};

struct pg_map_t
{
	std::map<int32_t, std::map<int32_t, pg_info_t>> pool_pg_map;
	std::map<int32_t, int64_t> pool_version;
};

class partition_manager;

struct mon_client{
    mon_client(std::string& _mon_host, int _mon_port, int _osd_id, std::string& _osd_addr,
            int _osd_port, std::string& _osd_uuid, partition_manager* _pm)
    : mon_host(_mon_host)
    , mon_port(_mon_port)
    , osd_id(_osd_id)
    , is_booted(false)
    , is_running(false)
    , pm(_pm)
    , osd_addr(_osd_addr)
    , osd_port(_osd_port)
    , osd_uuid(_osd_uuid) {}

    int connect_mon();
    int sock_quit(int rc);
    int send_bootrequest();

    void sock_shutdown(void){
	    is_running = false;
    }

	std::string mon_host;
	int mon_port;
	int osd_id;
    bool is_booted;
    bool is_running;
    partition_manager* pm;
    std::string osd_addr;
    int osd_port;
    std::string osd_uuid;

	struct spdk_sock *sock;
	struct spdk_sock_group *group;
	struct spdk_poller *poller_in;
	struct spdk_poller *poller_getosdmap;
	struct spdk_poller *poller_getpgmap;
	// struct spdk_poller *poller_printpgmap;
	struct spdk_poller *time_out;
    osd_map_t osdmap;
    pg_map_t pgmap;
    int rc;
};


#endif