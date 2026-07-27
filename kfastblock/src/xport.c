#include <linux/errno.h>

#include "kfastblock/xport.h"

static int kfastblock_xport_tcp_probe(const struct kfastblock_leader_info *leader)
{
	if (!kfastblock_leader_has_tcp(leader))
		return -EINVAL;
	return 0;
}

/*
 * Cheap RDMA capability probe: only validate that the leader advertises
 * address + rdma_port. Do NOT open an RDMA CM connection here.
 *
 * Why: xport_select() may run on every I/O path decision / meta refresh.
 * Full connect/disconnect would thrash CM, allocate QP/MR, and add multi-
 * second latency on failure. Real I/O still performs a full connect via
 * kfastblock_rdma_conn_connect() (transport / RDMA pool owns that).
 */
static int kfastblock_xport_rdma_probe(const struct kfastblock_leader_info *leader)
{
	if (!kfastblock_leader_has_rdma(leader))
		return -ENOTCONN;
	return 0;
}

static const struct kfastblock_xport_ops kfastblock_xport_tcp = {
	.name = "tcp",
	.transport_id = KFASTBLOCK_OSD_TRANSPORT_TCP,
	.probe = kfastblock_xport_tcp_probe,
};

static const struct kfastblock_xport_ops kfastblock_xport_rdma = {
	.name = "rdma",
	.transport_id = KFASTBLOCK_OSD_TRANSPORT_RDMA,
	.probe = kfastblock_xport_rdma_probe,
};

const struct kfastblock_xport_ops *kfastblock_xport_tcp_ops(void)
{
	return &kfastblock_xport_tcp;
}

const struct kfastblock_xport_ops *kfastblock_xport_rdma_ops(void)
{
	return &kfastblock_xport_rdma;
}

const struct kfastblock_xport_ops *
kfastblock_xport_ops_lookup(u32 transport_id)
{
	switch (transport_id) {
	case KFASTBLOCK_OSD_TRANSPORT_TCP:
		return &kfastblock_xport_tcp;
	case KFASTBLOCK_OSD_TRANSPORT_RDMA:
		return &kfastblock_xport_rdma;
	case KFASTBLOCK_OSD_TRANSPORT_AUTO:
		/* Prefer RDMA when available; caller still falls back. */
		return &kfastblock_xport_rdma;
	default:
		return NULL;
	}
}

const struct kfastblock_xport_ops *
kfastblock_xport_select(u32 preference,
			const struct kfastblock_leader_info *leader)
{
	const struct kfastblock_xport_ops *ops;

	if (!kfastblock_xport_preference_valid(preference))
		preference = KFASTBLOCK_OSD_TRANSPORT_TCP;

	if (preference == KFASTBLOCK_OSD_TRANSPORT_AUTO) {
		ops = &kfastblock_xport_rdma;
		if (ops->probe && !ops->probe(leader))
			return ops;
		return &kfastblock_xport_tcp;
	}

	ops = kfastblock_xport_ops_lookup(preference);
	if (!ops)
		return &kfastblock_xport_tcp;
	return ops;
}
