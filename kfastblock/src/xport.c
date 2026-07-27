#include <linux/errno.h>

#include "kfastblock/xport.h"
#include "kfastblock/xport_rdma.h"

static int kfastblock_xport_tcp_probe(const struct kfastblock_leader_info *leader)
{
	if (!leader || !leader->address[0] || !leader->port)
		return -EINVAL;
	return 0;
}

static int kfastblock_xport_rdma_probe(const struct kfastblock_leader_info *leader)
{
	struct kfastblock_rdma_conn *conn;
	int ret;

	if (!leader || !leader->address[0] || !leader->rdma_port)
		return -ENOTCONN;

	conn = kfastblock_rdma_conn_alloc();
	if (!conn)
		return -ENOMEM;
	ret = kfastblock_rdma_conn_connect(conn, leader);
	kfastblock_rdma_conn_free(conn);
	return ret;
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
