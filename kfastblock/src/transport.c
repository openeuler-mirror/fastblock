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

#define KFASTBLOCK_TRANSPORT_OSD_ENDPOINT_PARALLEL_LIMIT 1U

enum kfastblock_transport_buffer_flags {
	KFASTBLOCK_TRANSPORT_BUFFER_ZERO = 1U << 0,
	KFASTBLOCK_TRANSPORT_BUFFER_LARGE = 1U << 1,
};

struct kfastblock_transport_body_part {
	const void *buf;
	u32 len;
};

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
		*cached_out = cached;
		*sock_out = sock;
		return 0;
	}

	ret = kfastblock_transport_check_osd_backoff(vol, leader);
	if (ret)
		return ret;

	active_count = kfastblock_osd_conn_pool_endpoint_active_count(
		vol->socket_cache, KFASTBLOCK_MAX_SOCKET_CACHE, leader);
	if (active_count >= KFASTBLOCK_TRANSPORT_OSD_ENDPOINT_PARALLEL_LIMIT)
		goto fallback_wait_match;

	cached = kfastblock_transport_reserve_osd_slot(vol);
	if (!cached)
		goto fallback_wait_match;

	cached->osd_id = leader->osd_id;
	cached->port = leader->port;
	strscpy(cached->address, leader->address, sizeof(cached->address));
	ret = kfastblock_transport_connect_osd_cached_locked(vol, cached, leader,
						    &sock);
	if (ret)
		return ret;

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
	if (!kf_req || object_index >= kf_req->nr_objects)
		return -EINVAL;

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

static int kfastblock_transport_queue_next_object(struct kfastblock_request *kf_req)
{
	unsigned int object_index;
	int ret;

	if (!kf_req)
		return -EINVAL;

	mutex_lock(&kf_req->dispatch_lock);
	if (kf_req->next_object_to_queue >= kf_req->nr_objects) {
		mutex_unlock(&kf_req->dispatch_lock);
		return 0;
	}
	object_index = kf_req->next_object_to_queue;
	ret = kfastblock_transport_queue_object_work(kf_req, object_index);
	if (!ret)
		kf_req->next_object_to_queue++;
	mutex_unlock(&kf_req->dispatch_lock);

	return ret;
}

static int kfastblock_transport_discard_unscheduled(struct kfastblock_request *kf_req)
{
	unsigned int unscheduled;

	if (!kf_req)
		return 0;

	mutex_lock(&kf_req->dispatch_lock);
	if (kf_req->next_object_to_queue >= kf_req->nr_objects) {
		mutex_unlock(&kf_req->dispatch_lock);
		return 0;
	}
	unscheduled = kf_req->nr_objects - kf_req->next_object_to_queue;
	kf_req->next_object_to_queue = kf_req->nr_objects;
	mutex_unlock(&kf_req->dispatch_lock);

	return unscheduled;
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
	struct kfastblock_raw_header rsp_hdr;
	void *body = NULL;
	struct kfastblock_transport_body_part parts[3];
	u32 body_len;
	int ret;

	body_len = sizeof(req) + strlen(view->image.pool_name) + strlen(view->image.image_name);
	parts[0].buf = &req;
	parts[0].len = sizeof(req);
	parts[1].buf = view->image.pool_name;
	parts[1].len = strlen(view->image.pool_name);
	parts[2].buf = view->image.image_name;
	parts[2].len = strlen(view->image.image_name);

	ret = kfastblock_transport_send_request_parts(sock,
					KFASTBLOCK_RAW_SERVICE_MONITOR,
					KFASTBLOCK_RAW_OP_GET_IMAGE_INFO,
					seq,
					parts,
					ARRAY_SIZE(parts));
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_SERVICE_MONITOR,
					 KFASTBLOCK_RAW_OP_GET_IMAGE_INFO,
					 seq,
					 &rsp_hdr,
					 &body);
	if (ret == -ESTALE)
		return 0;
	if (ret) {
		kvfree(body);
		return ret;
	}
	if (!body || le32_to_cpu(rsp_hdr.body_len) != sizeof(rsp)) {
		kvfree(body);
		return -EPROTO;
	}
	memcpy(&rsp, body, sizeof(rsp));
	kvfree(body);

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
	struct kfastblock_raw_cluster_map_rsp_hdr rsp;
	struct kfastblock_raw_header rsp_hdr;
	struct kfastblock_cluster_view scratch = {};
	void *body = NULL;
	u32 body_len;
	u32 pool_pg_count = 0;
	size_t offset = 0;
	int ret;

	ret = kfastblock_transport_send_request(sock,
					KFASTBLOCK_RAW_SERVICE_MONITOR,
					KFASTBLOCK_RAW_OP_GET_CLUSTER_MAP,
					seq,
					&req,
					sizeof(req));
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_SERVICE_MONITOR,
					 KFASTBLOCK_RAW_OP_GET_CLUSTER_MAP,
					 seq,
					 &rsp_hdr,
					 &body);
	if (ret == -ESTALE)
		return 0;
	if (ret) {
			kvfree(body);
			return ret;
		}
	body_len = le32_to_cpu(rsp_hdr.body_len);
	if (!body || body_len < sizeof(rsp)) {
		kvfree(body);
		return -EPROTO;
	}

	ret = kfastblock_transport_copy_from_body(body, body_len, &offset, &rsp,
						 sizeof(rsp));
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

	ret = kfastblock_transport_decode_osd_entries(body, body_len, &offset,
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

	ret = kfastblock_transport_decode_pg_entries(body, body_len, &offset,
						     scratch.routes,
						     scratch.route_count,
						     view->image.pool_id,
						     &pool_pg_count);
	if (ret)
		goto out;
	if (offset != body_len) {
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
	kvfree(body);
	kfastblock_meta_cleanup_view(&scratch);
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
	struct kfastblock_raw_get_leader_rsp rsp;
	struct kfastblock_raw_header rsp_hdr;
	void *body = NULL;
	u32 body_len;
	u16 address_len;
	int ret;

	if (!leader)
		return -EINVAL;

	memset(leader, 0, sizeof(*leader));
	ret = kfastblock_transport_send_request(sock,
					KFASTBLOCK_RAW_SERVICE_OSD,
					KFASTBLOCK_RAW_OSD_OP_GET_LEADER,
					seq,
					&req,
					sizeof(req));
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_SERVICE_OSD,
					 KFASTBLOCK_RAW_OSD_OP_GET_LEADER,
					 seq,
					 &rsp_hdr,
					 &body);
	if (ret) {
			kvfree(body);
			return ret;
		}

	body_len = le32_to_cpu(rsp_hdr.body_len);
	if (!body || body_len < sizeof(rsp)) {
		kvfree(body);
		return -EPROTO;
	}

	memcpy(&rsp, body, sizeof(rsp));
	address_len = le16_to_cpu(rsp.address_len);
	if (body_len != sizeof(rsp) + address_len ||
	    !address_len ||
	    address_len >= sizeof(leader->address)) {
			kvfree(body);
			return -EPROTO;
		}

	leader->osd_id = le32_to_cpu(rsp.leader_id);
	leader->port = le16_to_cpu(rsp.leader_port);
	memcpy(leader->address, (u8 *)body + sizeof(rsp), address_len);
	leader->address[address_len] = '\0';
	kvfree(body);

	if (!leader->osd_id || !leader->port || !leader->address[0])
		return -EPROTO;

	return 0;
}

static int kfastblock_transport_write_object(struct socket *sock,
					     u32 pool_id,
					     const struct kfastblock_object_extent *extent,
					     const void *data,
					     u64 seq)
{
	struct kfastblock_raw_write_object_req req = {
		.pool_id = cpu_to_le32(pool_id),
	};
	struct kfastblock_raw_header rsp_hdr;
	void *body = NULL;
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

	ret = kfastblock_transport_send_request_parts(sock,
					KFASTBLOCK_RAW_SERVICE_OSD,
					KFASTBLOCK_RAW_OSD_OP_WRITE_OBJECT,
					seq,
					parts,
					ARRAY_SIZE(parts));
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_SERVICE_OSD,
					 KFASTBLOCK_RAW_OSD_OP_WRITE_OBJECT,
					 seq,
					 &rsp_hdr,
					 &body);
	kvfree(body);
	return ret;
}

static int kfastblock_transport_read_object(struct socket *sock,
					    u32 pool_id,
					    const struct kfastblock_object_extent *extent,
					    void *data,
					    u64 seq)
{
	struct kfastblock_raw_read_object_req req = {
		.pool_id = cpu_to_le32(pool_id),
		.pg_id = cpu_to_le32(extent->pg_id),
		.offset = cpu_to_le64(extent->object_offset),
		.length = cpu_to_le32(extent->length),
		.object_name_len = cpu_to_le16(strlen(extent->object_name)),
		.reserved = 0,
	};
	struct kfastblock_raw_read_object_rsp rsp;
	struct kfastblock_raw_header rsp_hdr;
	void *body = NULL;
	struct kfastblock_transport_body_part parts[2];
	u32 object_name_len;
	u32 body_len;
	u32 data_len;
	int ret;

	if (!sock || !extent || (!data && extent->length))
		return -EINVAL;

	object_name_len = strlen(extent->object_name);
	parts[0].buf = &req;
	parts[0].len = sizeof(req);
	parts[1].buf = extent->object_name;
	parts[1].len = object_name_len;

	ret = kfastblock_transport_send_request_parts(sock,
					KFASTBLOCK_RAW_SERVICE_OSD,
					KFASTBLOCK_RAW_OSD_OP_READ_OBJECT,
					seq,
					parts,
					ARRAY_SIZE(parts));
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_SERVICE_OSD,
					 KFASTBLOCK_RAW_OSD_OP_READ_OBJECT,
					 seq,
					 &rsp_hdr,
					 &body);
	if (ret) {
			kvfree(body);
			return ret;
		}

	body_len = le32_to_cpu(rsp_hdr.body_len);
	if (!body || body_len < sizeof(rsp)) {
		kvfree(body);
		return -EPROTO;
	}

	memcpy(&rsp, body, sizeof(rsp));
	data_len = le32_to_cpu(rsp.data_len);
	if (body_len != sizeof(rsp) + data_len || data_len > extent->length) {
			kvfree(body);
			return -EPROTO;
		}

	memset(data, 0, extent->length);
	if (data_len)
		memcpy(data, (u8 *)body + sizeof(rsp), data_len);
	kvfree(body);
	return 0;
}

static int kfastblock_transport_delete_object(struct socket *sock,
					      u32 pool_id,
					      const struct kfastblock_object_extent *extent,
					      u64 seq)
{
	struct kfastblock_raw_delete_object_req req = {
		.pool_id = cpu_to_le32(pool_id),
		.pg_id = cpu_to_le32(extent->pg_id),
		.object_name_len = cpu_to_le16(strlen(extent->object_name)),
		.reserved = 0,
	};
	struct kfastblock_raw_header rsp_hdr;
	void *body = NULL;
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

	ret = kfastblock_transport_send_request_parts(sock,
					KFASTBLOCK_RAW_SERVICE_OSD,
					KFASTBLOCK_RAW_OSD_OP_DELETE_OBJECT,
					seq,
					parts,
					ARRAY_SIZE(parts));
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_SERVICE_OSD,
					 KFASTBLOCK_RAW_OSD_OP_DELETE_OBJECT,
					 seq,
					 &rsp_hdr,
					 &body);
	kvfree(body);
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

static int kfastblock_transport_submit_object_io(
	struct kfastblock_request *kf_req,
	const struct kfastblock_object_extent *extent,
	enum req_op op)
{
	struct kfastblock_volume *vol;
	struct request *rq;
	struct kfastblock_leader_info leader = {};
	struct kfastblock_request_pg_hint *hint;
	struct kfastblock_cached_socket *cached = NULL;
	struct socket *sock = NULL;
	void *buf = NULL;
	unsigned int actions = 0;
	int ret;
	int attempt;
	u64 seq;

	if (!kf_req || !kf_req->vol || !kf_req->rq || !extent)
		return -EINVAL;
	vol = kf_req->vol;
	rq = kf_req->rq;
	hint = kfastblock_transport_find_request_pg_hint(kf_req, extent->pg_id);

	ret = kfastblock_transport_request_view_stale(kf_req);
	if (ret) {
		kfastblock_recovery_apply_object_failure(
			vol, kf_req->request_pool_id, extent, op, NULL, ret,
			kfastblock_recovery_classify_object_failure(ret));
		if (hint)
			kfastblock_request_invalidate_pg_hint_leader(hint);
		return ret;
	}

	if (op == REQ_OP_WRITE) {
			buf = kfastblock_buffer_pool_alloc(
				&vol->object_buffer_pool,
				extent->length, GFP_KERNEL, false);
			if (!buf)
				return -ENOMEM;
		ret = kfastblock_transport_copy_request_data(
			rq, extent->request_offset, buf, extent->length, false);
		if (ret)
			goto out;
	} else if (op == REQ_OP_WRITE_ZEROES) {
			buf = kfastblock_buffer_pool_alloc(
				&vol->object_buffer_pool,
				extent->length, GFP_KERNEL, true);
			if (!buf)
				return -ENOMEM;
		} else if (op == REQ_OP_READ) {
			buf = kfastblock_buffer_pool_alloc(
				&vol->object_buffer_pool,
				extent->length, GFP_KERNEL, false);
			if (!buf)
				return -ENOMEM;
		}

	for (attempt = 0; attempt < 2; ++attempt) {
		if (attempt == 0 && hint &&
		    !kfastblock_request_get_pg_hint_leader(hint, &leader)) {
			ret = 0;
		} else {
			ret = kfastblock_transport_query_pg_leader(kf_req, hint, &leader);
		}
		if (ret)
			goto out;

		ret = kfastblock_transport_acquire_osd_socket(vol, &leader,
						      &cached, &sock);
		if (ret) {
			actions = kfastblock_recovery_classify_object_failure(ret);

			if (actions) {
				kfastblock_recovery_apply_object_failure(
					vol, kf_req->request_pool_id, extent, op, &leader,
					ret, actions);
				if (hint &&
				    (actions & KFASTBLOCK_RECOVERY_INVALIDATE_LEADER))
					kfastblock_request_invalidate_pg_hint_leader(hint);
				sock = NULL;
				if (actions & KFASTBLOCK_RECOVERY_RETRY)
					continue;
			}
			goto out;
		}

		seq = kfastblock_transport_next_seq(cached);
		ret = kfastblock_transport_maybe_inject_fault(
			vol, KFASTBLOCK_FAULT_OBJECT_IO);
		if (ret) {
			actions = kfastblock_recovery_classify_object_failure(ret);
			kfastblock_recovery_finalize_osd_socket(vol, cached, &leader,
								ret, actions);
			cached = NULL;
			if (actions) {
				kfastblock_recovery_apply_object_failure(
					vol, kf_req->request_pool_id, extent, op, &leader,
					ret, actions);
				if (hint &&
				    (actions & KFASTBLOCK_RECOVERY_INVALIDATE_LEADER))
					kfastblock_request_invalidate_pg_hint_leader(hint);
				if (actions & KFASTBLOCK_RECOVERY_RETRY)
					continue;
			}
			goto out;
		}
		if (op == REQ_OP_WRITE || op == REQ_OP_WRITE_ZEROES)
			ret = kfastblock_transport_write_object(sock, kf_req->request_pool_id,
						 extent, buf, seq);
		else if (op == REQ_OP_READ)
			ret = kfastblock_transport_read_object(sock, kf_req->request_pool_id,
						extent, buf, seq);
		else
			ret = kfastblock_transport_delete_object(sock,
							kf_req->request_pool_id,
							extent, seq);
		if (ret == -ENOENT && op == REQ_OP_READ) {
			memset(buf, 0, extent->length);
			ret = 0;
		} else if (ret == -ENOENT && op == REQ_OP_DISCARD) {
			ret = 0;
		}
		actions = ret ? kfastblock_recovery_classify_object_failure(ret) : 0;
		kfastblock_recovery_finalize_osd_socket(vol, cached, &leader, ret,
							actions);
		cached = NULL;
		if (ret) {
			if (actions) {
				kfastblock_recovery_apply_object_failure(
					vol, kf_req->request_pool_id, extent, op, &leader,
					ret, actions);
				if (hint &&
				    (actions & KFASTBLOCK_RECOVERY_INVALIDATE_LEADER))
					kfastblock_request_invalidate_pg_hint_leader(hint);
				sock = NULL;
				if (actions & KFASTBLOCK_RECOVERY_RETRY)
					continue;
			}
		} else if (hint) {
			kfastblock_request_set_pg_hint_leader(hint, &leader);
		}
		if (!ret && op == REQ_OP_READ)
			ret = kfastblock_transport_copy_request_data(
				rq, extent->request_offset, buf, extent->length, true);
		if (!ret)
			kfastblock_volume_account_object_success(
				vol, op, extent->pg_id, leader.osd_id, extent->length);
		if (!ret)
			kfastblock_scheduler_note_success(&vol->scheduler);
		goto out;
		}

out:
	if (cached)
		kfastblock_transport_release_osd_socket(cached);
	if (ret)
		kfastblock_volume_account_object_error(
			vol, op, extent->pg_id, leader.osd_id, extent->length, ret);
	kfastblock_buffer_pool_free(&vol->object_buffer_pool, buf);
	return ret;
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
	struct request *rq;
	enum req_op op;

	if (!kf_req || !kf_req->rq || !kf_req->vol ||
	    object_index >= kf_req->nr_objects)
		return -EINVAL;

	rq = kf_req->rq;
	op = kfastblock_transport_object_op(kf_req, object_index);
	switch (op) {
	case REQ_OP_FLUSH:
		return 0;
	case REQ_OP_READ:
	case REQ_OP_WRITE:
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		break;
	default:
		return -EOPNOTSUPP;
	}

	return kfastblock_transport_submit_object_io(
		kf_req,
		&kf_req->objects[object_index],
		op);
}

static int kfastblock_transport_prefetch_request_leaders(
	struct kfastblock_request *kf_req)
{
	unsigned int i;
	int ret;

	if (!kf_req || !kf_req->vol)
		return -EINVAL;
	ret = kfastblock_transport_request_view_stale(kf_req);
	if (ret) {
		kfastblock_volume_account_refresh_kick(kf_req->vol, 0, ret);
		kfastblock_volume_kick_refresh(kf_req->vol);
		return ret;
	}

	for (i = 0; i < kf_req->nr_unique_pgs; ++i) {
		struct kfastblock_leader_info leader;
		struct kfastblock_cached_socket *cached = NULL;
		struct socket *sock = NULL;
		struct kfastblock_request_pg_hint *hint = &kf_req->pg_hints[i];

		if (kfastblock_request_get_pg_hint_leader(hint, &leader))
			ret = kfastblock_transport_query_pg_leader(kf_req, hint, &leader);
		else
			ret = 0;
		if (ret) {
			kfastblock_request_invalidate_pg_hint_leader(hint);
			if (kfastblock_recovery_prefetch_should_fail_request(ret))
				return ret;
			continue;
		}
		kfastblock_request_set_pg_hint_leader(hint, &leader);
		ret = kfastblock_transport_acquire_osd_socket(
			kf_req->vol, &leader, &cached, &sock);
		(void)sock;
		if (!ret)
			kfastblock_transport_release_osd_socket(cached);
	}

	return 0;
}

static void kfastblock_transport_complete_request(struct kfastblock_request *kf_req,
						  int ret)
{
	bool first_error = false;
	int remaining;

	if (ret)
		first_error = kfastblock_transport_set_first_error(kf_req, ret);
	else if (!kfastblock_transport_request_failed(kf_req))
		first_error = kfastblock_transport_set_first_error(
			kf_req, kfastblock_transport_queue_next_object(kf_req));

	remaining = atomic_dec_return(&kf_req->pending_objects);
	if (first_error) {
		int unscheduled = kfastblock_transport_discard_unscheduled(kf_req);

		if (unscheduled)
			remaining = atomic_sub_return(unscheduled,
						&kf_req->pending_objects);
	}

	if (remaining == 0) {
		blk_status_t status =
			kfastblock_recovery_errno_to_blk_status(kf_req->status);
		kfastblock_volume_account_io_complete(kf_req->vol, kf_req->status);
		if (!kf_req->status)
			kfastblock_volume_mark_success(kf_req->vol,
					      KFASTBLOCK_VOLUME_SOURCE_OBJECT_IO);
		kfastblock_request_cleanup(kf_req);
		blk_mq_end_request(kf_req->rq, status);
		kfastblock_volume_put_io(kf_req->vol);
		put_device(&kf_req->vol->dev);
	}
}

static void kfastblock_transport_abort_request(struct kfastblock_request *kf_req,
					       int ret)
{
	int remaining;
	int unscheduled;

	if (!kf_req)
		return;

	(void)kfastblock_transport_set_first_error(kf_req, ret);
	unscheduled = kfastblock_transport_discard_unscheduled(kf_req);
	if (!unscheduled)
		return;

	remaining = atomic_sub_return(unscheduled, &kf_req->pending_objects);
	if (remaining == 0) {
		blk_status_t status =
			kfastblock_recovery_errno_to_blk_status(kf_req->status);
		kfastblock_volume_account_io_complete(kf_req->vol, kf_req->status);
		kfastblock_request_cleanup(kf_req);
		blk_mq_end_request(kf_req->rq, status);
		kfastblock_volume_put_io(kf_req->vol);
		put_device(&kf_req->vol->dev);
	}
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
	if (kfastblock_transport_request_failed(kf_req)) {
		kfastblock_transport_complete_request(kf_req, 0);
		return;
	}

	ret = kfastblock_transport_submit_object(kf_req,
					       obj_work->object_index);
	kfastblock_transport_complete_request(kf_req, ret);
}

int kfastblock_transport_submit(struct kfastblock_request *kf_req)
{
	unsigned int i;
	unsigned int initial_dispatch;
	int ret = 0;

	if (!kf_req || !kf_req->rq || !kf_req->vol)
		return -EINVAL;

	if (!kf_req->nr_objects) {
		kfastblock_request_cleanup(kf_req);
		blk_mq_end_request(kf_req->rq, BLK_STS_OK);
		kfastblock_volume_put_io(kf_req->vol);
		return 0;
	}

	ret = kfastblock_transport_prefetch_request_leaders(kf_req);
	if (ret) {
		kfastblock_request_cleanup(kf_req);
		kfastblock_volume_account_io_complete(kf_req->vol, ret);
		blk_mq_end_request(kf_req->rq,
			kfastblock_recovery_errno_to_blk_status(ret));
		kfastblock_volume_put_io(kf_req->vol);
		return 0;
	}

	kf_req->status = 0;
	atomic_set(&kf_req->pending_objects, kf_req->nr_objects);
	kf_req->next_object_to_queue = 0;
	initial_dispatch = min_t(unsigned int, kf_req->nr_objects,
				 kf_req->dispatch_window ?
				 kf_req->dispatch_window :
				 1);
	get_device(&kf_req->vol->dev);
	for (i = 0; i < initial_dispatch; ++i) {
		ret = kfastblock_transport_queue_next_object(kf_req);
		if (ret)
			break;
	}
	if (ret)
		kfastblock_transport_abort_request(kf_req, ret);

	return 0;
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
