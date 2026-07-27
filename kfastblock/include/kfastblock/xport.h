#ifndef KFASTBLOCK_XPORT_H
#define KFASTBLOCK_XPORT_H

#include <linux/types.h>

#include "kfastblock/common.h"
#include "kfastblock/meta.h"

/*
 * OSD data-plane transport backend.
 * Monitor control-plane is always raw TCP and is out of scope here.
 */
struct kfastblock_xport_ops {
	const char *name;
	u32 transport_id; /* KFASTBLOCK_OSD_TRANSPORT_* */
	/* Return 0 if this backend can talk to leader endpoint. */
	int (*probe)(const struct kfastblock_leader_info *leader);
};

const struct kfastblock_xport_ops *
kfastblock_xport_ops_lookup(u32 transport_id);
const struct kfastblock_xport_ops *kfastblock_xport_tcp_ops(void);
const struct kfastblock_xport_ops *kfastblock_xport_rdma_ops(void);

/*
 * Choose a backend for one OSD endpoint according to attach preference.
 * AUTO prefers RDMA when probe succeeds, otherwise TCP.
 */
const struct kfastblock_xport_ops *
kfastblock_xport_select(u32 preference,
			const struct kfastblock_leader_info *leader);

#endif
