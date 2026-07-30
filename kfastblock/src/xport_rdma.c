#include <linux/errno.h>
#include <linux/slab.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "kfastblock/xport_rdma.h"

enum kfastblock_rdma_conn_state {
	KFASTBLOCK_RDMA_CONN_IDLE = 0,
	KFASTBLOCK_RDMA_CONN_RESOLVING_ADDR,
	KFASTBLOCK_RDMA_CONN_RESOLVING_ROUTE,
	KFASTBLOCK_RDMA_CONN_CONNECTING,
	KFASTBLOCK_RDMA_CONN_ESTABLISHED,
	KFASTBLOCK_RDMA_CONN_ERROR,
	KFASTBLOCK_RDMA_CONN_DISCONNECTING,
};

struct kfastblock_rdma_conn {
	char peer_addr[KFASTBLOCK_MAX_ADDR_LEN];
	u16 peer_port;
	u8 state;
	bool connected;
	int last_error;
	struct rdma_cm_id *cm_id;
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
	if (conn->state != KFASTBLOCK_RDMA_CONN_IDLE &&
	    conn->state != KFASTBLOCK_RDMA_CONN_ERROR)
		return -EBUSY;

	strscpy(conn->peer_addr, leader->address, sizeof(conn->peer_addr));
	conn->peer_port = leader->rdma_port;
	conn->connected = false;
	conn->last_error = -EOPNOTSUPP;
	conn->state = KFASTBLOCK_RDMA_CONN_ERROR;

	/* CM/QP wiring lands in follow-up commits. */
	return conn->last_error;
}

void kfastblock_rdma_conn_disconnect(struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return;
	conn->connected = false;
	conn->state = KFASTBLOCK_RDMA_CONN_IDLE;
	conn->last_error = 0;
	conn->peer_port = 0;
	conn->peer_addr[0] = '\0';
}
