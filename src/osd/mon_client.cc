#include<string>
#include "mon_client.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "partition_manager.h"


#define ACCEPT_TIMEOUT_US 1000
#define GET_OSDMAP_US 5000000
#define GET_PGMAP_US 5000000
#define CLOSE_TIMEOUT_US 1000000
#define BUFFER_SIZE 65536
#define ADDR_STR_LEN INET_ADDRSTRLEN

static char g_impl_name[] = "posix";

static int
fbclient_sock_close_timeout_poll(void *arg)
{
	mon_client *ctx = (mon_client *)arg;
	SPDK_NOTICELOG("Connection closed\n");

	spdk_poller_unregister(&ctx->time_out);
	spdk_poller_unregister(&ctx->poller_in);
	spdk_sock_close(&ctx->sock);
	// spdk_sock_group_close(&ctx->group);

	spdk_app_stop(ctx->rc);
	return SPDK_POLLER_BUSY;
}

int
mon_client::sock_quit(int _rc)
{
	rc = _rc;
	spdk_poller_unregister(&poller_getosdmap);
	spdk_poller_unregister(&poller_getpgmap);
	if (!time_out)
	{
		time_out = SPDK_POLLER_REGISTER(fbclient_sock_close_timeout_poll, this,
											 CLOSE_TIMEOUT_US);
	}
	return 0;
}

static int
fbclient_monitor_rpc_processer(void *arg)
{
	mon_client *ctx = (mon_client *)arg;
	int rc;
	char buf_in[BUFFER_SIZE];

	/*
	 * Get response
	 */
	rc = spdk_sock_recv(ctx->sock, buf_in, sizeof(buf_in) - 1);
	if (rc <= 0)
	{
		if (errno == EAGAIN || errno == EWOULDBLOCK)
		{
			return SPDK_POLLER_IDLE;
		}

		SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
					errno, spdk_strerror(errno));
		return SPDK_POLLER_BUSY;
	}

	if (rc > 0)
	{
		// SPDK_NOTICELOG("got %d bytes of data\r\n", rc);

		//(fixme) use switch/case
		// deserilize the response
		msg::Response resp;
		// resp.ParseFromString(buf_in);
		resp.ParseFromArray(buf_in, rc);
		if (resp.has_get_osdmap_response())
		{
			if (!ctx->is_booted)
			{
				// a osd not booted can't receive this kind of message
				return SPDK_POLLER_IDLE;
			}
			auto ec = resp.get_osdmap_response().errorcode();

			if (ec != msg::OsdMapErrorCode::ok)
			{
				SPDK_NOTICELOG("getosdmap: errorode is: %d\r\n", resp.get_osdmap_response().errorcode());
				return SPDK_POLLER_IDLE;
			}
			else
			{
				auto osdmap_version = resp.get_osdmap_response().osdmapversion();
				if (osdmap_version > ctx->osdmap.osdmap_version)
				{
					ctx->osdmap.osdmap_version = osdmap_version;
					// SPDK_NOTICELOG("getosdmap: errorode is: %d\r\n", resp.get_osdmap_response().errorcode());
					auto osds = resp.get_osdmap_response().osds();

					// they have higher version, update our map, we should clean o
					ctx->osdmap.osd_map.clear();
					for (int i = 0; i < osds.size(); i++)
					{
						osd_info_t osd_info;
						osd_info.node_id = osds[i].osdid();
						osd_info.isin = osds[i].isin();
						osd_info.isup = osds[i].isup();
						osd_info.ispendingcreate = osds[i].ispendingcreate();
						osd_info.port = osds[i].port();
                        osd_info.address = osds[i].address();
						ctx->osdmap.osd_map[osd_info.node_id] = std::move(osd_info);
					}
				}
				else if (osdmap_version == ctx->osdmap.osdmap_version)
				{
					SPDK_NOTICELOG("getosdmap: osdmapversion is the same, do nothing\r\n");
				}
			}
		}
		else if (resp.has_get_pgmap_response())
		{
			if (!ctx->is_booted)
			{
				// a osd not booted can't receive this kind of message
				return SPDK_POLLER_IDLE;
			}
			// SPDK_NOTICELOG("got getpgmap response\r\n");

			auto pv = resp.get_pgmap_response().poolid_pgmapversion();
			if (pv.empty())
			{
				SPDK_NOTICELOG("got pgmap, no pools created yet\r\n");
				return SPDK_POLLER_IDLE;
			}

			auto ec = resp.get_pgmap_response().errorcode();
			auto pgs = resp.get_pgmap_response().pgs();

			// pair.first为poolid，second为errorcode
			for (auto pair : ec)
			{
				// SPDK_NOTICELOG("pool %d ec is :%d\r\n", pair.first, pair.second);
				if (pair.second != msg::GetPgMapErrorCode::pgMapGetOk)
				{
					continue;
				}
				if (!pgs.contains(pair.first) || !pv.contains(pair.first))
				{
					SPDK_ERRLOG("pool %d has no pgmap, that is a error\r\n", pair.first);
					//(fixme)
					continue;
				}
				// this is a new pool, we don't know any thing about it, we trust 
				if (!ctx->pgmap.pool_pg_map.contains(pair.first))
				{
					ctx->pgmap.pool_version[pair.first] = pv[pair.first];

					// we'd rather do copy from protobuf data structure
					// clear pgmap for this pool
					ctx->pgmap.pool_pg_map[pair.first].clear();

					for (auto p : pgs)
					{
						// only print current pool
						if (p.first == pair.first)
						{
							for (auto pg : p.second.pi())
							{
								pg_info_t pit;
								std::vector<int> ol;
								pit.pgid = pg.pgid();
								// std::string osd_str = "";
								std::vector<osd_info_t> osds;
								for (auto osd : pg.osdid())
								{
									ol.push_back(osd);
									osds.push_back(ctx->osdmap.osd_map[osd]);
									// osd_str += std::to_string(osd) + ",";
								}
								// SPDK_NOTICELOG("----- core [%u] pool:%d pg:%d osd list: [%s]--------\r\n", spdk_env_get_current_core(), p.first, pit.pgid, osd_str.c_str());
								pit.osd_list = std::move(ol);
								ctx->pgmap.pool_pg_map[pair.first][pg.pgid()] = std::move(pit);
								ctx->pm->create_partition(p.first, pg.pgid(), std::move(osds), pv[pair.first]);
							}
							break;
						}
					}
				}
				else
				{
					// we receive message from existing pool
					// according to our protocol, when there is a update, map vers
					if (ctx->pgmap.pool_version[pair.first] < pv[pair.first])
					{
						// update pool_version
						ctx->pgmap.pool_version[pair.first] = pv[pair.first];

						// we trust monitor
						ctx->pgmap.pool_pg_map.clear();
						for (auto p : pgs)
						{
							// only print current pool
							if (p.first == pair.first)
							{

								SPDK_NOTICELOG("pgs of pool:%d \r\n", p.first);
								for (auto pg : p.second.pi())
								{
									pg_info_t pit;
									std::vector<int> ol;
									pit.pgid = pg.pgid();
									for (auto osd : pg.osdid())
									{
										ol.push_back(osd);
									}
									pit.osd_list = std::move(ol);
									ctx->pgmap.pool_pg_map[pair.first][pit.pgid] = std::move(pit);
								}
								break;
							}
						}
					}
				}
			}

			return SPDK_POLLER_IDLE;
		}
		else if (resp.has_apply_id_response())
		{
			if (!ctx->is_booted)
			{
				// a osd not booted can't receive this kind of message
				return SPDK_POLLER_IDLE;
			}
		}
		else if (resp.has_boot_response())
		{
			if (ctx->is_booted)
			{
				SPDK_NOTICELOG("a booted osd can't send this kind of message\r\n");
				// a osd not booted can't received this kind of message
				return SPDK_POLLER_IDLE;
			}
			else
			{
				auto boot_resp = resp.mutable_boot_response();
				assert(boot_resp->ok());
				ctx->is_booted = true;
				SPDK_NOTICELOG("osd is booted\r\n");
				return SPDK_POLLER_BUSY;
			}
		}
		else if (resp.has_heartbeat_response())
		{
			if (!ctx->is_booted)
			{
				// a osd not booted can't receive this kind of message
				return SPDK_POLLER_IDLE;
			}
			SPDK_NOTICELOG("got heartbeat response\r\n");
		}
		else if (resp.has_osd_stop_response())
		{
			if (!ctx->is_booted)
			{
				// a osd not booted can't receive this kind of message
				return SPDK_POLLER_IDLE;
			}
			SPDK_NOTICELOG("got osd_stop response\r\n");
		}
		else if (resp.has_list_pools_response())
		{
			if (!ctx->is_booted)
			{
				// a osd not booted can't receive this kind of message
				return SPDK_POLLER_IDLE;
			}
			SPDK_NOTICELOG("got list_pools response\r\n");
		}
		else
		{
			SPDK_NOTICELOG("got unknown response\r\n");
		}
	}

	return SPDK_POLLER_BUSY;
}

static int
fbclient_print_osdmap_and_pgmap(void *arg)
{
    mon_client *ctx = (mon_client *)arg;
	if (!ctx->is_booted)
	{
		// a osd not booted can't send this kind of message
		return SPDK_POLLER_IDLE;
	}

	if (ctx->osdmap.osdmap_version == -1)
	{
		// a osd not booted can't send this kind of message
		return SPDK_POLLER_IDLE;
	}

	for (auto i = 0; i < ctx->osdmap.osd_map.size(); i++)
	{
		SPDK_NOTICELOG("osd %d:  address %s, port %d, isup: %d, isin: %d, ispending_create:%d\r\n",
					   ctx->osdmap.osd_map[i].node_id, ctx->osdmap.osd_map[i].address.c_str(), ctx->osdmap.osd_map[i].port,
					   ctx->osdmap.osd_map[i].isup, ctx->osdmap.osd_map[i].isin, ctx->osdmap.osd_map[i].ispendingcreate);
	}
	SPDK_NOTICELOG("--------------------------------------------------------\r\n");

	for (auto& pair : ctx->pgmap.pool_version)
	{
		SPDK_NOTICELOG("poolid: %d, poolversion: %lx\n", pair.first, pair.second);
	}

	for (auto& pair : ctx->pgmap.pool_pg_map)
	{
		for (auto& pg_map : pair.second)
		{
			auto& pg = pg_map.second;
			std::string osd_str = "";
            for(auto osdid : pg.osd_list){
				osd_str += std::to_string(osdid) + ",";
			}
			// (fixme)use hardcode 3 copies as we usually do it
			SPDK_NOTICELOG("poolid:pgid: %d:%d, osdlist: [%s]\r\n", pair.first, pg.pgid, osd_str.c_str());
		}
	}

	SPDK_NOTICELOG("--------------------------------------------------------\r\n");

	return SPDK_POLLER_BUSY;
}


static int
fbclient_get_osdmap_poll(void *arg)
{
	mon_client *ctx = (mon_client *)arg;
	int rc = 0;
	struct iovec iov;

	if (!ctx->is_running)
	{
		/* EOF */
		SPDK_NOTICELOG("Closing connection...\n");
		ctx->sock_quit(0);
		return SPDK_POLLER_IDLE;
	}
	if (!ctx->is_booted)
	{
		// a osd not booted can't send this kind of message
		return SPDK_POLLER_IDLE;
	}

	msg::Request *req = new msg::Request();
	msg::GetOsdMapRequest osdmap_req;

	osdmap_req.set_currentversion(-1);
	req->set_allocated_get_osdmap_request(&osdmap_req);

	size_t size = req->ByteSizeLong();
	char *buf_out = (char *)malloc(size);

	req->SerializeToArray(buf_out, size);
	iov.iov_base = buf_out;
	iov.iov_len = size;
	rc = spdk_sock_writev(ctx->sock, &iov, 1);
    free(buf_out);
	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
fbclient_get_pgmap_poll(void *arg)
{
	mon_client *ctx = (mon_client *)arg;
	int rc = 0;
	struct iovec iov;

	if (!ctx->is_running)
	{
		/* EOF */
		SPDK_NOTICELOG("Closing connection...\n");
		ctx->sock_quit(0);
		return SPDK_POLLER_IDLE;
	}

	if (!ctx->is_booted)
	{
		// a osd not booted can't send this kind of message
		return SPDK_POLLER_IDLE;
	}
	msg::Request *req = new msg::Request();
	req->mutable_get_pgmap_request();

	msg::GetPgMapRequest pgmap_req;
	req->set_allocated_get_pgmap_request(&pgmap_req);

	// if we do have version
	if (ctx->pgmap.pool_version.size() != 0)
	{
		for (const auto &pair : ctx->pgmap.pool_version)
		{
			(*pgmap_req.mutable_pool_versions())[pair.first] = pair.second;
		}
	}

	size_t size = req->ByteSizeLong();
	char *buf_out = (char *)malloc(size);

	req->SerializeToArray(buf_out, size);
	iov.iov_base = buf_out;
	iov.iov_len = size;
	rc = spdk_sock_writev(ctx->sock, &iov, 1);
    free(buf_out);
	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

int mon_client::send_bootrequest()
{
	int rc = 0;
	struct iovec iov;

	if (!is_running)
	{
		/* EOF */
		SPDK_NOTICELOG("Closing connection...\n");
		sock_quit(0);
		return -1;
	}
	if (is_booted)
	{
		// a osd not booted can't receive this kind of message
		return -1;
	}

	msg::Request *req = new msg::Request();
	req->mutable_boot_request();

	msg::BootRequest br;
	//(fixme)use parsed config file address
	br.set_address("127.0.0.1");
	br.set_address("fbdev");
	//(fixme) use montor port + 1 to avoid conflict
	br.set_port(port + 1);
	//(fixme)use hard coded size
	br.set_size(1024 * 1024);

	/*
	br.set_osd_id(ctx->osd_id);
	struct spdk_uuid id;
	spdk_uuid_generate(&id);
	char uuid_char[SPDK_UUID_STRING_LEN];
	spdk_uuid_fmt_lower(uuid_char, SPDK_UUID_STRING_LEN, &id);
	br.set_uuid(uuid_char);
	*/

	//(fixme)using for debug, remember to remove this
	br.set_osd_id(1);
	br.set_uuid("3e5307a4-e0c2-452d-ab1a-1517c8ce8dda");

	req->set_allocated_boot_request(&br);

	size_t size = req->ByteSizeLong();
	char *buf_out = (char *)malloc(size);

	req->SerializeToArray(buf_out, size);
	iov.iov_base = buf_out;
	iov.iov_len = size;
	rc = spdk_sock_writev(sock, &iov, 1);
    free(buf_out);
	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

int mon_client::connect_mon(){
	int rc;
	char saddr[ADDR_STR_LEN], caddr[ADDR_STR_LEN];
	uint16_t cport, sport;
	struct spdk_sock_opts opts;

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.zcopy = true;

	SPDK_NOTICELOG("Connecting to the server on %s:%d\n", host.c_str(), port);

	sock = spdk_sock_connect_ext(host.c_str(), port, g_impl_name, &opts);
	if (sock == NULL)
	{
		SPDK_ERRLOG("connect error(%d): %s\n", errno, spdk_strerror(errno));
		return -1;
	}

	rc = spdk_sock_getaddr(sock, saddr, sizeof(saddr), &sport, caddr, sizeof(caddr), &cport);
	if (rc < 0)
	{
		SPDK_ERRLOG("Cannot get connection addresses\n");
		spdk_sock_close(&sock);
		return -1;
	}

	SPDK_NOTICELOG("Connection established between me with monitor, monitor address: (%s:%hu), my address: (%s:%hu)\n", caddr, cport, saddr, sport);

	is_running = true;
	poller_in = SPDK_POLLER_REGISTER(fbclient_monitor_rpc_processer, this, 0);
	poller_getosdmap = SPDK_POLLER_REGISTER(fbclient_get_osdmap_poll, this, GET_OSDMAP_US);
	poller_getpgmap = SPDK_POLLER_REGISTER(fbclient_get_pgmap_poll, this, GET_PGMAP_US);
	// poller_printpgmap = SPDK_POLLER_REGISTER(fbclient_print_osdmap_and_pgmap, this, GET_OSDMAP_US);

	send_bootrequest();
	return 0;    
}