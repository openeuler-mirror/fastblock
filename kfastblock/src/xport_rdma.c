#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <net/net_namespace.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "kfastblock/rawproto.h"
#include "kfastblock/xport_rdma.h"

/* Tunable via module params; defaults match previous hardcodes. */
static unsigned int kfastblock_rdma_cm_timeout_ms = 3000;
static unsigned int kfastblock_rdma_io_timeout_ms = 5000;
/* Client outstanding RECV posts; depth>1 pipelines next response. */
static unsigned int kfastblock_rdma_recv_depth = 2;
#define KFASTBLOCK_RDMA_RECV_DEPTH_MIN 1U
#define KFASTBLOCK_RDMA_RECV_DEPTH_MAX 16U

module_param_named(rdma_cm_timeout_ms, kfastblock_rdma_cm_timeout_ms, uint, 0644);
MODULE_PARM_DESC(rdma_cm_timeout_ms,
		 "RDMA CM address/route/connect timeout in milliseconds");
module_param_named(rdma_io_timeout_ms, kfastblock_rdma_io_timeout_ms, uint, 0644);
MODULE_PARM_DESC(rdma_io_timeout_ms,
		 "RDMA SEND/RECV completion poll timeout in milliseconds");
module_param_named(rdma_recv_depth, kfastblock_rdma_recv_depth, uint, 0644);
MODULE_PARM_DESC(rdma_recv_depth,
		 "Outstanding RDMA RECV posts per connection (1-16)");

static unsigned int kfastblock_rdma_timeout_ms_or_default(unsigned int v,
							 unsigned int def)
{
	if (!v)
		return def;
	if (v > 60000U)
		return 60000U;
	return v;
}

static unsigned int kfastblock_rdma_recv_depth_clamped(void)
{
	unsigned int d = kfastblock_rdma_recv_depth;

	if (d < KFASTBLOCK_RDMA_RECV_DEPTH_MIN)
		return KFASTBLOCK_RDMA_RECV_DEPTH_MIN;
	if (d > KFASTBLOCK_RDMA_RECV_DEPTH_MAX)
		return KFASTBLOCK_RDMA_RECV_DEPTH_MAX;
	return d;
}

static unsigned long kfastblock_rdma_send_ok;
static unsigned long kfastblock_rdma_send_err;
static unsigned long kfastblock_rdma_recv_ok;
static unsigned long kfastblock_rdma_recv_err;
static unsigned long kfastblock_rdma_exchange_ok;
static unsigned long kfastblock_rdma_exchange_err;
static unsigned long kfastblock_rdma_connect_ok;
static unsigned long kfastblock_rdma_connect_err;

module_param_named(rdma_send_ok, kfastblock_rdma_send_ok, ulong, 0444);
MODULE_PARM_DESC(rdma_send_ok, "RDMA SEND successes");
module_param_named(rdma_send_err, kfastblock_rdma_send_err, ulong, 0444);
MODULE_PARM_DESC(rdma_send_err, "RDMA SEND failures");
module_param_named(rdma_recv_ok, kfastblock_rdma_recv_ok, ulong, 0444);
MODULE_PARM_DESC(rdma_recv_ok, "RDMA RECV successes");
module_param_named(rdma_recv_err, kfastblock_rdma_recv_err, ulong, 0444);
MODULE_PARM_DESC(rdma_recv_err, "RDMA RECV failures");
module_param_named(rdma_exchange_ok, kfastblock_rdma_exchange_ok, ulong, 0444);
MODULE_PARM_DESC(rdma_exchange_ok, "RDMA exchange successes");
module_param_named(rdma_exchange_err, kfastblock_rdma_exchange_err, ulong, 0444);
MODULE_PARM_DESC(rdma_exchange_err, "RDMA exchange failures");
module_param_named(rdma_connect_ok, kfastblock_rdma_connect_ok, ulong, 0444);
MODULE_PARM_DESC(rdma_connect_ok, "RDMA connect successes");
module_param_named(rdma_connect_err, kfastblock_rdma_connect_err, ulong, 0444);
MODULE_PARM_DESC(rdma_connect_err, "RDMA connect failures");

/*
 * Connection state machine (client):
 *   IDLE -> RESOLVING_ADDR -> RESOLVING_ROUTE -> CONNECTING -> ESTABLISHED
 *   any of the above -> ERROR on CM/verbs failure
 *   ESTABLISHED -> DISCONNECTING -> IDLE on teardown
 *   ERROR -> IDLE after destroy_resources (reconnect allowed via connect())
 *
 * Poll / I/O must only run in ESTABLISHED. If state leaves ESTABLISHED
 * mid-poll (async CM disconnect, explicit disconnect), return -ENOTCONN
 * and surface via last_error.
 */
enum kfastblock_rdma_conn_state {
	KFASTBLOCK_RDMA_CONN_IDLE = 0,
	KFASTBLOCK_RDMA_CONN_RESOLVING_ADDR,
	KFASTBLOCK_RDMA_CONN_RESOLVING_ROUTE,
	KFASTBLOCK_RDMA_CONN_CONNECTING,
	KFASTBLOCK_RDMA_CONN_ESTABLISHED,
	KFASTBLOCK_RDMA_CONN_ERROR,
	KFASTBLOCK_RDMA_CONN_DISCONNECTING,
};

static const char *__maybe_unused kfastblock_rdma_conn_state_name(u8 state)
{
	switch (state) {
	case KFASTBLOCK_RDMA_CONN_IDLE:
		return "idle";
	case KFASTBLOCK_RDMA_CONN_RESOLVING_ADDR:
		return "resolving_addr";
	case KFASTBLOCK_RDMA_CONN_RESOLVING_ROUTE:
		return "resolving_route";
	case KFASTBLOCK_RDMA_CONN_CONNECTING:
		return "connecting";
	case KFASTBLOCK_RDMA_CONN_ESTABLISHED:
		return "established";
	case KFASTBLOCK_RDMA_CONN_ERROR:
		return "error";
	case KFASTBLOCK_RDMA_CONN_DISCONNECTING:
		return "disconnecting";
	default:
		return "unknown";
	}
}

enum kfastblock_rdma_wr_id {
	KFASTBLOCK_RDMA_WR_SEND = 1,
	/* RECV wr_id base; actual id = base + slot index [0, depth). */
	KFASTBLOCK_RDMA_WR_RECV_BASE = 0x100,
};

static bool kfastblock_rdma_wr_is_recv(u64 wr_id)
{
	return wr_id >= KFASTBLOCK_RDMA_WR_RECV_BASE &&
	       wr_id < (KFASTBLOCK_RDMA_WR_RECV_BASE +
			KFASTBLOCK_RDMA_RECV_DEPTH_MAX);
}

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
	/* Staging buffers for raw SEND/RECV frames. */
	void *send_buf;
	void *recv_buf;
	u32 send_buf_len;
	u32 recv_buf_len;
	u64 send_dma;
	u64 recv_dma;
	bool send_mapped;
	bool recv_mapped;
	bool recv_posted;
	/* Target outstanding RECV posts (clamped at connect). */
	u8 recv_depth;
	/* Currently posted RECV count (0..recv_depth). */
	u8 recv_posted_count;
	/* Next RECV wr_id slot index (rotates in [0, depth_max)). */
	u8 recv_wr_slot;
	struct completion send_done;
	struct completion recv_done;
	int send_wc_status;
	int recv_wc_status;
	u32 recv_byte_len;
};

static void kfastblock_rdma_conn_unmap_bufs(struct kfastblock_rdma_conn *conn)
{
	struct ib_device *dev;

	if (!conn || !conn->cm_id || !conn->cm_id->device)
		return;
	dev = conn->cm_id->device;
	if (conn->send_mapped) {
		ib_dma_unmap_single(dev, conn->send_dma, conn->send_buf_len,
				    DMA_TO_DEVICE);
		conn->send_mapped = false;
		conn->send_dma = 0;
	}
	if (conn->recv_mapped) {
		ib_dma_unmap_single(dev, conn->recv_dma, conn->recv_buf_len,
				    DMA_FROM_DEVICE);
		conn->recv_mapped = false;
		conn->recv_dma = 0;
	}
}

static void kfastblock_rdma_conn_free_bufs(struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return;
	kfastblock_rdma_conn_unmap_bufs(conn);
	kfree(conn->send_buf);
	conn->send_buf = NULL;
	conn->send_buf_len = 0;
	kfree(conn->recv_buf);
	conn->recv_buf = NULL;
	conn->recv_buf_len = 0;
	conn->recv_posted = false;
	conn->recv_posted_count = 0;
	conn->recv_wr_slot = 0;
}

static void kfastblock_rdma_conn_destroy_resources(struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return;

	if (conn->cm_id && conn->cm_id->qp) {
		rdma_destroy_qp(conn->cm_id);
	}
	kfastblock_rdma_conn_free_bufs(conn);
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
	unsigned long timeout = msecs_to_jiffies(kfastblock_rdma_timeout_ms_or_default(kfastblock_rdma_cm_timeout_ms, 3000));

	if (!wait_for_completion_timeout(&conn->cm_done, timeout))
		return -ETIMEDOUT;
	if (conn->cm_event != expect)
		return conn->cm_event_status ? conn->cm_event_status : -ECONNREFUSED;
	return 0;
}

static int kfastblock_rdma_alloc_bufs(struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return -EINVAL;
	if (conn->send_buf && conn->recv_buf)
		return 0;

	conn->send_buf = kzalloc(KFASTBLOCK_RDMA_BUF_LEN, GFP_KERNEL);
	if (!conn->send_buf)
		return -ENOMEM;
	conn->send_buf_len = KFASTBLOCK_RDMA_BUF_LEN;

	conn->recv_buf = kzalloc(KFASTBLOCK_RDMA_BUF_LEN, GFP_KERNEL);
	if (!conn->recv_buf) {
		kfree(conn->send_buf);
		conn->send_buf = NULL;
		conn->send_buf_len = 0;
		return -ENOMEM;
	}
	conn->recv_buf_len = KFASTBLOCK_RDMA_BUF_LEN;
	return 0;
}

static int kfastblock_rdma_map_bufs(struct kfastblock_rdma_conn *conn)
{
	struct ib_device *dev;

	if (!conn || !conn->cm_id || !conn->cm_id->device)
		return -EINVAL;
	if (!conn->send_buf || !conn->recv_buf)
		return -ENOMEM;
	dev = conn->cm_id->device;

	if (!conn->send_mapped) {
		conn->send_dma = ib_dma_map_single(dev, conn->send_buf,
						   conn->send_buf_len,
						   DMA_TO_DEVICE);
		if (ib_dma_mapping_error(dev, conn->send_dma))
			return -EIO;
		conn->send_mapped = true;
	}
	if (!conn->recv_mapped) {
		conn->recv_dma = ib_dma_map_single(dev, conn->recv_buf,
						   conn->recv_buf_len,
						   DMA_FROM_DEVICE);
		if (ib_dma_mapping_error(dev, conn->recv_dma)) {
			kfastblock_rdma_conn_unmap_bufs(conn);
			return -EIO;
		}
		conn->recv_mapped = true;
	}
	return 0;
}

static int kfastblock_rdma_post_recv(struct kfastblock_rdma_conn *conn)
{
	struct ib_sge sge;
	struct ib_recv_wr wr;
	const struct ib_recv_wr *bad;
	int ret;
	u8 slot;

	if (!conn || !conn->cm_id || !conn->cm_id->qp || !conn->pd ||
	    !conn->recv_mapped)
		return -ENOTCONN;
	if (!conn->recv_depth)
		conn->recv_depth = 1;
	if (conn->recv_posted_count >= conn->recv_depth)
		return 0;

	/* Rotating slot tags wr_id so multi-depth CQ entries stay unique. */
	slot = conn->recv_wr_slot % KFASTBLOCK_RDMA_RECV_DEPTH_MAX;
	memset(&sge, 0, sizeof(sge));
	sge.addr = conn->recv_dma;
	sge.length = conn->recv_buf_len;
	sge.lkey = conn->pd->local_dma_lkey;

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = KFASTBLOCK_RDMA_WR_RECV_BASE + slot;
	wr.sg_list = &sge;
	wr.num_sge = 1;

	ret = ib_post_recv(conn->cm_id->qp, &wr, &bad);
	if (ret)
		return ret;
	if (!conn->recv_posted) {
		reinit_completion(&conn->recv_done);
		conn->recv_wc_status = 0;
		conn->recv_byte_len = 0;
	}
	conn->recv_posted = true;
	conn->recv_posted_count++;
	conn->recv_wr_slot = (u8)((slot + 1) % KFASTBLOCK_RDMA_RECV_DEPTH_MAX);
	return 0;
}

/* Post RECV WRs until outstanding count reaches conn->recv_depth. */
static int kfastblock_rdma_post_recv_fill(struct kfastblock_rdma_conn *conn)
{
	int ret;

	if (!conn)
		return -EINVAL;
	if (!conn->recv_depth)
		conn->recv_depth = (u8)kfastblock_rdma_recv_depth_clamped();
	while (conn->recv_posted_count < conn->recv_depth) {
		ret = kfastblock_rdma_post_recv(conn);
		if (ret)
			return ret;
	}
	return 0;
}

static int kfastblock_rdma_poll_one(struct kfastblock_rdma_conn *conn,
				    unsigned long deadline)
{
	struct ib_wc wc;
	int n;

	if (!conn || !conn->cq)
		return -EINVAL;

	while (time_before(jiffies, deadline)) {
		/* Disconnect during poll: stop spinning and surface ENOTCONN. */
		if (conn->state != KFASTBLOCK_RDMA_CONN_ESTABLISHED ||
		    !conn->connected) {
			conn->last_error = -ENOTCONN;
			return -ENOTCONN;
		}
		if (!conn->cq)
			return -ENOTCONN;

		n = ib_poll_cq(conn->cq, 1, &wc);
		if (n < 0) {
			conn->last_error = n;
			conn->state = KFASTBLOCK_RDMA_CONN_ERROR;
			return n;
		}
		if (n == 0) {
			cpu_relax();
			continue;
		}

		/* Non-success WC: still deliver to waiter; caller checks status. */
		if (wc.status != IB_WC_SUCCESS) {
			conn->last_error = -EIO;
			/* Fatal CQ errors tear down usability of this conn. */
			if (wc.status == IB_WC_WR_FLUSH_ERR ||
			    wc.status == IB_WC_RETRY_EXC_ERR ||
			    wc.status == IB_WC_RESP_TIMEOUT_ERR ||
			    wc.status == IB_WC_FATAL_ERR) {
				conn->connected = false;
				conn->state = KFASTBLOCK_RDMA_CONN_ERROR;
			}
		}

		if (wc.wr_id == KFASTBLOCK_RDMA_WR_SEND) {
			conn->send_wc_status = wc.status;
			complete(&conn->send_done);
			return 0;
		}
		if (kfastblock_rdma_wr_is_recv(wc.wr_id)) {
			conn->recv_wc_status = wc.status;
			conn->recv_byte_len = wc.byte_len;
			if (conn->recv_posted_count)
				conn->recv_posted_count--;
			conn->recv_posted = conn->recv_posted_count > 0;
			complete(&conn->recv_done);
			return 0;
		}
		/* Unknown wr_id: ignore and keep polling. */
	}
	conn->last_error = -ETIMEDOUT;
	return -ETIMEDOUT;
}

struct kfastblock_rdma_conn *kfastblock_rdma_conn_alloc(void)
{
	struct kfastblock_rdma_conn *conn;

	conn = kzalloc(sizeof(*conn), GFP_KERNEL);
	if (!conn)
		return NULL;
	init_completion(&conn->cm_done);
	init_completion(&conn->send_done);
	init_completion(&conn->recv_done);
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
					kfastblock_rdma_timeout_ms_or_default(
						kfastblock_rdma_cm_timeout_ms,
						3000));
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
					 kfastblock_rdma_timeout_ms_or_default(
						 kfastblock_rdma_cm_timeout_ms,
						 3000));
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

		ret = kfastblock_rdma_alloc_bufs(conn);
		if (ret) {
			conn->last_error = ret;
			goto err_destroy_id;
		}
		ret = kfastblock_rdma_map_bufs(conn);
		if (ret) {
			conn->last_error = ret;
			goto err_destroy_id;
		}
		conn->recv_depth = (u8)kfastblock_rdma_recv_depth_clamped();
		conn->recv_posted_count = 0;
		conn->recv_wr_slot = 0;
		ret = kfastblock_rdma_post_recv_fill(conn);
		if (ret) {
			conn->last_error = ret;
			goto err_destroy_id;
		}
	}

	conn->connected = true;
	conn->state = KFASTBLOCK_RDMA_CONN_ESTABLISHED;
	conn->last_error = 0;
	kfastblock_rdma_connect_ok++;
	return 0;

err_destroy_id:
	conn->state = KFASTBLOCK_RDMA_CONN_ERROR;
	kfastblock_rdma_conn_destroy_resources(conn);
	kfastblock_rdma_connect_err++;
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
	struct ib_sge sge;
	struct ib_send_wr wr;
	const struct ib_send_wr *bad;
	struct ib_device *dev;
	unsigned long deadline;
	int ret;

	if (!kfastblock_rdma_conn_is_connected(conn) || !buf || !len)
		return -EINVAL;
	if (!conn->cm_id->qp || !conn->pd || !conn->send_mapped)
		return -ENOTCONN;
	if (len > conn->send_buf_len)
		return -EMSGSIZE;

	dev = conn->cm_id->device;
	memcpy(conn->send_buf, buf, len);
	/* CPU wrote staging buffer; sync for device. */
	ib_dma_sync_single_for_device(dev, conn->send_dma, len, DMA_TO_DEVICE);

	memset(&sge, 0, sizeof(sge));
	sge.addr = conn->send_dma;
	sge.length = len;
	sge.lkey = conn->pd->local_dma_lkey;

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = KFASTBLOCK_RDMA_WR_SEND;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode = IB_WR_SEND;
	wr.send_flags = IB_SEND_SIGNALED;

	reinit_completion(&conn->send_done);
	conn->send_wc_status = 0;
	ret = ib_post_send(conn->cm_id->qp, &wr, &bad);
	if (ret) {
		conn->last_error = ret;
		kfastblock_rdma_send_err++;
		return ret;
	}

	deadline = jiffies + msecs_to_jiffies(kfastblock_rdma_timeout_ms_or_default(kfastblock_rdma_io_timeout_ms, 5000));
	while (!completion_done(&conn->send_done)) {
		ret = kfastblock_rdma_poll_one(conn, deadline);
		if (ret) {
			kfastblock_rdma_send_err++;
			return ret;
		}
	}
	if (conn->send_wc_status != IB_WC_SUCCESS) {
		conn->last_error = -EIO;
		kfastblock_rdma_send_err++;
		return -EIO;
	}
	kfastblock_rdma_send_ok++;
	return 0;
}

int kfastblock_rdma_conn_recv(struct kfastblock_rdma_conn *conn,
			      void *buf, u32 buf_len)
{
	struct ib_device *dev;
	unsigned long deadline;
	int ret;

	if (!kfastblock_rdma_conn_is_connected(conn) || !buf || !buf_len)
		return -EINVAL;
	if (!conn->cm_id->qp || !conn->pd || !conn->recv_mapped)
		return -ENOTCONN;

	dev = conn->cm_id->device;
	if (!conn->recv_posted || !conn->recv_posted_count) {
		ret = kfastblock_rdma_post_recv_fill(conn);
		if (ret) {
			conn->last_error = ret;
			return ret;
		}
	}

	deadline = jiffies + msecs_to_jiffies(kfastblock_rdma_timeout_ms_or_default(kfastblock_rdma_io_timeout_ms, 5000));
	while (!completion_done(&conn->recv_done)) {
		ret = kfastblock_rdma_poll_one(conn, deadline);
		if (ret)
			return ret;
	}
	if (conn->recv_wc_status != IB_WC_SUCCESS) {
		conn->last_error = -EIO;
		kfastblock_rdma_recv_err++;
		return -EIO;
	}
	if (conn->recv_byte_len > buf_len) {
		kfastblock_rdma_recv_err++;
		return -EMSGSIZE;
	}

	{
		u32 got = conn->recv_byte_len;

		ib_dma_sync_single_for_cpu(dev, conn->recv_dma, got,
					   DMA_FROM_DEVICE);
		memcpy(buf, conn->recv_buf, got);

		/* Keep outstanding RECV depth filled for the next cycle. */
		ret = kfastblock_rdma_post_recv_fill(conn);
		if (ret) {
			conn->last_error = ret;
			kfastblock_rdma_recv_err++;
			return ret;
		}
		kfastblock_rdma_recv_ok++;
		return (int)got;
	}
}

int kfastblock_rdma_conn_exchange(struct kfastblock_rdma_conn *conn,
				  const void *req, u32 req_len,
				  void *rsp, u32 rsp_cap, u64 expect_seq)
{
	const struct kfastblock_raw_header *rhdr;
	struct kfastblock_raw_header *shdr;
	int ret;

	if (!kfastblock_rdma_conn_is_connected(conn) || !req || !req_len ||
	    !rsp || !rsp_cap)
		return -EINVAL;
	if (req_len < sizeof(struct kfastblock_raw_header))
		return -EINVAL;

	rhdr = req;
	if (le64_to_cpu(rhdr->seq) != expect_seq)
		return -EINVAL;

	ret = kfastblock_rdma_conn_send(conn, req, req_len);
	if (ret) {
		kfastblock_rdma_exchange_err++;
		return ret;
	}

	ret = kfastblock_rdma_conn_recv(conn, rsp, rsp_cap);
	if (ret < 0) {
		kfastblock_rdma_exchange_err++;
		return ret;
	}
	if ((u32)ret < sizeof(struct kfastblock_raw_header)) {
		kfastblock_rdma_exchange_err++;
		return -EPROTO;
	}

	shdr = rsp;
	if (le32_to_cpu(shdr->magic) != KFASTBLOCK_RAW_MAGIC ||
	    !(le32_to_cpu(shdr->flags) & KFASTBLOCK_RAW_FLAG_RESPONSE) ||
	    le64_to_cpu(shdr->seq) != expect_seq ||
	    shdr->opcode != rhdr->opcode ||
	    shdr->service != rhdr->service) {
		kfastblock_rdma_exchange_err++;
		conn->last_error = -EPROTO;
		return -EPROTO;
	}

	kfastblock_rdma_exchange_ok++;
	conn->last_error = 0;
	return ret;
}

const char *kfastblock_rdma_conn_peer_addr(const struct kfastblock_rdma_conn *conn)
{
	return conn ? conn->peer_addr : "";
}

u16 kfastblock_rdma_conn_peer_port(const struct kfastblock_rdma_conn *conn)
{
	return conn ? conn->peer_port : 0;
}

int kfastblock_rdma_conn_last_error(const struct kfastblock_rdma_conn *conn)
{
	return conn ? conn->last_error : -EINVAL;
}

const char *kfastblock_rdma_conn_state_str(const struct kfastblock_rdma_conn *conn)
{
	if (!conn)
		return "null";
	return kfastblock_rdma_conn_state_name(conn->state);
}
