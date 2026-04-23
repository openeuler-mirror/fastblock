#include <linux/errno.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/net.h>
#include <linux/socket.h>

#include "kfastblock/common.h"
#include "kfastblock/transport.h"

int kfastblock_transport_init(void)
{
	return 0;
}

void kfastblock_transport_exit(void)
{
}

int kfastblock_transport_sockaddr_from_endpoint(
	const struct kfastblock_monitor_endpoint *endpoint,
	struct sockaddr_in *addr)
{
	int ret;

	if (!endpoint || !addr || !endpoint->host[0] || !endpoint->port)
		return -EINVAL;

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(endpoint->port);
	ret = in4_pton(endpoint->host, -1, (u8 *)&addr->sin_addr.s_addr,
		       -1, NULL);
	if (!ret)
		return -EINVAL;

	return 0;
}

int kfastblock_transport_try_connect_monitor(
	const struct kfastblock_monitor_endpoint *endpoint,
	struct socket **sock)
{
	struct sockaddr_in addr;
	struct socket *local_sock;
	int ret;

	if (!sock)
		return -EINVAL;

	ret = kfastblock_transport_sockaddr_from_endpoint(endpoint, &addr);
	if (ret)
		return ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &local_sock);
	if (ret)
		return ret;

	ret = kernel_connect(local_sock, (struct sockaddr *)&addr, sizeof(addr),
		     O_NONBLOCK);
	if (ret == -EINPROGRESS || ret == -EALREADY)
		ret = 0;
	if (ret) {
		sock_release(local_sock);
		return ret;
	}

	*sock = local_sock;
	return 0;
}

int kfastblock_transport_fetch_cluster_view(struct kfastblock_cluster_view *view,
				    const struct kfastblock_attach_spec *spec)
{
	u32 i;
	int first_err = -EOPNOTSUPP;

	if (!view || !spec || !spec->nr_monitors)
		return -EINVAL;

	for (i = 0; i < spec->nr_monitors; ++i) {
		struct socket *sock = NULL;
		int ret;

		ret = kfastblock_transport_try_connect_monitor(&spec->monitors[i],
					      &sock);
		if (ret) {
			if (first_err == -EOPNOTSUPP)
				first_err = ret;
			continue;
		}

		/*
		 * TODO:
		 * 1. encode a kernel-friendly monitor request
		 * 2. fetch image info
		 * 3. fetch osdmap/pgmap
		 * 4. fill cluster view and leader hints
		 */
		sock_release(sock);
		return -EOPNOTSUPP;
	}

	return first_err;
}

int kfastblock_transport_submit(struct kfastblock_request *kf_req)
{
	/*
	 * TODO:
	 * 1. serialize parent request into per-object OSD requests
	 * 2. route each object by pg/leader
	 * 3. aggregate sub-request completions back into the blk request
	 */
	return -EOPNOTSUPP;
}
