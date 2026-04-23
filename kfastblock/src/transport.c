#include <linux/errno.h>

#include "kfastblock/transport.h"

int kfastblock_transport_init(void)
{
	return 0;
}

void kfastblock_transport_exit(void)
{
}

int kfastblock_transport_fetch_cluster_view(struct kfastblock_cluster_view *view,
				    const struct kfastblock_attach_spec *spec)
{
	/*
	 * TODO:
	 * 1. talk to monitor over a kernel-friendly control plane
	 * 2. fetch image info
	 * 3. fetch osdmap/pgmap
	 * 4. seed leader hints or equivalent route cache
	 */
	return -EOPNOTSUPP;
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
