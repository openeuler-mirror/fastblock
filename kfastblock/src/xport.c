#include <linux/errno.h>

#include "kfastblock/xport.h"

static int kfastblock_xport_tcp_probe(const struct kfastblock_leader_info *leader)
{
	if (!leader || !leader->address[0] || !leader->port)
		return -EINVAL;
	return 0;
}

static int kfastblock_xport_rdma_probe(const struct kfastblock_leader_info *leader)
{
	if (!leader || !leader->address[0] || !leader->rdma_port)
		return -ENOTCONN;
	/* RDMA backend is not implemented yet. */
	return -EOPNOTSUPP;
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
