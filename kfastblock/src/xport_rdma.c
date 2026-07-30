#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/slab.h>
#include <net/net_namespace.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "kfastblock/xport_rdma.h"

#define KFASTBLOCK_RDMA_CM_TIMEOUT_MS 3000

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
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct completion cm_done;
	enum rdma_cm_event_type cm_event;
	int cm_event_status;
};

static void kfastblock_rdma_conn_destroy_resources(struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return;

	if (conn->cm_id && conn->cm_id->qp) {
		rdma_destroy_qp(conn->cm_id);
	}
	if (conn->cq) {
		ib_destroy_cq(conn->cq);
		conn->cq = NULL;
	}
	if (conn->pd) {
		ib_dealloc_pd(conn->pd);
		conn->pd = NULL;
	}
	if (conn->cm_id) {
		rdma_destroy_id(conn->cm_id);
		conn->cm_id = NULL;
	}
}

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

static int kfastblock_rdma_build_dst_addr(const char *host, u16 port,
					  struct sockaddr_in *dst)
{
	if (!host || !port || !dst)
		return -EINVAL;

	memset(dst, 0, sizeof(*dst));
	dst->sin_family = AF_INET;
	dst->sin_port = htons(port);
	if (in4_pton(host, -1, (u8 *)&dst->sin_addr.s_addr, -1, NULL) != 1)
		return -EINVAL;
	return 0;
}

static int kfastblock_rdma_wait_cm_event(struct kfastblock_rdma_conn *conn,
					 enum rdma_cm_event_type expect)
{
	unsigned long timeout = msecs_to_jiffies(KFASTBLOCK_RDMA_CM_TIMEOUT_MS);

	if (!wait_for_completion_timeout(&conn->cm_done, timeout))
		return -ETIMEDOUT;
	if (conn->cm_event != expect)
		return conn->cm_event_status ? conn->cm_event_status : -ECONNREFUSED;
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

	{
		struct sockaddr_in dst;
		int ret;

		ret = kfastblock_rdma_build_dst_addr(conn->peer_addr,
						     conn->peer_port, &dst);
		if (ret) {
			conn->last_error = ret;
			goto err_destroy_id;
		}

		reinit_completion(&conn->cm_done);
		conn->state = KFASTBLOCK_RDMA_CONN_RESOLVING_ADDR;
		ret = rdma_resolve_addr(conn->cm_id, NULL,
					(struct sockaddr *)&dst,
					KFASTBLOCK_RDMA_CM_TIMEOUT_MS);
		if (ret) {
			conn->last_error = ret;
			goto err_destroy_id;
		}

		ret = kfastblock_rdma_wait_cm_event(
			conn, RDMA_CM_EVENT_ADDR_RESOLVED);
		if (ret) {
			conn->last_error = ret;
			goto err_destroy_id;
		}

		reinit_completion(&conn->cm_done);
		conn->state = KFASTBLOCK_RDMA_CONN_RESOLVING_ROUTE;
		ret = rdma_resolve_route(conn->cm_id,
					 KFASTBLOCK_RDMA_CM_TIMEOUT_MS);
		if (ret) {
			conn->last_error = ret;
			goto err_destroy_id;
		}

		ret = kfastblock_rdma_wait_cm_event(
			conn, RDMA_CM_EVENT_ROUTE_RESOLVED);
		if (ret) {
			conn->last_error = ret;
			goto err_destroy_id;
		}

		conn->pd = ib_alloc_pd(conn->cm_id->device, 0);
		if (IS_ERR(conn->pd)) {
			conn->last_error = PTR_ERR(conn->pd);
			conn->pd = NULL;
			goto err_destroy_id;
		}

		{
			struct ib_cq_init_attr cq_attr = {
				.cqe = 64,
			};

			conn->cq = ib_create_cq(conn->cm_id->device, NULL, NULL,
						conn, &cq_attr);
			if (IS_ERR(conn->cq)) {
				conn->last_error = PTR_ERR(conn->cq);
				conn->cq = NULL;
				goto err_destroy_id;
			}
		}

		{
			struct ib_qp_init_attr qp_attr = {
				.send_cq = conn->cq,
				.recv_cq = conn->cq,
				.cap = {
					.max_send_wr = 32,
					.max_recv_wr = 32,
					.max_send_sge = 1,
					.max_recv_sge = 1,
				},
				.qp_type = IB_QPT_RC,
				.sq_sig_type = IB_SIGNAL_REQ_WR,
			};

			ret = rdma_create_qp(conn->cm_id, conn->pd, &qp_attr);
			if (ret) {
				conn->last_error = ret;
				goto err_destroy_id;
			}
		}

		{
			struct rdma_conn_param conn_param = {
				.responder_resources = 1,
				.initiator_depth = 1,
				.retry_count = 3,
				.rnr_retry_count = 3,
			};

			reinit_completion(&conn->cm_done);
			conn->state = KFASTBLOCK_RDMA_CONN_CONNECTING;
			ret = rdma_connect(conn->cm_id, &conn_param);
			if (ret) {
				conn->last_error = ret;
				goto err_destroy_id;
			}

			ret = kfastblock_rdma_wait_cm_event(
				conn, RDMA_CM_EVENT_ESTABLISHED);
			if (ret) {
				conn->last_error = ret;
				goto err_destroy_id;
			}
		}
	}

	conn->connected = true;
	conn->state = KFASTBLOCK_RDMA_CONN_ESTABLISHED;
	conn->last_error = 0;
	return 0;

err_destroy_id:
	conn->state = KFASTBLOCK_RDMA_CONN_ERROR;
	kfastblock_rdma_conn_destroy_resources(conn);
	return conn->last_error;
}

void kfastblock_rdma_conn_disconnect(struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return;
	conn->state = KFASTBLOCK_RDMA_CONN_DISCONNECTING;
	if (conn->cm_id && conn->connected)
		rdma_disconnect(conn->cm_id);
	kfastblock_rdma_conn_destroy_resources(conn);
	conn->connected = false;
	conn->state = KFASTBLOCK_RDMA_CONN_IDLE;
	conn->last_error = 0;
	conn->peer_port = 0;
	conn->peer_addr[0] = '\0';
}

bool kfastblock_rdma_conn_is_connected(const struct kfastblock_rdma_conn *conn)
{
	return conn && conn->connected &&
	       conn->state == KFASTBLOCK_RDMA_CONN_ESTABLISHED &&
	       conn->cm_id;
}

int kfastblock_rdma_conn_send(struct kfastblock_rdma_conn *conn,
			      const void *buf, u32 len)
{
	if (!kfastblock_rdma_conn_is_connected(conn) || !buf || !len)
		return -EINVAL;
	if (!conn->cm_id->qp || !conn->pd)
		return -ENOTCONN;
	/* Full SEND WR + MR path lands in follow-up commits. */
	return -EOPNOTSUPP;
}

int kfastblock_rdma_conn_recv(struct kfastblock_rdma_conn *conn,
			      void *buf, u32 buf_len)
{
	if (!kfastblock_rdma_conn_is_connected(conn) || !buf || !buf_len)
		return -EINVAL;
	if (!conn->cm_id->qp || !conn->pd)
		return -ENOTCONN;
	/* Full RECV WR + poll path lands in follow-up commits. */
	return -EOPNOTSUPP;
}
