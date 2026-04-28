#include <linux/byteorder/little_endian.h>
#include <linux/blk-mq.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/kernel.h>
#include <linux/net.h>
#include <linux/highmem.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/workqueue.h>
#include <linux/moduleparam.h>
#include <net/sock.h>

#include "kfastblock/common.h"
#include "kfastblock/buffer.h"
#include "kfastblock/connpool.h"
#include "kfastblock/fault.h"
#include "kfastblock/rawproto.h"
#include "kfastblock/recovery.h"
#include "kfastblock/scheduler.h"
#include "kfastblock/transport.h"
#include "kfastblock/volume.h"

#define KFASTBLOCK_OBJECT_IO_MAX_ATTEMPTS 2

static int kfastblock_transport_try_connect_host(const char *host,
						 u16 port,
						 struct socket **sock);
static void kfastblock_transport_object_work(struct work_struct *work);
static int kfastblock_transport_query_pg_leader(
	struct kfastblock_request *kf_req,
	struct kfastblock_request_pg_hint *hint,
	struct kfastblock_leader_info *leader);
static int kfastblock_transport_check_osd_backoff(
	struct kfastblock_volume *vol,
	const struct kfastblock_leader_info *leader);
static struct kfastblock_cached_socket *
kfastblock_transport_reserve_osd_slot(struct kfastblock_volume *vol);
static struct workqueue_struct *g_kfastblock_transport_wq;
static unsigned int g_kfastblock_osd_endpoint_parallel_limit = 1;
static bool g_kfastblock_osd_endpoint_parallel_trace;

module_param_named(osd_endpoint_parallel_limit,
		   g_kfastblock_osd_endpoint_parallel_limit,
		   uint, 0644);
MODULE_PARM_DESC(osd_endpoint_parallel_limit,
		 "maximum active raw TCP sockets allowed per OSD endpoint");
module_param_named(osd_endpoint_parallel_trace,
		   g_kfastblock_osd_endpoint_parallel_trace,
		   bool, 0644);
MODULE_PARM_DESC(osd_endpoint_parallel_trace,
		 "enable ratelimited logging for OSD endpoint parallel decisions");

enum kfastblock_transport_buffer_flags {
	KFASTBLOCK_TRANSPORT_BUFFER_ZERO = 1U << 0,
	KFASTBLOCK_TRANSPORT_BUFFER_LARGE = 1U << 1,
};

struct kfastblock_transport_body_part {
	const void *buf;
	u32 len;
};

struct kfastblock_transport_exchange_ctx {
	struct kfastblock_request *kf_req;
	unsigned int object_index;
	struct kfastblock_pipeline_entry *pipeline_entry;
	u8 service;
	u8 opcode;
	u64 seq;
};

struct kfastblock_transport_response_ctx {
	struct kfastblock_raw_header hdr;
	void *body;
	u32 body_len;
	int ret;
};

struct kfastblock_transport_object_io_ctx {
	struct kfastblock_request *kf_req;
	struct kfastblock_volume *vol;
	const struct kfastblock_object_extent *extent;
	struct kfastblock_request_pg_hint *hint;
	struct kfastblock_leader_info leader;
	struct kfastblock_cached_socket *cached;
	struct socket *sock;
	void *buf;
	unsigned int object_index;
	unsigned int attempt;
	unsigned int actions;
	int ret;
	enum req_op op;
	u8 raw_opcode;
	struct kfastblock_transport_exchange_ctx exchange;
	struct kfastblock_transport_response_ctx response;
};

struct kfastblock_transport_submit_ctx {
	struct kfastblock_request *kf_req;
	struct kfastblock_request_dispatch_batch batch;
	unsigned int initial_dispatch;
	int ret;
	bool finished;
};

enum kfastblock_transport_object_attempt_failure_kind {
	KFASTBLOCK_OBJECT_ATTEMPT_FAIL_BEFORE_EXCHANGE = 0,
	KFASTBLOCK_OBJECT_ATTEMPT_FAIL_AFTER_EXCHANGE = 1,
};

static int kfastblock_transport_copy_from_body(const u8 *body,
					       size_t body_len,
					       size_t *offset,
					       void *dst,
					       size_t len);
static int kfastblock_transport_decode_osd_entries(
	const u8 *body,
	size_t body_len,
	size_t *offset,
	struct kfastblock_osd_endpoint *osds,
	u32 osd_count);
static int kfastblock_transport_decode_pg_entries(
	const u8 *body,
	size_t body_len,
	size_t *offset,
	struct kfastblock_pg_route *routes,
	u32 route_count,
	u32 pool_id,
	u32 *pool_pg_count);

static void kfastblock_transport_trace_osd_endpoint_decision(
	struct kfastblock_volume *vol,
	const struct kfastblock_leader_info *leader,
	const char *decision,
	u32 active_count,
	u32 endpoint_parallel_limit)
{
	if (!READ_ONCE(g_kfastblock_osd_endpoint_parallel_trace) ||
	    !vol || !leader || !decision)
		return;

	dev_info_ratelimited(&vol->dev,
			     "osd endpoint decision=%s osd_id=%u addr=%s port=%u active=%u limit=%u\n",
			     decision, leader->osd_id, leader->address,
			     leader->port, active_count, endpoint_parallel_limit);
}

static int kfastblock_transport_maybe_inject_fault(
	struct kfastblock_volume *vol,
	u32 site)
{
	int ret = 0;

	if (!vol)
		return 0;
	if (!kfastblock_fault_injection_should_fail(&vol->fault_injection,
						    site, &ret))
		return 0;

	kfastblock_volume_account_fault_injection(vol, site, ret);
	return ret ? ret : -EIO;
}

static void *kfastblock_transport_alloc_buffer(size_t size, gfp_t gfp,
					       unsigned int flags)
{
	if (!size)
		return NULL;
	if (flags & KFASTBLOCK_TRANSPORT_BUFFER_LARGE) {
		if (flags & KFASTBLOCK_TRANSPORT_BUFFER_ZERO)
			return kvzalloc(size, gfp);
		return kvmalloc(size, gfp);
	}
	if (flags & KFASTBLOCK_TRANSPORT_BUFFER_ZERO)
		return kzalloc(size, gfp);
	return kmalloc(size, gfp);
}

static void kfastblock_transport_free_buffer(void *buf, unsigned int flags)
{
	if (!buf)
		return;
	if (flags & KFASTBLOCK_TRANSPORT_BUFFER_LARGE) {
		kvfree(buf);
		return;
	}
	kfree(buf);
}

static void kfastblock_transport_close_cached_socket_locked(
	struct kfastblock_cached_socket *cached)
{
	kfastblock_osd_conn_slot_close_locked(cached);
}

static void kfastblock_transport_close_cached_monitor_socket_locked(
	struct kfastblock_cached_monitor_socket *cached)
{
	kfastblock_monitor_conn_slot_close_locked(cached);
}

static bool kfastblock_transport_monitor_socket_matches_locked(
	const struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint)
{
	return kfastblock_monitor_conn_slot_matches_locked(cached, endpoint);
}

static u64 kfastblock_transport_next_seq(struct kfastblock_cached_socket *cached)
{
	return kfastblock_osd_conn_slot_next_seq_locked(cached);
}

static u64 kfastblock_transport_next_monitor_seq(
	struct kfastblock_cached_monitor_socket *cached)
{
	return kfastblock_monitor_conn_slot_next_seq_locked(cached);
}

static void kfastblock_transport_mark_osd_socket_failure(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	const int ret)
{
	unsigned long backoff_jiffies;

	if (!cached || !leader)
		return;

	backoff_jiffies = kfastblock_osd_conn_slot_mark_failure_locked(cached,
							       leader, ret);
	kfastblock_volume_account_socket_backoff(vol, false, leader->osd_id,
				leader->port, cached->fail_streak, backoff_jiffies, ret);
}

static void kfastblock_transport_mark_monitor_socket_failure(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint,
	const int ret)
{
	unsigned long backoff_jiffies;

	if (!cached || !endpoint)
		return;

	backoff_jiffies = kfastblock_monitor_conn_slot_mark_failure_locked(
		cached, endpoint, ret);
	kfastblock_volume_account_socket_backoff(vol, true, 0, endpoint->port,
				cached->fail_streak, backoff_jiffies, ret);
}

static void kfastblock_transport_mark_osd_socket_success(
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader)
{
	kfastblock_osd_conn_slot_mark_success_locked(cached, leader);
}

static void kfastblock_transport_mark_monitor_socket_success(
	struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint)
{
	kfastblock_monitor_conn_slot_mark_success_locked(cached, endpoint);
}

static int kfastblock_transport_connect_osd_cached_locked(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	struct socket **sock_out)
{
	int ret;

	if (!cached || !leader || !sock_out)
		return -EINVAL;

	kfastblock_transport_close_cached_socket_locked(cached);
	ret = kfastblock_transport_maybe_inject_fault(
		vol, KFASTBLOCK_FAULT_OSD_CONNECT);
	if (ret) {
		kfastblock_transport_mark_osd_socket_failure(vol, cached, leader, ret);
		cached->connecting = false;
		mutex_unlock(&cached->lock);
		return ret;
	}
	ret = kfastblock_transport_try_connect_host(leader->address, leader->port,
					 sock_out);
	if (ret) {
		kfastblock_transport_mark_osd_socket_failure(vol, cached, leader, ret);
		cached->connecting = false;
		mutex_unlock(&cached->lock);
		return ret;
	}

	cached->sock = *sock_out;
	cached->next_seq = 1;
	cached->connecting = false;
	kfastblock_transport_mark_osd_socket_success(cached, leader);
	return 0;
}

static int kfastblock_transport_get_monitor_socket(
	struct kfastblock_volume *vol,
	const struct kfastblock_attach_spec *spec,
	u32 endpoint_index,
	struct kfastblock_cached_monitor_socket **cached_out,
	struct socket **sock_out)
{
	const struct kfastblock_monitor_endpoint *endpoint;
	struct kfastblock_cached_monitor_socket *cached;
	unsigned long remaining = 0;
	int ret;

	if (!vol || !spec || !cached_out || !sock_out)
		return -EINVAL;
	if (endpoint_index >= spec->nr_monitors)
		return -EINVAL;

	endpoint = &spec->monitors[endpoint_index];
	*sock_out = NULL;
	cached = kfastblock_monitor_conn_pool_acquire_match(vol->monitor_cache,
							spec->nr_monitors,
							endpoint, sock_out);
	if (cached) {
		*cached_out = cached;
		mutex_unlock(&cached->lock);
		return 0;
	}

	ret = kfastblock_monitor_conn_pool_check_backoff(vol->monitor_cache,
						 spec->nr_monitors,
						 endpoint, &remaining);
	if (ret) {
		kfastblock_volume_account_socket_backoff_wait(vol, true, 0,
			endpoint->port, remaining, ret);
		return ret;
	}

	cached = &vol->monitor_cache[endpoint_index];
	*cached_out = cached;
	mutex_lock(&cached->lock);
	if (kfastblock_transport_monitor_socket_matches_locked(cached, endpoint)) {
		kfastblock_monitor_conn_slot_note_reuse_locked(cached);
		if (sock_out)
			*sock_out = cached->sock;
		mutex_unlock(&cached->lock);
		return 0;
	}
	kfastblock_transport_close_cached_monitor_socket_locked(cached);
	kfastblock_monitor_conn_slot_begin_connect_locked(cached);
	ret = kfastblock_transport_maybe_inject_fault(
		vol, KFASTBLOCK_FAULT_MONITOR_CONNECT);
	if (ret) {
		kfastblock_transport_mark_monitor_socket_failure(vol, cached,
								 endpoint, ret);
		mutex_unlock(&cached->lock);
		return ret;
	}
	ret = kfastblock_transport_try_connect_monitor(endpoint, sock_out);
	if (ret) {
		kfastblock_transport_mark_monitor_socket_failure(vol, cached, endpoint, ret);
		mutex_unlock(&cached->lock);
		return ret;
	}

	cached->sock = *sock_out;
	cached->next_seq = 1;
	kfastblock_transport_mark_monitor_socket_success(cached, endpoint);
	mutex_unlock(&cached->lock);
	*sock_out = NULL;
	return 0;
}

static void kfastblock_transport_release_osd_socket(
	struct kfastblock_cached_socket *cached)
{
	if (cached)
		mutex_unlock(&cached->lock);
}

static int kfastblock_transport_acquire_osd_socket(
	struct kfastblock_volume *vol,
	const struct kfastblock_leader_info *leader,
	struct kfastblock_cached_socket **cached_out,
	struct socket **sock_out)
{
	struct kfastblock_cached_socket *cached = NULL;
	struct socket *sock = NULL;
	u32 active_count;
	u32 endpoint_parallel_limit;
	int ret;

	if (!vol || !leader || !cached_out || !sock_out)
		return -EINVAL;

	*cached_out = NULL;
	*sock_out = NULL;
	cached = kfastblock_osd_conn_pool_try_acquire_match(
		vol->socket_cache,
		KFASTBLOCK_MAX_SOCKET_CACHE,
		leader, &sock);
	if (cached) {
		kfastblock_transport_trace_osd_endpoint_decision(
			vol, leader, "reuse", 0, 0);
		*cached_out = cached;
		*sock_out = sock;
		return 0;
	}

	ret = kfastblock_transport_check_osd_backoff(vol, leader);
	if (ret)
		return ret;

	endpoint_parallel_limit =
		max_t(u32, READ_ONCE(g_kfastblock_osd_endpoint_parallel_limit), 1);
	active_count = kfastblock_osd_conn_pool_endpoint_active_count(
		vol->socket_cache, KFASTBLOCK_MAX_SOCKET_CACHE, leader);
	if (active_count >= endpoint_parallel_limit) {
		kfastblock_transport_trace_osd_endpoint_decision(
			vol, leader, "fallback_limit",
			active_count, endpoint_parallel_limit);
		goto fallback_wait_match;
	}

	cached = kfastblock_transport_reserve_osd_slot(vol);
	if (!cached) {
		kfastblock_transport_trace_osd_endpoint_decision(
			vol, leader, "fallback_reserve_busy",
			active_count, endpoint_parallel_limit);
		goto fallback_wait_match;
	}

	cached->osd_id = leader->osd_id;
	cached->port = leader->port;
	strscpy(cached->address, leader->address, sizeof(cached->address));
	ret = kfastblock_transport_connect_osd_cached_locked(vol, cached, leader,
						    &sock);
	if (ret)
		return ret;

	kfastblock_transport_trace_osd_endpoint_decision(
		vol, leader, "reserve_connect",
		active_count + 1, endpoint_parallel_limit);
	*cached_out = cached;
	*sock_out = sock;
	return 0;

fallback_wait_match:
	cached = kfastblock_osd_conn_pool_acquire_match(vol->socket_cache,
							KFASTBLOCK_MAX_SOCKET_CACHE,
							leader, &sock);
	if (!cached)
		return -EBUSY;

	*cached_out = cached;
	*sock_out = sock;
	return 0;
}

static struct kfastblock_request_pg_hint *
kfastblock_transport_find_request_pg_hint(struct kfastblock_request *kf_req,
					  u32 pg_id)
{
	unsigned int i;

	if (!kf_req)
		return NULL;

	for (i = 0; i < kf_req->nr_unique_pgs; ++i) {
		if (kf_req->pg_hints[i].pg_id == pg_id)
			return &kf_req->pg_hints[i];
	}

	return NULL;
}

static int kfastblock_transport_check_osd_backoff(
	struct kfastblock_volume *vol,
	const struct kfastblock_leader_info *leader)
{
	int ret;
	unsigned long remaining = 0;

	if (!vol || !leader)
		return -EINVAL;

	ret = kfastblock_osd_conn_pool_check_backoff(vol->socket_cache,
						     KFASTBLOCK_MAX_SOCKET_CACHE,
						     leader, &remaining);
	if (ret) {
		kfastblock_volume_account_socket_backoff_wait(vol, false,
			leader->osd_id, leader->port, remaining, ret);
		return ret;
	}

	return 0;
}

static struct kfastblock_cached_socket *
kfastblock_transport_reserve_osd_slot(struct kfastblock_volume *vol)
{
	if (!vol)
		return NULL;

	return kfastblock_osd_conn_pool_reserve(vol->socket_cache,
						KFASTBLOCK_MAX_SOCKET_CACHE);
}

static int kfastblock_transport_status_to_errno(const u32 status)
{
	switch (status) {
	case KFASTBLOCK_RAW_STATUS_OK:
		return 0;
	case KFASTBLOCK_RAW_STATUS_INVALID_REQUEST:
		return -EINVAL;
	case KFASTBLOCK_RAW_STATUS_NOT_FOUND:
		return -ENOENT;
	case KFASTBLOCK_RAW_STATUS_STALE_EPOCH:
		return -ESTALE;
	case KFASTBLOCK_RAW_STATUS_RETRY_LATER:
		return -EAGAIN;
	case KFASTBLOCK_RAW_STATUS_NOT_LEADER:
		return -ENOLINK;
	case KFASTBLOCK_RAW_STATUS_PG_INITIALIZING:
		return -EAGAIN;
	case KFASTBLOCK_RAW_STATUS_OSD_DOWN:
		return -EHOSTDOWN;
	default:
		return -EIO;
	}
}

static bool kfastblock_transport_request_failed(
	const struct kfastblock_request *kf_req)
{
	return kf_req && READ_ONCE(kf_req->status) != 0;
}

static bool kfastblock_transport_set_first_error(struct kfastblock_request *kf_req,
						 int ret)
{
	unsigned long flags;
	bool first = false;

	if (!kf_req || !ret)
		return false;

	spin_lock_irqsave(&kf_req->status_lock, flags);
	if (!kf_req->status) {
		kf_req->status = ret;
		first = true;
	}
	spin_unlock_irqrestore(&kf_req->status_lock, flags);
	return first;
}

static int kfastblock_transport_queue_object_work(
	struct kfastblock_request *kf_req,
	unsigned int object_index)
{
	int ret;

	if (!kf_req || object_index >= kf_req->nr_objects)
		return -EINVAL;

	ret = kfastblock_request_mark_object_queued(kf_req, object_index);
	if (ret)
		return ret;

	INIT_WORK(&kf_req->object_works[object_index].work,
		  kfastblock_transport_object_work);
	if (!g_kfastblock_transport_wq ||
	    !queue_work(g_kfastblock_transport_wq,
			&kf_req->object_works[object_index].work)) {
		if (kf_req->vol)
			kfastblock_scheduler_note_dispatch_failure(
				&kf_req->vol->scheduler);
		return -EBUSY;
	}

	return 0;
}

static int
kfastblock_transport_queue_dispatch_batch(struct kfastblock_request *kf_req,
					  const struct kfastblock_request_dispatch_batch *batch)
{
	unsigned int i;
	int ret = 0;

	if (!kf_req || !batch)
		return -EINVAL;

	for (i = 0; i < batch->nr_indexes; ++i) {
		ret = kfastblock_transport_queue_object_work(kf_req,
						     batch->indexes[i]);
		if (ret)
			break;
	}

	return ret;
}

static int kfastblock_transport_refill_dispatch_window(
	struct kfastblock_request *kf_req)
{
	struct kfastblock_request_dispatch_batch batch;
	unsigned int credits;
	int ret;

	if (!kf_req)
		return -EINVAL;

	credits = kfastblock_request_dispatch_credits(kf_req);
	if (!credits)
		return 0;

	kfastblock_request_dispatch_batch_reset(&batch);
	ret = kfastblock_request_pick_dispatch_batch(kf_req, &batch, credits);
	if (ret < 0 || !batch.nr_indexes)
		return ret;

	return kfastblock_transport_queue_dispatch_batch(kf_req, &batch);
}

static int kfastblock_transport_discard_unscheduled(struct kfastblock_request *kf_req)
{
	if (!kf_req)
		return 0;

	return kfastblock_request_cancel_unqueued(kf_req);
}

static enum req_op kfastblock_transport_object_op(
	const struct kfastblock_request *kf_req,
	const unsigned int object_index)
{
	enum req_op op;
	const struct kfastblock_object_extent *extent;

	if (!kf_req || !kf_req->rq || object_index >= kf_req->nr_objects)
		return REQ_OP_LAST;

	op = req_op(kf_req->rq);
	if (op != REQ_OP_DISCARD)
		return op;

	extent = &kf_req->objects[object_index];
	if (extent->object_offset == 0 &&
	    extent->length == kf_req->request_object_size)
		return REQ_OP_DISCARD;

	return REQ_OP_WRITE_ZEROES;
}

static u8 kfastblock_transport_raw_opcode_for_object_op(enum req_op op)
{
	switch (op) {
	case REQ_OP_WRITE:
	case REQ_OP_WRITE_ZEROES:
		return KFASTBLOCK_RAW_OSD_OP_WRITE_OBJECT;
	case REQ_OP_READ:
		return KFASTBLOCK_RAW_OSD_OP_READ_OBJECT;
	case REQ_OP_DISCARD:
		return KFASTBLOCK_RAW_OSD_OP_DELETE_OBJECT;
	default:
		return 0;
	}
}

static int kfastblock_transport_request_view_stale(
	const struct kfastblock_request *kf_req)
{
	u64 osdmap_epoch;
	u64 pgmap_epoch;

	if (!kf_req || !kf_req->vol)
		return -EINVAL;

	osdmap_epoch = READ_ONCE(kf_req->vol->view.osdmap_epoch);
	pgmap_epoch = READ_ONCE(kf_req->vol->view.pgmap_epoch);
	if (osdmap_epoch != kf_req->request_osdmap_epoch ||
	    pgmap_epoch != kf_req->request_pgmap_epoch)
		return -ESTALE;

	return 0;
}

static int kfastblock_transport_send_all(struct socket *sock,
					 const void *buf,
					 size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		struct kvec iov = {
			.iov_base = (void *)((const u8 *)buf + sent),
			.iov_len = len - sent,
		};
		struct msghdr msg = {
			.msg_flags = MSG_NOSIGNAL,
		};
		int rc = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);

		if (rc <= 0)
			return rc < 0 ? rc : -EIO;
		sent += rc;
	}

	return 0;
}

static int kfastblock_transport_configure_socket(struct socket *sock)
{
	long timeout;

	if (!sock || !sock->sk)
		return -EINVAL;

	timeout = msecs_to_jiffies(KFASTBLOCK_DEFAULT_SOCKET_TIMEOUT_MS);
	sock->sk->sk_rcvtimeo = timeout;
	sock->sk->sk_sndtimeo = timeout;
	return 0;
}

static int kfastblock_transport_recv_all(struct socket *sock,
					 void *buf,
					 size_t len)
{
	size_t received = 0;

	while (received < len) {
		struct kvec iov = {
			.iov_base = (u8 *)buf + received,
			.iov_len = len - received,
		};
		struct msghdr msg = {
			.msg_flags = MSG_WAITALL,
		};
		int rc = kernel_recvmsg(sock, &msg, &iov, 1, iov.iov_len, msg.msg_flags);

		if (rc <= 0)
			return rc < 0 ? rc : -EIO;
		received += rc;
	}

	return 0;
}

static int kfastblock_transport_send_request(struct socket *sock,
					     u8 service,
					     u8 opcode,
					     u64 seq,
					     const void *body,
					     u32 body_len)
{
	struct kfastblock_raw_header hdr = {
		.magic = cpu_to_le32(KFASTBLOCK_RAW_MAGIC),
		.version_major = KFASTBLOCK_RAW_VERSION_MAJOR,
		.version_minor = KFASTBLOCK_RAW_VERSION_MINOR,
		.service = service,
		.opcode = opcode,
		.flags = cpu_to_le32(0),
		.seq = cpu_to_le64(seq),
		.status = cpu_to_le32(0),
		.body_len = cpu_to_le32(body_len),
	};
	int ret;

	ret = kfastblock_transport_send_all(sock, &hdr, sizeof(hdr));
	if (ret)
		return ret;
	if (!body_len)
		return 0;
	return kfastblock_transport_send_all(sock, body, body_len);
}

static int kfastblock_transport_send_request_parts(
	struct socket *sock,
	u8 service,
	u8 opcode,
	u64 seq,
	const struct kfastblock_transport_body_part *parts,
	unsigned int nr_parts)
{
	u32 body_len = 0;
	unsigned int i;
	int ret;

	if (parts && nr_parts) {
		for (i = 0; i < nr_parts; ++i)
			body_len += parts[i].len;
	}

	ret = kfastblock_transport_send_request(sock, service, opcode, seq, NULL, body_len);
	if (ret)
		return ret;

	for (i = 0; i < nr_parts; ++i) {
		if (!parts[i].len)
			continue;
		ret = kfastblock_transport_send_all(sock, parts[i].buf, parts[i].len);
		if (ret)
			return ret;
	}

	return 0;
}

static int kfastblock_transport_recv_response(struct socket *sock,
					      u8 expected_service,
					      u8 expected_opcode,
					      u64 expected_seq,
					      struct kfastblock_raw_header *hdr,
					      void **body)
{
	u32 body_len;
	int ret;

	if (!hdr || !body)
		return -EINVAL;

	*body = NULL;
	ret = kfastblock_transport_recv_all(sock, hdr, sizeof(*hdr));
	if (ret)
		return ret;
	if (le32_to_cpu(hdr->magic) != KFASTBLOCK_RAW_MAGIC)
		return -EPROTO;
	if (hdr->version_major != KFASTBLOCK_RAW_VERSION_MAJOR)
		return -EPROTO;
	if (hdr->service != expected_service)
		return -EPROTO;
	if (hdr->opcode != expected_opcode)
		return -EPROTO;
	if (!(le32_to_cpu(hdr->flags) & KFASTBLOCK_RAW_FLAG_RESPONSE))
		return -EPROTO;
	if (le64_to_cpu(hdr->seq) != expected_seq)
		return -EPROTO;

	body_len = le32_to_cpu(hdr->body_len);
	if (!body_len)
		return kfastblock_transport_status_to_errno(le32_to_cpu(hdr->status));

	*body = kfastblock_transport_alloc_buffer(body_len, GFP_KERNEL,
				       KFASTBLOCK_TRANSPORT_BUFFER_LARGE);
	if (!*body)
		return -ENOMEM;
	ret = kfastblock_transport_recv_all(sock, *body, body_len);
	if (ret) {
		kfastblock_transport_free_buffer(*body,
				       KFASTBLOCK_TRANSPORT_BUFFER_LARGE);
		*body = NULL;
		return ret;
	}
	return kfastblock_transport_status_to_errno(le32_to_cpu(hdr->status));
}

static void kfastblock_transport_response_ctx_reset(
	struct kfastblock_transport_response_ctx *ctx)
{
	if (!ctx)
		return;

	memset(&ctx->hdr, 0, sizeof(ctx->hdr));
	ctx->body = NULL;
	ctx->body_len = 0;
	ctx->ret = 0;
}

static void kfastblock_transport_response_ctx_release(
	struct kfastblock_transport_response_ctx *ctx)
{
	if (!ctx)
		return;

	kvfree(ctx->body);
	ctx->body = NULL;
	ctx->body_len = 0;
	ctx->ret = 0;
	memset(&ctx->hdr, 0, sizeof(ctx->hdr));
}

static int kfastblock_transport_response_expect_min_body(
	const struct kfastblock_transport_response_ctx *ctx,
	u32 min_len)
{
	if (!ctx)
		return -EINVAL;
	if (!ctx->body || ctx->body_len < min_len)
		return -EPROTO;
	return 0;
}

static int kfastblock_transport_response_copy_fixed(
	const struct kfastblock_transport_response_ctx *ctx,
	void *dst,
	u32 expected_len)
{
	int ret;

	if (!ctx || !dst)
		return -EINVAL;

	ret = kfastblock_transport_response_expect_min_body(ctx, expected_len);
	if (ret)
		return ret;
	if (ctx->body_len != expected_len)
		return -EPROTO;

	memcpy(dst, ctx->body, expected_len);
	return 0;
}

static int kfastblock_transport_exec_request_parts(
	struct socket *sock,
	u8 service,
	u8 opcode,
	u64 seq,
	const struct kfastblock_transport_body_part *parts,
	unsigned int nr_parts,
	struct kfastblock_transport_response_ctx *rsp)
{
	int ret;

	if (!rsp)
		return -EINVAL;

	kfastblock_transport_response_ctx_reset(rsp);
	ret = kfastblock_transport_send_request_parts(sock, service, opcode, seq,
						      parts, nr_parts);
	if (ret) {
		rsp->ret = ret;
		return ret;
	}

	ret = kfastblock_transport_recv_response(sock, service, opcode, seq,
						 &rsp->hdr, &rsp->body);
	rsp->ret = ret;
	if (!ret)
		rsp->body_len = le32_to_cpu(rsp->hdr.body_len);
	return ret;
}

static int kfastblock_transport_exec_request(
	struct socket *sock,
	u8 service,
	u8 opcode,
	u64 seq,
	const void *body,
	u32 body_len,
	struct kfastblock_transport_response_ctx *rsp)
{
	struct kfastblock_transport_body_part part = {
		.buf = body,
		.len = body_len,
	};

	if (!body || !body_len)
		return kfastblock_transport_exec_request_parts(sock, service, opcode,
							       seq, NULL, 0, rsp);
	return kfastblock_transport_exec_request_parts(sock, service, opcode, seq,
						       &part, 1, rsp);
}

static int kfastblock_transport_exec_request_fixed_body(
	struct socket *sock,
	u8 service,
	u8 opcode,
	u64 seq,
	const struct kfastblock_transport_body_part *parts,
	unsigned int nr_parts,
	void *dst,
	u32 expected_len)
{
	struct kfastblock_transport_response_ctx response = {};
	int ret;

	ret = kfastblock_transport_exec_request_parts(sock, service, opcode, seq,
						      parts, nr_parts,
						      &response);
	if (ret)
		goto out;

	ret = kfastblock_transport_response_copy_fixed(&response, dst,
						       expected_len);
out:
	kfastblock_transport_response_ctx_release(&response);
	return ret;
}

static int kfastblock_transport_exec_request_status_only(
	struct socket *sock,
	u8 service,
	u8 opcode,
	u64 seq,
	const struct kfastblock_transport_body_part *parts,
	unsigned int nr_parts)
{
	struct kfastblock_transport_response_ctx response = {};
	int ret;

	ret = kfastblock_transport_exec_request_parts(sock, service, opcode, seq,
						      parts, nr_parts,
						      &response);
	kfastblock_transport_response_ctx_release(&response);
	return ret;
}

static int kfastblock_transport_begin_exchange(
	struct kfastblock_transport_exchange_ctx *ctx,
	struct kfastblock_request *kf_req,
	unsigned int object_index,
	u8 service,
	u8 opcode,
	u64 seq)
{
	if (!ctx || !kf_req || !seq)
		return -EINVAL;

	memset(ctx, 0, sizeof(*ctx));
	ctx->kf_req = kf_req;
	ctx->object_index = object_index;
	ctx->service = service;
	ctx->opcode = opcode;
	ctx->seq = seq;
	ctx->pipeline_entry = kfastblock_pipeline_begin_exchange(
		&kf_req->pipeline, seq, object_index, service, opcode);
	if (!ctx->pipeline_entry)
		return -ENOMEM;

	kfastblock_request_record_object_seq(kf_req, object_index, seq);
	kfastblock_volume_update_pipeline_snapshot(kf_req->vol,
						&kf_req->pipeline);
	return 0;
}

static void kfastblock_transport_finish_exchange(
	struct kfastblock_transport_exchange_ctx *ctx,
	int ret,
	struct kfastblock_raw_header *rsp_hdr)
{
	u32 flags = 0;
	u32 body_len = 0;
	s32 status = ret;

	if (!ctx || !ctx->kf_req || !ctx->seq)
		return;

	if (rsp_hdr) {
		flags = le32_to_cpu(rsp_hdr->flags);
		body_len = le32_to_cpu(rsp_hdr->body_len);
		status = kfastblock_transport_status_to_errno(
			le32_to_cpu(rsp_hdr->status));
	}
	kfastblock_request_record_object_response(ctx->kf_req,
						  ctx->object_index,
						  status,
						  body_len,
						  flags);
	kfastblock_pipeline_finish_exchange(&ctx->kf_req->pipeline,
					    ctx->seq, ret, status,
					    body_len, flags);
	kfastblock_volume_update_pipeline_snapshot(ctx->kf_req->vol,
						&ctx->kf_req->pipeline);
}

static int kfastblock_transport_match_exchange_response(
	struct kfastblock_transport_exchange_ctx *ctx)
{
	unsigned int matched_object = 0;
	int ret;

	if (!ctx || !ctx->kf_req)
		return -EINVAL;

	ret = kfastblock_request_lookup_object_by_seq(ctx->kf_req, ctx->seq,
						      &matched_object);
	if (ret)
		return ret;
	if (matched_object != ctx->object_index)
		return -EPROTO;

	return 0;
}

static bool kfastblock_transport_retry_requested(unsigned int actions)
{
	return !!(actions & KFASTBLOCK_RECOVERY_RETRY);
}

static bool kfastblock_transport_handle_object_failure(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx || !ctx->kf_req || !ctx->extent || !ctx->actions)
		return false;

	kfastblock_recovery_apply_object_failure(
		ctx->kf_req->vol, ctx->kf_req->request_pool_id, ctx->extent,
		ctx->op, &ctx->leader, ctx->ret, ctx->actions);
	if (ctx->hint &&
	    (ctx->actions & KFASTBLOCK_RECOVERY_INVALIDATE_LEADER))
		kfastblock_request_invalidate_pg_hint_leader(ctx->hint);
	if (kfastblock_transport_retry_requested(ctx->actions)) {
		kfastblock_request_requeue_object(
			ctx->kf_req, ctx->object_index, ctx->ret);
		return true;
	}

	return false;
}

static int kfastblock_transport_complete_exchange_response(
	struct kfastblock_transport_exchange_ctx *ctx,
	struct kfastblock_transport_response_ctx *response,
	int ret)
{
	int match_ret;

	if (!ctx)
		return -EINVAL;

	match_ret = ret ? 0 : kfastblock_transport_match_exchange_response(ctx);
	if (!ret && match_ret)
		ret = match_ret;
	kfastblock_transport_finish_exchange(ctx, ret,
					    ret || !response ? NULL : &response->hdr);
	return ret;
}

static void kfastblock_transport_release_response_and_exchange(
	struct kfastblock_transport_exchange_ctx *exchange,
	struct kfastblock_transport_response_ctx *response,
	int *ret)
{
	int local_ret = ret ? *ret : 0;

	if (exchange)
		local_ret = kfastblock_transport_complete_exchange_response(
			exchange, response, local_ret);
	if (ret)
		*ret = local_ret;
	kfastblock_transport_response_ctx_release(response);
}

static int kfastblock_transport_response_decode_leader(
	struct kfastblock_transport_response_ctx *response,
	struct kfastblock_leader_info *leader)
{
	struct kfastblock_raw_get_leader_rsp rsp;
	u16 address_len;

	if (!response || !leader)
		return -EINVAL;

	if (kfastblock_transport_response_expect_min_body(response, sizeof(rsp)))
		return -EPROTO;

	memcpy(&rsp, response->body, sizeof(rsp));
	address_len = le16_to_cpu(rsp.address_len);
	if (response->body_len != sizeof(rsp) + address_len ||
	    !address_len ||
	    address_len >= sizeof(leader->address))
		return -EPROTO;

	memset(leader, 0, sizeof(*leader));
	leader->osd_id = le32_to_cpu(rsp.leader_id);
	leader->port = le16_to_cpu(rsp.leader_port);
	memcpy(leader->address, (u8 *)response->body + sizeof(rsp), address_len);
	leader->address[address_len] = '\0';
	if (!leader->osd_id || !leader->port || !leader->address[0])
		return -EPROTO;

	return 0;
}

static int kfastblock_transport_response_decode_read_object(
	struct kfastblock_transport_response_ctx *response,
	const struct kfastblock_object_extent *extent,
	void *data)
{
	struct kfastblock_raw_read_object_rsp rsp;
	u32 data_len;

	if (!response || !extent || (!data && extent->length))
		return -EINVAL;

	if (kfastblock_transport_response_expect_min_body(response, sizeof(rsp)))
		return -EPROTO;

	memcpy(&rsp, response->body, sizeof(rsp));
	data_len = le32_to_cpu(rsp.data_len);
	if (response->body_len != sizeof(rsp) + data_len ||
	    data_len > extent->length)
		return -EPROTO;

	memset(data, 0, extent->length);
	if (data_len)
		memcpy(data, (u8 *)response->body + sizeof(rsp), data_len);
	return 0;
}

static int kfastblock_transport_response_decode_cluster_map_header(
	struct kfastblock_transport_response_ctx *response,
	size_t *offset,
	struct kfastblock_raw_cluster_map_rsp_hdr *rsp)
{
	if (!response || !offset || !rsp)
		return -EINVAL;

	if (kfastblock_transport_response_expect_min_body(response, sizeof(*rsp)))
		return -EPROTO;

	return kfastblock_transport_copy_from_body(response->body,
						   response->body_len,
						   offset, rsp,
						   sizeof(*rsp));
}

static int kfastblock_transport_response_decode_cluster_map(
	struct kfastblock_transport_response_ctx *response,
	struct kfastblock_cluster_view *view)
{
	struct kfastblock_raw_cluster_map_rsp_hdr rsp;
	struct kfastblock_cluster_view scratch = {};
	u32 pool_pg_count = 0;
	size_t offset = 0;
	int ret;

	if (!response || !view)
		return -EINVAL;

	ret = kfastblock_transport_response_decode_cluster_map_header(
		response, &offset, &rsp);
	if (ret)
		goto out;

	scratch.osd_count = le32_to_cpu(rsp.osd_count);
	scratch.route_count = le32_to_cpu(rsp.pg_count);
	if (scratch.osd_count) {
		scratch.osds = kvcalloc(scratch.osd_count, sizeof(*scratch.osds),
					GFP_KERNEL);
		if (!scratch.osds) {
			ret = -ENOMEM;
			goto out;
		}
	}

	ret = kfastblock_transport_decode_osd_entries(response->body,
						      response->body_len, &offset,
						      scratch.osds,
						      scratch.osd_count);
	if (ret)
		goto out;

	if (scratch.route_count) {
		scratch.routes = kvcalloc(scratch.route_count,
					  sizeof(*scratch.routes), GFP_KERNEL);
		if (!scratch.routes) {
			ret = -ENOMEM;
			goto out;
		}
	}

	ret = kfastblock_transport_decode_pg_entries(response->body,
						     response->body_len, &offset,
						     scratch.routes,
						     scratch.route_count,
						     view->image.pool_id,
						     &pool_pg_count);
	if (ret)
		goto out;
	if (offset != response->body_len) {
		ret = -EPROTO;
		goto out;
	}
	if (!pool_pg_count) {
		ret = -ENOENT;
		goto out;
	}

	ret = kfastblock_meta_replace_cluster_map(
		view,
		le64_to_cpu(rsp.osdmap_epoch),
		le64_to_cpu(rsp.pgmap_epoch),
		pool_pg_count,
		scratch.osd_count,
		scratch.osds,
		scratch.route_count,
		scratch.routes);
	if (!ret) {
		scratch.osd_count = 0;
		scratch.route_count = 0;
		scratch.osds = NULL;
		scratch.routes = NULL;
	}

out:
	kfastblock_meta_cleanup_view(&scratch);
	return ret;
}

static int kfastblock_transport_fetch_image_info(struct socket *sock,
					 struct kfastblock_cluster_view *view,
					 u64 seq)
{
	struct kfastblock_raw_get_image_info_req req = {
		.image_epoch = cpu_to_le64(view->image_epoch),
		.pool_name_len = cpu_to_le16(strlen(view->image.pool_name)),
		.image_name_len = cpu_to_le16(strlen(view->image.image_name)),
	};
	struct kfastblock_raw_get_image_info_rsp rsp;
	struct kfastblock_transport_body_part parts[3];
	int ret;

	parts[0].buf = &req;
	parts[0].len = sizeof(req);
	parts[1].buf = view->image.pool_name;
	parts[1].len = strlen(view->image.pool_name);
	parts[2].buf = view->image.image_name;
	parts[2].len = strlen(view->image.image_name);

	ret = kfastblock_transport_exec_request_fixed_body(
		sock, KFASTBLOCK_RAW_SERVICE_MONITOR,
		KFASTBLOCK_RAW_OP_GET_IMAGE_INFO, seq,
		parts, ARRAY_SIZE(parts), &rsp, sizeof(rsp));
	if (ret == -ESTALE)
		return 0;
	if (ret)
		return ret;

	view->image_epoch = le64_to_cpu(rsp.image_epoch);
	view->image.pool_id = le32_to_cpu(rsp.pool_id);
	view->image.block_size = le32_to_cpu(rsp.block_size);
	view->image.object_size = le32_to_cpu(rsp.object_size);
	view->image.size_bytes = le64_to_cpu(rsp.size_bytes);
	view->image.read_only = !!(le32_to_cpu(rsp.flags) & KFASTBLOCK_RAW_IMAGE_FLAG_READ_ONLY);
	if (!view->image.block_size)
		view->image.block_size = KFASTBLOCK_DEFAULT_BLOCK_SIZE;

	return 0;
}

static int kfastblock_transport_advance_offset(size_t body_len,
					       size_t *offset,
					       size_t len)
{
	if (!offset)
		return -EINVAL;
	if (*offset + len > body_len)
		return -EPROTO;
	*offset += len;
	return 0;
}

static int kfastblock_transport_copy_from_body(const u8 *body,
					       size_t body_len,
					       size_t *offset,
					       void *dst,
					       size_t len)
{
	int ret;

	if (!body || !dst)
		return -EINVAL;

	ret = kfastblock_transport_advance_offset(body_len, offset, len);
	if (ret)
		return ret;

	memcpy(dst, body + (*offset - len), len);
	return 0;
}

static int kfastblock_transport_decode_osd_entries(
	const u8 *body,
	size_t body_len,
	size_t *offset,
	struct kfastblock_osd_endpoint *osds,
	u32 osd_count)
{
	u32 i;

	for (i = 0; i < osd_count; ++i) {
		struct kfastblock_raw_osd_entry_hdr osd_hdr;
		u16 address_len;
		u16 shard_count;
		u16 shard_idx;
		int ret;

		ret = kfastblock_transport_copy_from_body(body, body_len, offset,
							 &osd_hdr,
							 sizeof(osd_hdr));
		if (ret)
			return ret;

		address_len = le16_to_cpu(osd_hdr.address_len);
		shard_count = le16_to_cpu(osd_hdr.shard_count);
		if (address_len >= sizeof(osds[i].address))
			return -E2BIG;

		osds[i].osd_id = le32_to_cpu(osd_hdr.osd_id);
		osds[i].flags = le32_to_cpu(osd_hdr.flags);
		osds[i].shard_count = shard_count;

		ret = kfastblock_transport_advance_offset(body_len, offset,
							  address_len);
		if (ret)
			return ret;
		if (address_len)
			memcpy(osds[i].address, body + (*offset - address_len),
			       address_len);
		osds[i].address[address_len] = '\0';

		if (!shard_count)
			continue;

		osds[i].shards = kcalloc(shard_count, sizeof(*osds[i].shards),
					 GFP_KERNEL);
		if (!osds[i].shards)
			return -ENOMEM;

		for (shard_idx = 0; shard_idx < shard_count; ++shard_idx) {
			struct kfastblock_raw_osd_shard_entry shard_entry;

			ret = kfastblock_transport_copy_from_body(
				body, body_len, offset, &shard_entry,
				sizeof(shard_entry));
			if (ret)
				return ret;

			osds[i].shards[shard_idx].shard_id =
				le32_to_cpu(shard_entry.shard_id);
			osds[i].shards[shard_idx].port =
				le16_to_cpu(shard_entry.port);
			osds[i].shards[shard_idx].core_id =
				le16_to_cpu(shard_entry.core_id);
		}
	}

	return 0;
}

static int kfastblock_transport_decode_pg_entries(
	const u8 *body,
	size_t body_len,
	size_t *offset,
	struct kfastblock_pg_route *routes,
	u32 route_count,
	u32 pool_id,
	u32 *pool_pg_count)
{
	u32 i;

	if (!pool_pg_count)
		return -EINVAL;

	*pool_pg_count = 0;
	for (i = 0; i < route_count; ++i) {
		struct kfastblock_raw_pg_entry_hdr pg_hdr;
		u32 replica_count;
		u32 replica_idx;
		int ret;

		ret = kfastblock_transport_copy_from_body(body, body_len, offset,
							 &pg_hdr,
							 sizeof(pg_hdr));
		if (ret)
			return ret;

		routes[i].pool_id = le32_to_cpu(pg_hdr.pool_id);
		routes[i].pg_id = le32_to_cpu(pg_hdr.pg_id);
		routes[i].version = le64_to_cpu(pg_hdr.version);
		routes[i].state = le32_to_cpu(pg_hdr.state);
		routes[i].primary_shard = le32_to_cpu(pg_hdr.primary_shard);
		routes[i].replica_count = le32_to_cpu(pg_hdr.replica_count);
		replica_count = routes[i].replica_count;

		if (routes[i].pool_id == pool_id)
			++(*pool_pg_count);
		if (!replica_count)
			continue;

		routes[i].osd_ids = kcalloc(replica_count,
					     sizeof(*routes[i].osd_ids),
					     GFP_KERNEL);
		if (!routes[i].osd_ids)
			return -ENOMEM;

		for (replica_idx = 0; replica_idx < replica_count; ++replica_idx) {
			__le32 osd_id;

			ret = kfastblock_transport_copy_from_body(body, body_len,
							 offset, &osd_id,
							 sizeof(osd_id));
			if (ret)
				return ret;

			routes[i].osd_ids[replica_idx] = le32_to_cpu(osd_id);
		}
	}

	return 0;
}

static int kfastblock_transport_fetch_cluster_map_from_monitor(
	struct socket *sock,
	struct kfastblock_cluster_view *view,
	u64 seq)
{
	struct kfastblock_raw_get_cluster_map_req req = {
		.osdmap_epoch = cpu_to_le64(view->osdmap_epoch),
		.pool_id = cpu_to_le32(view->image.pool_id),
		.reserved = cpu_to_le32(0),
		.pgmap_epoch = cpu_to_le64(view->pgmap_epoch),
	};
	struct kfastblock_transport_response_ctx response = {};
	int ret;

	ret = kfastblock_transport_exec_request(
		sock, KFASTBLOCK_RAW_SERVICE_MONITOR,
		KFASTBLOCK_RAW_OP_GET_CLUSTER_MAP, seq,
		&req, sizeof(req), &response);
	if (ret == -ESTALE)
		return 0;
	if (ret) {
		kfastblock_transport_response_ctx_release(&response);
		return ret;
	}
	ret = kfastblock_transport_response_decode_cluster_map(&response, view);
	kfastblock_transport_response_ctx_release(&response);
	return ret;
}

static int kfastblock_transport_fetch_pg_leader_from_osd(
	struct socket *sock,
	u32 pool_id,
	u32 pg_id,
	u64 seq,
	struct kfastblock_leader_info *leader)
{
	struct kfastblock_raw_get_leader_req req = {
		.pool_id = cpu_to_le32(pool_id),
		.pg_id = cpu_to_le32(pg_id),
	};
	struct kfastblock_transport_response_ctx response = {};
	int ret;

	if (!leader)
		return -EINVAL;

	memset(leader, 0, sizeof(*leader));
	ret = kfastblock_transport_exec_request(
		sock, KFASTBLOCK_RAW_SERVICE_OSD,
		KFASTBLOCK_RAW_OSD_OP_GET_LEADER, seq,
		&req, sizeof(req), &response);
	if (ret) {
		kfastblock_transport_response_ctx_release(&response);
		return ret;
	}
	ret = kfastblock_transport_response_decode_leader(&response, leader);
	kfastblock_transport_response_ctx_release(&response);
	return 0;
}

static int kfastblock_transport_write_object(struct socket *sock,
					     u32 pool_id,
					     const struct kfastblock_object_extent *extent,
					     const void *data,
					     u64 seq,
					     struct kfastblock_transport_response_ctx *response)
{
	struct kfastblock_raw_write_object_req req = {
		.pool_id = cpu_to_le32(pool_id),
	};
	struct kfastblock_transport_body_part parts[3];
	u32 object_name_len;
	u32 data_len;
	int ret;

	if (!sock || !extent || (!data && extent->length))
		return -EINVAL;

	object_name_len = strlen(extent->object_name);
	data_len = extent->length;
	req.pg_id = cpu_to_le32(extent->pg_id);
	req.offset = cpu_to_le64(extent->object_offset);
	req.data_len = cpu_to_le32(data_len);
	req.object_name_len = cpu_to_le16(object_name_len);
	req.reserved = 0;

	parts[0].buf = &req;
	parts[0].len = sizeof(req);
	parts[1].buf = extent->object_name;
	parts[1].len = object_name_len;
	parts[2].buf = data;
	parts[2].len = data_len;

	ret = kfastblock_transport_exec_request_parts(
		sock, KFASTBLOCK_RAW_SERVICE_OSD,
		KFASTBLOCK_RAW_OSD_OP_WRITE_OBJECT, seq,
		parts, ARRAY_SIZE(parts), response);
	return ret;
}

static int kfastblock_transport_read_object(struct socket *sock,
					    u32 pool_id,
					    const struct kfastblock_object_extent *extent,
					    void *data,
					    u64 seq,
					    struct kfastblock_transport_response_ctx *response)
{
	struct kfastblock_raw_read_object_req req = {
		.pool_id = cpu_to_le32(pool_id),
		.pg_id = cpu_to_le32(extent->pg_id),
		.offset = cpu_to_le64(extent->object_offset),
		.length = cpu_to_le32(extent->length),
		.object_name_len = cpu_to_le16(strlen(extent->object_name)),
		.reserved = 0,
	};
	struct kfastblock_transport_body_part parts[2];
	u32 object_name_len;
	int ret;

	if (!sock || !extent || (!data && extent->length))
		return -EINVAL;

	object_name_len = strlen(extent->object_name);
	parts[0].buf = &req;
	parts[0].len = sizeof(req);
	parts[1].buf = extent->object_name;
	parts[1].len = object_name_len;

	ret = kfastblock_transport_exec_request_parts(
		sock, KFASTBLOCK_RAW_SERVICE_OSD,
		KFASTBLOCK_RAW_OSD_OP_READ_OBJECT, seq,
		parts, ARRAY_SIZE(parts), response);
	if (ret)
		return ret;

	return kfastblock_transport_response_decode_read_object(response,
								extent,
								data);
}

static int kfastblock_transport_delete_object(struct socket *sock,
					      u32 pool_id,
					      const struct kfastblock_object_extent *extent,
					      u64 seq,
					      struct kfastblock_transport_response_ctx *response)
{
	struct kfastblock_raw_delete_object_req req = {
		.pool_id = cpu_to_le32(pool_id),
		.pg_id = cpu_to_le32(extent->pg_id),
		.object_name_len = cpu_to_le16(strlen(extent->object_name)),
		.reserved = 0,
	};
	struct kfastblock_transport_body_part parts[2];
	u32 object_name_len;
	int ret;

	if (!sock || !extent)
		return -EINVAL;

	object_name_len = strlen(extent->object_name);
	parts[0].buf = &req;
	parts[0].len = sizeof(req);
	parts[1].buf = extent->object_name;
	parts[1].len = object_name_len;

	ret = kfastblock_transport_exec_request_status_only(
		sock, KFASTBLOCK_RAW_SERVICE_OSD,
		KFASTBLOCK_RAW_OSD_OP_DELETE_OBJECT, seq,
		parts, ARRAY_SIZE(parts));
	kfastblock_transport_response_ctx_reset(response);
	response->ret = ret;
	return ret;
}

static int kfastblock_transport_copy_request_data(struct request *rq,
						  u32 request_offset,
						  void *buf,
						  u32 len,
						  bool to_request)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	u32 copied = 0;
	u32 skipped = 0;

	if (!rq || (!buf && len))
		return -EINVAL;
	if (!len)
		return 0;

	rq_for_each_segment(bvec, rq, iter) {
		u32 seg_offset;
		u32 chunk;
		u8 *page_addr;

		if (request_offset >= skipped + bvec.bv_len) {
			skipped += bvec.bv_len;
			continue;
		}

		seg_offset = request_offset > skipped ?
			request_offset - skipped : 0;
		chunk = min_t(u32, len - copied, bvec.bv_len - seg_offset);
		page_addr = kmap_local_page(bvec.bv_page);
		if (to_request)
			memcpy(page_addr + bvec.bv_offset + seg_offset,
			       (u8 *)buf + copied, chunk);
		else
			memcpy((u8 *)buf + copied,
			       page_addr + bvec.bv_offset + seg_offset, chunk);
		kunmap_local(page_addr);

		copied += chunk;
		skipped += bvec.bv_len;
		if (copied == len)
			return 0;
	}

	return copied == len ? 0 : -EIO;
}

static int kfastblock_transport_prepare_object_buffer(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	struct kfastblock_volume *vol;
	void *buf;
	int ret = 0;

	if (!ctx)
		return -EINVAL;
	ctx->buf = NULL;
	vol = ctx->vol;
	if (!vol)
		return -EINVAL;

	switch (ctx->op) {
	case REQ_OP_WRITE:
		buf = kfastblock_buffer_pool_alloc(&vol->object_buffer_pool,
						   ctx->extent->length, GFP_KERNEL,
						   false);
		if (!buf)
			return -ENOMEM;
		ret = kfastblock_transport_copy_request_data(
			ctx->kf_req->rq, ctx->extent->request_offset, buf,
			ctx->extent->length, false);
		if (ret) {
			kfastblock_buffer_pool_free(&vol->object_buffer_pool, buf);
			return ret;
		}
		ctx->buf = buf;
		return 0;
	case REQ_OP_WRITE_ZEROES:
		buf = kfastblock_buffer_pool_alloc(&vol->object_buffer_pool,
						   ctx->extent->length, GFP_KERNEL,
						   true);
		if (!buf)
			return -ENOMEM;
		ctx->buf = buf;
		return 0;
	case REQ_OP_READ:
		buf = kfastblock_buffer_pool_alloc(&vol->object_buffer_pool,
						   ctx->extent->length, GFP_KERNEL,
						   false);
		if (!buf)
			return -ENOMEM;
		ctx->buf = buf;
		return 0;
	default:
		return 0;
	}
}

static void kfastblock_transport_release_object_buffer(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx || !ctx->kf_req || !ctx->kf_req->vol)
		return;

	kfastblock_buffer_pool_free(&ctx->kf_req->vol->object_buffer_pool,
				      ctx->buf);
	ctx->buf = NULL;
}

static int kfastblock_transport_execute_object_opcode(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return -EINVAL;

	if (ctx->op == REQ_OP_WRITE || ctx->op == REQ_OP_WRITE_ZEROES)
		return kfastblock_transport_write_object(
			ctx->sock, ctx->kf_req->request_pool_id, ctx->extent,
			ctx->buf, ctx->exchange.seq, &ctx->response);
	if (ctx->op == REQ_OP_READ)
		return kfastblock_transport_read_object(
			ctx->sock, ctx->kf_req->request_pool_id, ctx->extent,
			ctx->buf, ctx->exchange.seq, &ctx->response);
	return kfastblock_transport_delete_object(
		ctx->sock, ctx->kf_req->request_pool_id, ctx->extent,
		ctx->exchange.seq, &ctx->response);
}

static int kfastblock_transport_finish_object_success(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	int ret = 0;

	if (!ctx || !ctx->kf_req || !ctx->extent)
		return -EINVAL;

	if (ctx->op == REQ_OP_READ)
		ret = kfastblock_transport_copy_request_data(
			ctx->kf_req->rq, ctx->extent->request_offset, ctx->buf,
			ctx->extent->length, true);
	if (ret)
		return ret;

	kfastblock_volume_account_object_success(
		ctx->kf_req->vol, ctx->op, ctx->extent->pg_id, ctx->leader.osd_id,
		ctx->extent->length);
	kfastblock_scheduler_note_success(&ctx->kf_req->vol->scheduler);
	return 0;
}

static int kfastblock_transport_pick_object_leader(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return -EINVAL;

	if (ctx->attempt == 0 && ctx->hint &&
	    !kfastblock_request_get_pg_hint_leader(ctx->hint, &ctx->leader))
		return 0;

	return kfastblock_transport_query_pg_leader(
		ctx->kf_req, ctx->hint, &ctx->leader);
}

static int kfastblock_transport_normalize_object_ret(
	struct kfastblock_transport_object_io_ctx *ctx,
	int ret)
{
	if (!ctx)
		return ret;

	if (ret == -ENOENT && ctx->op == REQ_OP_READ) {
		memset(ctx->buf, 0, ctx->extent->length);
		return 0;
	}
	if (ret == -ENOENT && ctx->op == REQ_OP_DISCARD)
		return 0;

	return ret;
}

static int kfastblock_transport_prepare_object_exchange(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	u64 seq;
	int ret;

	if (!ctx)
		return -EINVAL;

	ret = kfastblock_transport_acquire_osd_socket(
		ctx->vol, &ctx->leader, &ctx->cached, &ctx->sock);
	if (ret)
		return ret;

	seq = kfastblock_transport_next_seq(ctx->cached);
	return kfastblock_transport_begin_exchange(
		&ctx->exchange, ctx->kf_req, ctx->object_index,
		KFASTBLOCK_RAW_SERVICE_OSD, ctx->raw_opcode, seq);
}

static bool kfastblock_transport_retry_object_after_failure(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx || !ctx->actions)
		return false;

	return kfastblock_transport_handle_object_failure(ctx);
}

static void kfastblock_transport_note_object_leader_success(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx || !ctx->hint)
		return;

	kfastblock_request_set_pg_hint_leader(ctx->hint, &ctx->leader);
}

static void kfastblock_transport_finalize_object_socket(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx || !ctx->vol || !ctx->cached)
		return;

	kfastblock_recovery_finalize_osd_socket(
		ctx->vol, ctx->cached, &ctx->leader, ctx->ret, ctx->actions);
	ctx->cached = NULL;
}

static void kfastblock_transport_abort_object_exchange(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return;

	kfastblock_transport_finalize_object_socket(ctx);
	kfastblock_transport_finish_exchange(&ctx->exchange, ctx->ret, NULL);
}

static int kfastblock_transport_run_object_exchange(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return -EINVAL;

	ctx->ret = kfastblock_transport_execute_object_opcode(ctx);
	ctx->ret = kfastblock_transport_normalize_object_ret(ctx, ctx->ret);
	if (!ctx->ret)
		ctx->ret = kfastblock_transport_match_exchange_response(
			&ctx->exchange);
	kfastblock_transport_release_response_and_exchange(
		&ctx->exchange, &ctx->response, &ctx->ret);
	return ctx->ret;
}

static void kfastblock_transport_reject_stale_object_request(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return;

	ctx->ret = kfastblock_transport_request_view_stale(ctx->kf_req);
	if (ctx->ret) {
		ctx->actions = kfastblock_recovery_classify_object_failure(
			ctx->ret);
		kfastblock_transport_handle_object_failure(ctx);
	}
}

static void kfastblock_transport_cleanup_object_io(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return;

	if (ctx->cached)
		kfastblock_transport_release_osd_socket(ctx->cached);
	if (ctx->ret && ctx->vol && ctx->extent)
		kfastblock_volume_account_object_error(
			ctx->vol, ctx->op, ctx->extent->pg_id, ctx->leader.osd_id,
			ctx->extent->length, ctx->ret);
	kfastblock_transport_release_object_buffer(ctx);
	ctx->cached = NULL;
	ctx->sock = NULL;
	ctx->ret = 0;
	ctx->actions = 0;
	kfastblock_transport_response_ctx_reset(&ctx->response);
}

static int kfastblock_transport_begin_object_attempt(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	int ret;

	if (!ctx)
		return -EINVAL;

	ret = kfastblock_transport_pick_object_leader(ctx);
	if (ret)
		return ret;

	return kfastblock_transport_prepare_object_exchange(ctx);
}

static int kfastblock_transport_complete_successful_object_attempt(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return -EINVAL;

	kfastblock_transport_note_object_leader_success(ctx);
	ctx->ret = kfastblock_transport_finish_object_success(ctx);
	return ctx->ret;
}

static bool kfastblock_transport_fail_object_attempt(
	struct kfastblock_transport_object_io_ctx *ctx,
	enum kfastblock_transport_object_attempt_failure_kind kind)
{
	if (!ctx)
		return false;

	ctx->actions = kfastblock_recovery_classify_object_failure(ctx->ret);
	if (kind == KFASTBLOCK_OBJECT_ATTEMPT_FAIL_AFTER_EXCHANGE)
		kfastblock_transport_abort_object_exchange(ctx);
	else
		kfastblock_transport_finalize_object_socket(ctx);
	return kfastblock_transport_retry_object_after_failure(ctx);
}

static bool kfastblock_transport_finalize_completed_object_attempt(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return false;

	ctx->actions = ctx->ret ?
		kfastblock_recovery_classify_object_failure(ctx->ret) : 0;
	kfastblock_transport_finalize_object_socket(ctx);
	if (ctx->ret)
		return kfastblock_transport_retry_object_after_failure(ctx);

	ctx->ret = kfastblock_transport_complete_successful_object_attempt(ctx);
	return false;
}

static int kfastblock_transport_object_io_ctx_init(
	struct kfastblock_transport_object_io_ctx *ctx,
	struct kfastblock_request *kf_req,
	unsigned int object_index)
{
	if (!ctx || !kf_req || !kf_req->vol || !kf_req->rq ||
	    object_index >= kf_req->nr_objects)
		return -EINVAL;

	memset(ctx, 0, sizeof(*ctx));
	ctx->kf_req = kf_req;
	ctx->vol = kf_req->vol;
	ctx->extent = &kf_req->objects[object_index];
	ctx->hint = kfastblock_transport_find_request_pg_hint(
		kf_req, ctx->extent->pg_id);
	ctx->object_index = object_index;
	ctx->op = kfastblock_transport_object_op(kf_req, object_index);
	ctx->raw_opcode = kfastblock_transport_raw_opcode_for_object_op(ctx->op);
	return 0;
}

static void kfastblock_transport_object_io_ctx_reset_attempt(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return;

	memset(&ctx->leader, 0, sizeof(ctx->leader));
	ctx->cached = NULL;
	ctx->sock = NULL;
	ctx->ret = 0;
	ctx->actions = 0;
	memset(&ctx->exchange, 0, sizeof(ctx->exchange));
	kfastblock_transport_response_ctx_reset(&ctx->response);
}

static void kfastblock_transport_begin_object_io_ctx(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return;

	kfastblock_request_mark_object_inflight(ctx->kf_req, ctx->object_index);
	if (kfastblock_transport_request_failed(ctx->kf_req)) {
		ctx->ret = -ECANCELED;
		return;
	}

	switch (ctx->op) {
	case REQ_OP_FLUSH:
		ctx->ret = 0;
		return;
	case REQ_OP_READ:
	case REQ_OP_WRITE:
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		ctx->ret = 0;
		return;
	default:
		ctx->ret = -EOPNOTSUPP;
		return;
	}
}

static void kfastblock_transport_prepare_object_execution(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return;

	ctx->ret = 0;
	ctx->actions = 0;
	kfastblock_transport_reject_stale_object_request(ctx);
	if (ctx->ret)
		return;

	ctx->ret = kfastblock_transport_prepare_object_buffer(ctx);
}

static bool kfastblock_transport_drive_object_attempt(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	if (!ctx)
		return false;

	ctx->ret = kfastblock_transport_begin_object_attempt(ctx);
	if (ctx->ret) {
		return kfastblock_transport_fail_object_attempt(
			ctx, KFASTBLOCK_OBJECT_ATTEMPT_FAIL_BEFORE_EXCHANGE);
	}

	ctx->ret = kfastblock_transport_maybe_inject_fault(
		ctx->vol, KFASTBLOCK_FAULT_OBJECT_IO);
	if (ctx->ret) {
		return kfastblock_transport_fail_object_attempt(
			ctx, KFASTBLOCK_OBJECT_ATTEMPT_FAIL_AFTER_EXCHANGE);
	}

	ctx->ret = kfastblock_transport_run_object_exchange(ctx);
	if (kfastblock_transport_finalize_completed_object_attempt(ctx))
		return true;

	return false;
}

static void kfastblock_transport_run_object_attempts(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	int attempt;

	if (!ctx)
		return;

	for (attempt = 0; attempt < KFASTBLOCK_OBJECT_IO_MAX_ATTEMPTS; ++attempt) {
		kfastblock_transport_object_io_ctx_reset_attempt(ctx);
		ctx->attempt = attempt;
		if (kfastblock_transport_drive_object_attempt(ctx))
			continue;
		return;
	}
}

static int kfastblock_transport_run_object_io_ctx(
	struct kfastblock_transport_object_io_ctx *ctx)
{
	int ret;

	if (!ctx)
		return -EINVAL;

	ctx->ret = 0;
	kfastblock_transport_begin_object_io_ctx(ctx);
	if (!ctx->ret && ctx->op != REQ_OP_FLUSH) {
		kfastblock_transport_prepare_object_execution(ctx);
		if (!ctx->ret)
			kfastblock_transport_run_object_attempts(ctx);
	}
	if (ctx->op == REQ_OP_FLUSH)
		ctx->ret = 0;
	ret = ctx->ret;
	kfastblock_transport_cleanup_object_io(ctx);
	return ret;
}

static int kfastblock_transport_submit_object_io(
	struct kfastblock_request *kf_req,
	unsigned int object_index)
{
	struct kfastblock_transport_object_io_ctx ctx = {};

	ctx.ret = kfastblock_transport_object_io_ctx_init(
		&ctx, kf_req, object_index);
	if (ctx.ret)
		return ctx.ret;

	return kfastblock_transport_run_object_io_ctx(&ctx);
}

int kfastblock_transport_init(void)
{
	g_kfastblock_transport_wq = alloc_workqueue(
		"kfastblock-transport",
		WQ_UNBOUND | WQ_MEM_RECLAIM,
		KFASTBLOCK_DEFAULT_TRANSPORT_MAX_ACTIVE);
	return g_kfastblock_transport_wq ? 0 : -ENOMEM;
}

void kfastblock_transport_exit(void)
{
	if (!g_kfastblock_transport_wq)
		return;

	drain_workqueue(g_kfastblock_transport_wq);
	destroy_workqueue(g_kfastblock_transport_wq);
	g_kfastblock_transport_wq = NULL;
}

static int kfastblock_transport_sockaddr_from_host_port(const char *host,
							u16 port,
							struct sockaddr_in *addr)
{
	int ret;

	if (!addr || !host || !*host || !port)
		return -EINVAL;

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	ret = in4_pton(host, -1, (u8 *)&addr->sin_addr.s_addr,
		       -1, NULL);
	if (!ret)
		return -EINVAL;

	return 0;
}

int kfastblock_transport_sockaddr_from_endpoint(
	const struct kfastblock_monitor_endpoint *endpoint,
	struct sockaddr_in *addr)
{
	if (!endpoint)
		return -EINVAL;

	return kfastblock_transport_sockaddr_from_host_port(endpoint->host,
						 endpoint->port,
						 addr);
}

static int kfastblock_transport_try_connect_host(const char *host,
						 u16 port,
						 struct socket **sock)
{
	struct sockaddr_in addr;
	struct socket *local_sock;
	int ret;

	if (!sock)
		return -EINVAL;

	ret = kfastblock_transport_sockaddr_from_host_port(host, port, &addr);
	if (ret)
		return ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &local_sock);
	if (ret)
		return ret;

	ret = kfastblock_transport_configure_socket(local_sock);
	if (ret) {
		sock_release(local_sock);
		return ret;
	}

	ret = kernel_connect(local_sock, (struct sockaddr *)&addr, sizeof(addr), 0);
	if (ret) {
		sock_release(local_sock);
		return ret;
	}

	*sock = local_sock;
	return 0;
}

int kfastblock_transport_try_connect_monitor(
	const struct kfastblock_monitor_endpoint *endpoint,
	struct socket **sock)
{
	if (!endpoint)
		return -EINVAL;

	return kfastblock_transport_try_connect_host(endpoint->host,
					 endpoint->port,
					 sock);
}

int kfastblock_transport_fetch_cluster_view(struct kfastblock_cluster_view *view,
				    const struct kfastblock_attach_spec *spec)
{
	u32 i;
	u64 seq = 1;
	int first_err = -EIO;

	if (!view || !spec || !spec->nr_monitors)
		return -EINVAL;

	for (i = 0; i < spec->nr_monitors; ++i) {
		struct socket *sock = NULL;
		int ret;

		ret = kfastblock_transport_try_connect_monitor(&spec->monitors[i],
					      &sock);
		if (ret) {
			first_err = ret;
			continue;
		}

		ret = kfastblock_transport_fetch_image_info(sock, view, seq++);
		if (!ret)
			ret = kfastblock_transport_fetch_cluster_map_from_monitor(sock, view, seq++);
		sock_release(sock);
		if (!ret)
			return 0;
		first_err = ret;
	}

	return first_err;
}

int kfastblock_transport_fetch_image(struct kfastblock_cluster_view *view,
				     const struct kfastblock_attach_spec *spec)
{
	u32 i;
	u64 seq = 1;
	int first_err = -EIO;

	if (!view || !spec || !spec->nr_monitors)
		return -EINVAL;

	for (i = 0; i < spec->nr_monitors; ++i) {
		struct socket *sock = NULL;
		int ret;

		ret = kfastblock_transport_try_connect_monitor(&spec->monitors[i],
					      &sock);
		if (ret) {
			first_err = ret;
			continue;
		}

		ret = kfastblock_transport_fetch_image_info(sock, view, seq++);
		sock_release(sock);
		if (!ret)
			return 0;
		first_err = ret;
	}

	return first_err;
}

int kfastblock_transport_fetch_cluster_map(struct kfastblock_cluster_view *view,
					   const struct kfastblock_attach_spec *spec)
{
	u32 i;
	u64 seq = 1;
	int first_err = -EIO;

	if (!view || !spec || !spec->nr_monitors)
		return -EINVAL;

	for (i = 0; i < spec->nr_monitors; ++i) {
		struct socket *sock = NULL;
		int ret;

		ret = kfastblock_transport_try_connect_monitor(&spec->monitors[i],
					      &sock);
		if (ret) {
			first_err = ret;
			continue;
		}

		ret = kfastblock_transport_fetch_cluster_map_from_monitor(
			sock, view, seq++);
		sock_release(sock);
		if (!ret)
			return 0;
		first_err = ret;
	}

	return first_err;
}

static int kfastblock_transport_query_pg_leader(
	struct kfastblock_request *kf_req,
	struct kfastblock_request_pg_hint *hint,
	struct kfastblock_leader_info *leader)
{
	struct kfastblock_volume *vol;
	u32 i;
	int first_err = -ENOENT;
	int ret;

	if (!kf_req || !kf_req->vol || !hint || !leader)
		return -EINVAL;
	vol = kf_req->vol;
	if (!hint->nr_targets || !hint->targets)
		return -ENOENT;

	ret = kfastblock_transport_maybe_inject_fault(
		vol, KFASTBLOCK_FAULT_LEADER_QUERY);
	if (ret) {
		kfastblock_recovery_apply_leader_failure(
			vol, kf_req->request_pool_id, hint->pg_id, ret,
			kfastblock_recovery_classify_leader_failure(ret));
		return ret;
	}

	ret = kfastblock_transport_request_view_stale(kf_req);
	if (ret) {
			kfastblock_recovery_apply_leader_failure(
				vol, kf_req->request_pool_id, hint->pg_id, ret,
				kfastblock_recovery_classify_leader_failure(ret));
		return ret;
	}

	for (i = 0; i < hint->nr_targets; ++i) {
		const struct kfastblock_request_pg_target *target_hint =
			&hint->targets[i];
		struct kfastblock_cached_socket *cached = NULL;
		struct socket *sock = NULL;
		struct kfastblock_leader_info target = {};
		unsigned int actions;
		u64 seq;

		if (!(target_hint->flags & KFASTBLOCK_OSD_FLAG_IN) ||
		    !(target_hint->flags & KFASTBLOCK_OSD_FLAG_UP) ||
		    !target_hint->address[0] || !target_hint->port) {
			first_err = -EHOSTDOWN;
			continue;
		}

		target.osd_id = target_hint->osd_id;
		target.port = target_hint->port;
		strscpy(target.address, target_hint->address, sizeof(target.address));
		ret = kfastblock_transport_acquire_osd_socket(vol, &target,
						      &cached, &sock);
		if (ret) {
				kfastblock_recovery_apply_leader_failure(
					vol, kf_req->request_pool_id, hint->pg_id, ret,
					kfastblock_recovery_classify_leader_failure(ret));
			first_err = ret;
			continue;
		}
		seq = kfastblock_transport_next_seq(cached);
		ret = kfastblock_transport_fetch_pg_leader_from_osd(sock,
						    kf_req->request_pool_id,
						    hint->pg_id,
						    seq,
						    leader);
		actions = kfastblock_recovery_classify_leader_failure(ret);
		kfastblock_recovery_finalize_osd_socket(vol, cached, &target, ret,
							actions);
		cached = NULL;
		kfastblock_volume_account_leader_query(
			vol, hint->pg_id, target.osd_id, ret);
		if (!ret) {
			ret = kfastblock_recovery_update_live_pg_leader(
				vol, kf_req->request_pool_id, hint->pg_id, leader);
			return ret ? ret : 0;
		}

		kfastblock_request_invalidate_pg_hint_leader(hint);
			kfastblock_recovery_apply_leader_failure(
				vol, kf_req->request_pool_id, hint->pg_id, ret,
				actions);
		first_err = ret;
	}

	return first_err;
}

static int kfastblock_transport_submit_object(struct kfastblock_request *kf_req,
					      unsigned int object_index)
{
	if (!kf_req || !kf_req->rq || !kf_req->vol ||
	    object_index >= kf_req->nr_objects)
		return -EINVAL;

	return kfastblock_transport_submit_object_io(kf_req, object_index);
}

static int kfastblock_transport_prefetch_pg_leader(
	struct kfastblock_request *kf_req,
	struct kfastblock_request_pg_hint *hint)
{
	struct kfastblock_leader_info leader;
	struct kfastblock_cached_socket *cached = NULL;
	struct socket *sock = NULL;
	int ret;

	if (!kf_req || !kf_req->vol || !hint)
		return -EINVAL;

	if (kfastblock_request_get_pg_hint_leader(hint, &leader))
		ret = kfastblock_transport_query_pg_leader(kf_req, hint, &leader);
	else
		ret = 0;
	if (ret) {
		kfastblock_request_invalidate_pg_hint_leader(hint);
		return ret;
	}

	kfastblock_request_set_pg_hint_leader(hint, &leader);
	ret = kfastblock_transport_acquire_osd_socket(
		kf_req->vol, &leader, &cached, &sock);
	(void)sock;
	if (!ret)
		kfastblock_transport_release_osd_socket(cached);

	return ret;
}

static void kfastblock_transport_prefetch_submit_leaders(
	struct kfastblock_transport_submit_ctx *ctx)
{
	unsigned int i;

	if (!ctx || !ctx->kf_req)
		return;

	ctx->ret = kfastblock_transport_request_view_stale(ctx->kf_req);
	if (ctx->ret) {
		kfastblock_volume_account_refresh_kick(ctx->kf_req->vol, 0,
						       ctx->ret);
		kfastblock_volume_kick_refresh(ctx->kf_req->vol);
		return;
	}

	for (i = 0; i < ctx->kf_req->nr_unique_pgs; ++i) {
		ctx->ret = kfastblock_transport_prefetch_pg_leader(
			ctx->kf_req, &ctx->kf_req->pg_hints[i]);
		if (ctx->ret &&
		    kfastblock_recovery_prefetch_should_fail_request(ctx->ret))
			return;
	}

	ctx->ret = 0;
}

static void kfastblock_transport_finish_request_now(
	struct kfastblock_request *kf_req,
	bool mark_success)
{
	blk_status_t status;

	if (!kf_req)
		return;

	status = kfastblock_recovery_errno_to_blk_status(kf_req->status);
	kfastblock_volume_account_io_complete(kf_req->vol, kf_req->status);
	if (mark_success && !kf_req->status)
		kfastblock_volume_mark_success(
			kf_req->vol, KFASTBLOCK_VOLUME_SOURCE_OBJECT_IO);
	kfastblock_request_cleanup(kf_req);
	blk_mq_end_request(kf_req->rq, status);
	kfastblock_volume_put_io(kf_req->vol);
	put_device(&kf_req->vol->dev);
}

static int kfastblock_transport_finish_submit_now(
	struct kfastblock_request *kf_req,
	int ret)
{
	if (!kf_req || !kf_req->rq || !kf_req->vol)
		return -EINVAL;

	if (!ret) {
		kfastblock_request_cleanup(kf_req);
		blk_mq_end_request(kf_req->rq, BLK_STS_OK);
		kfastblock_volume_put_io(kf_req->vol);
		return 0;
	}

	kfastblock_request_cleanup(kf_req);
	kfastblock_volume_account_io_complete(kf_req->vol, ret);
	blk_mq_end_request(kf_req->rq,
		kfastblock_recovery_errno_to_blk_status(ret));
	kfastblock_volume_put_io(kf_req->vol);
	return 0;
}

static void kfastblock_transport_finish_request_if_idle(
	struct kfastblock_request *kf_req,
	int remaining,
	bool mark_success)
{
	if (!kf_req || remaining != 0)
		return;

	kfastblock_transport_finish_request_now(kf_req, mark_success);
}

static int kfastblock_transport_drop_unscheduled_pending(
	struct kfastblock_request *kf_req,
	int remaining)
{
	int unscheduled;

	if (!kf_req)
		return remaining;

	unscheduled = kfastblock_transport_discard_unscheduled(kf_req);
	if (!unscheduled)
		return remaining;

	return atomic_sub_return(unscheduled, &kf_req->pending_objects);
}

static bool kfastblock_transport_update_request_status_after_object(
	struct kfastblock_request *kf_req,
	unsigned int object_index,
	int ret)
{
	bool first_error = false;

	if (!kf_req)
		return false;

	kfastblock_request_mark_object_complete(kf_req, object_index, ret);
	if (ret)
		first_error = kfastblock_transport_set_first_error(kf_req, ret);
	else if (!kfastblock_transport_request_failed(kf_req))
		first_error = kfastblock_transport_set_first_error(
			kf_req, kfastblock_transport_refill_dispatch_window(kf_req));

	return first_error;
}

static int kfastblock_transport_advance_request_pending(
	struct kfastblock_request *kf_req,
	bool drop_unscheduled)
{
	int remaining;

	if (!kf_req)
		return 0;

	remaining = atomic_dec_return(&kf_req->pending_objects);
	if (drop_unscheduled)
		remaining = kfastblock_transport_drop_unscheduled_pending(
			kf_req, remaining);

	return remaining;
}

static void kfastblock_transport_complete_request(struct kfastblock_request *kf_req,
						  unsigned int object_index,
						  int ret)
{
	bool first_error;
	int remaining;

	first_error = kfastblock_transport_update_request_status_after_object(
		kf_req, object_index, ret);
	remaining = kfastblock_transport_advance_request_pending(
		kf_req, first_error);

	kfastblock_transport_finish_request_if_idle(kf_req, remaining, true);
}

static void kfastblock_transport_abort_request(struct kfastblock_request *kf_req,
					       int ret)
{
	int remaining;

	if (!kf_req)
		return;

	(void)kfastblock_transport_set_first_error(kf_req, ret);
	remaining = kfastblock_transport_drop_unscheduled_pending(kf_req, 0);
	kfastblock_transport_finish_request_if_idle(kf_req, remaining, false);
}

static void kfastblock_transport_object_work(struct work_struct *work)
{
	struct kfastblock_object_work *obj_work = container_of(work,
						struct kfastblock_object_work,
						work);
	struct kfastblock_request *kf_req = obj_work->parent;
	int ret;

	if (!kf_req || !kf_req->rq || !kf_req->vol)
		return;

	ret = kfastblock_transport_submit_object(kf_req,
					       obj_work->object_index);
	kfastblock_transport_complete_request(kf_req,
					     obj_work->object_index,
					     ret);
}

static int kfastblock_transport_finish_empty_request(
	struct kfastblock_request *kf_req)
{
	return kfastblock_transport_finish_submit_now(kf_req, 0);
}

static int kfastblock_transport_submit_ctx_init(
	struct kfastblock_transport_submit_ctx *ctx,
	struct kfastblock_request *kf_req)
{
	if (!ctx || !kf_req || !kf_req->rq || !kf_req->vol)
		return -EINVAL;

	memset(ctx, 0, sizeof(*ctx));
	ctx->kf_req = kf_req;
	kfastblock_request_dispatch_batch_reset(&ctx->batch);
	return 0;
}

static void kfastblock_transport_prepare_request_submit(
	struct kfastblock_transport_submit_ctx *ctx)
{
	if (!ctx || !ctx->kf_req)
		return;

	if (!ctx->kf_req->nr_objects) {
		ctx->ret = kfastblock_transport_finish_empty_request(ctx->kf_req);
		ctx->finished = true;
		return;
	}

	ctx->kf_req->status = 0;
	kfastblock_request_prepare_runtime(ctx->kf_req);
	atomic_set(&ctx->kf_req->pending_objects, ctx->kf_req->nr_objects);
	ctx->initial_dispatch = min_t(unsigned int, ctx->kf_req->nr_objects,
				      ctx->kf_req->dispatch_window ?
				      ctx->kf_req->dispatch_window :
				      1);
	kfastblock_transport_prefetch_submit_leaders(ctx);
}

static int kfastblock_transport_kick_initial_dispatch(
	struct kfastblock_transport_submit_ctx *ctx)
{
	if (!ctx || !ctx->kf_req)
		return -EINVAL;

	kfastblock_request_dispatch_batch_reset(&ctx->batch);
	ctx->ret = kfastblock_request_pick_dispatch_batch(
		ctx->kf_req, &ctx->batch, ctx->initial_dispatch);
	if (ctx->ret < 0) {
		kfastblock_transport_abort_request(ctx->kf_req, ctx->ret);
		return 0;
	}

	get_device(&ctx->kf_req->vol->dev);
	ctx->ret = kfastblock_transport_queue_dispatch_batch(
		ctx->kf_req, &ctx->batch);
	if (ctx->ret)
		kfastblock_transport_abort_request(ctx->kf_req, ctx->ret);

	return 0;
}

static int kfastblock_transport_run_request_submit(
	struct kfastblock_transport_submit_ctx *ctx)
{
	if (!ctx || !ctx->kf_req)
		return -EINVAL;

	kfastblock_transport_prepare_request_submit(ctx);
	if (ctx->finished)
		return 0;
	if (ctx->ret < 0)
		return kfastblock_transport_finish_submit_now(ctx->kf_req,
							      ctx->ret);

	return kfastblock_transport_kick_initial_dispatch(ctx);
}

int kfastblock_transport_submit(struct kfastblock_request *kf_req)
{
	struct kfastblock_transport_submit_ctx ctx = {};
	int ret;

	ret = kfastblock_transport_submit_ctx_init(&ctx, kf_req);
	if (ret)
		return ret;

	return kfastblock_transport_run_request_submit(&ctx);
}

int kfastblock_transport_refresh_image_volume(struct kfastblock_volume *vol)
{
	u32 i;
	int first_err = -EIO;

	if (!vol)
		return -EINVAL;

	for (i = 0; i < vol->spec.nr_monitors; ++i) {
		struct kfastblock_cached_monitor_socket *cached = NULL;
		struct socket *sock = NULL;
		u64 seq;
		int ret;
		unsigned int actions;

		ret = kfastblock_transport_get_monitor_socket(vol, &vol->spec, i,
					      &cached, &sock);
		if (ret) {
			first_err = ret;
			continue;
		}

		mutex_lock(&cached->lock);
		if (!kfastblock_transport_monitor_socket_matches_locked(cached,
						      &vol->spec.monitors[i])) {
			mutex_unlock(&cached->lock);
			first_err = -EAGAIN;
			continue;
		}
		sock = cached->sock;
		seq = kfastblock_transport_next_monitor_seq(cached);
		ret = kfastblock_transport_maybe_inject_fault(
			vol, KFASTBLOCK_FAULT_MONITOR_FETCH_IMAGE);
		if (ret) {
			actions = kfastblock_recovery_classify_monitor_failure(ret);
			kfastblock_recovery_finalize_monitor_socket(vol, cached, ret,
								 actions);
			first_err = ret;
			continue;
		}
		ret = kfastblock_transport_fetch_image_info(sock, &vol->view, seq);
		if (!ret || ret == -ESTALE) {
			mutex_unlock(&cached->lock);
			return 0;
		}
		actions = kfastblock_recovery_classify_monitor_failure(ret);
		kfastblock_recovery_finalize_monitor_socket(vol, cached, ret,
							 actions);
		first_err = ret;
	}

	return first_err;
}

int kfastblock_transport_refresh_cluster_map_volume(struct kfastblock_volume *vol)
{
	u32 i;
	int first_err = -EIO;

	if (!vol)
		return -EINVAL;

	for (i = 0; i < vol->spec.nr_monitors; ++i) {
		struct kfastblock_cached_monitor_socket *cached = NULL;
		struct socket *sock = NULL;
		u64 seq;
		int ret;
		unsigned int actions;

		ret = kfastblock_transport_get_monitor_socket(vol, &vol->spec, i,
					      &cached, &sock);
		if (ret) {
			first_err = ret;
			continue;
		}

		mutex_lock(&cached->lock);
		if (!kfastblock_transport_monitor_socket_matches_locked(cached,
						      &vol->spec.monitors[i])) {
			mutex_unlock(&cached->lock);
			first_err = -EAGAIN;
			continue;
		}
		sock = cached->sock;
		seq = kfastblock_transport_next_monitor_seq(cached);
		ret = kfastblock_transport_maybe_inject_fault(
			vol, KFASTBLOCK_FAULT_MONITOR_FETCH_CLUSTER);
		if (ret) {
			actions = kfastblock_recovery_classify_monitor_failure(ret);
			kfastblock_recovery_finalize_monitor_socket(vol, cached, ret,
								 actions);
			first_err = ret;
			continue;
		}
		ret = kfastblock_transport_fetch_cluster_map_from_monitor(sock,
					      &vol->view, seq);
		if (!ret || ret == -ESTALE) {
			mutex_unlock(&cached->lock);
			return 0;
		}
		actions = kfastblock_recovery_classify_monitor_failure(ret);
		kfastblock_recovery_finalize_monitor_socket(vol, cached, ret,
							 actions);
		first_err = ret;
	}

	return first_err;
}
