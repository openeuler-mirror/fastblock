#ifndef KFASTBLOCK_TRANSPORT_H
#define KFASTBLOCK_TRANSPORT_H

#include <linux/in.h>
#include <linux/socket.h>
#include <linux/types.h>

#include "kfastblock/control.h"
#include "kfastblock/meta.h"
#include "kfastblock/request.h"

int kfastblock_transport_init(void);
void kfastblock_transport_exit(void);
int kfastblock_transport_fetch_cluster_view(struct kfastblock_cluster_view *view,
				    const struct kfastblock_attach_spec *spec);
int kfastblock_transport_fetch_image(struct kfastblock_cluster_view *view,
				     const struct kfastblock_attach_spec *spec);
int kfastblock_transport_fetch_cluster_map(struct kfastblock_cluster_view *view,
				   const struct kfastblock_attach_spec *spec);
int kfastblock_transport_refresh_image_volume(struct kfastblock_volume *vol);
int kfastblock_transport_refresh_cluster_map_volume(struct kfastblock_volume *vol);
int kfastblock_transport_get_pg_leader(struct kfastblock_volume *vol,
				       u32 pool_id, u32 pg_id,
				       struct kfastblock_leader_info *leader);
int kfastblock_transport_submit(struct kfastblock_request *kf_req);
int kfastblock_transport_sockaddr_from_endpoint(
	const struct kfastblock_monitor_endpoint *endpoint,
	struct sockaddr_in *addr);
int kfastblock_transport_try_connect_monitor(
	const struct kfastblock_monitor_endpoint *endpoint,
	struct socket **sock);

#endif
