#include <linux/errno.h>
#include <linux/slab.h>

#include "kfastblock/xport_rdma.h"

struct kfastblock_rdma_conn {
	char peer_addr[KFASTBLOCK_MAX_ADDR_LEN];
	u16 peer_port;
	bool connected;
};

struct kfastblock_rdma_conn *kfastblock_rdma_conn_alloc(void)
{
	return kzalloc(sizeof(struct kfastblock_rdma_conn), GFP_KERNEL);
}

void kfastblock_rdma_conn_free(struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return;
	kfastblock_rdma_conn_disconnect(conn);
	kfree(conn);
}

int kfastblock_rdma_conn_connect(struct kfastblock_rdma_conn *conn,
				 const struct kfastblock_leader_info *leader)
{
	if (!conn || !leader)
		return -EINVAL;
	if (!leader->address[0] || !leader->rdma_port)
		return -ENOTCONN;

	/* CM/QP wiring lands in follow-up commits. */
	return -EOPNOTSUPP;
}

void kfastblock_rdma_conn_disconnect(struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return;
	conn->connected = false;
	conn->peer_port = 0;
	conn->peer_addr[0] = '\0';
}
