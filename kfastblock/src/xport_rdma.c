#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <net/net_namespace.h>

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
	struct completion cm_done;
	enum rdma_cm_event_type cm_event;
	int cm_event_status;
};

static int kfastblock_rdma_cm_event_handler(struct rdma_cm_id *cm_id,
					    struct rdma_cm_event *event)
{
	struct kfastblock_rdma_conn *conn;

	if (!cm_id || !event)
		return 0;

	conn = cm_id->context;
	if (!conn)
		return 0;

	conn->cm_event = event->event;
	conn->cm_event_status = event->status;
	complete(&conn->cm_done);
	return 0;
}

struct kfastblock_rdma_conn *kfastblock_rdma_conn_alloc(void)
{
	struct kfastblock_rdma_conn *conn;

	conn = kzalloc(sizeof(*conn), GFP_KERNEL);
	if (!conn)
		return NULL;
	init_completion(&conn->cm_done);
	return conn;
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
	conn->last_error = 0;

	if (conn->cm_id)
		return -EBUSY;

	conn->cm_id = rdma_create_id(&init_net, kfastblock_rdma_cm_event_handler,
				     conn, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(conn->cm_id)) {
		conn->last_error = PTR_ERR(conn->cm_id);
		conn->cm_id = NULL;
		conn->state = KFASTBLOCK_RDMA_CONN_ERROR;
		return conn->last_error;
	}

	/* Address resolve lands in follow-up commits. */
	conn->last_error = -EOPNOTSUPP;
	conn->state = KFASTBLOCK_RDMA_CONN_ERROR;
	rdma_destroy_id(conn->cm_id);
	conn->cm_id = NULL;
	return conn->last_error;
}

void kfastblock_rdma_conn_disconnect(struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return;
	if (conn->cm_id) {
		rdma_destroy_id(conn->cm_id);
		conn->cm_id = NULL;
	}
	conn->connected = false;
	conn->state = KFASTBLOCK_RDMA_CONN_IDLE;
	conn->last_error = 0;
	conn->peer_port = 0;
	conn->peer_addr[0] = '\0';
}
