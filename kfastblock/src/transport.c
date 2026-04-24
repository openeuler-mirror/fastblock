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
#include "kfastblock/rawproto.h"
#include "kfastblock/transport.h"
#include "kfastblock/volume.h"

static int kfastblock_transport_try_connect_host(const char *host,
						 u16 port,
						 struct socket **sock);
static struct workqueue_struct *g_kfastblock_transport_wq;

static bool kfastblock_transport_should_retry_monitor(const int ret);

static void kfastblock_transport_close_cached_socket(
	struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	if (cached->sock) {
		sock_release(cached->sock);
		cached->sock = NULL;
	}
	cached->next_seq = 0;
}

static u64 kfastblock_transport_next_seq(struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return 1;

	if (!cached->next_seq)
		cached->next_seq = 1;
	return cached->next_seq++;
}

static void kfastblock_transport_close_cached_monitor_socket(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	if (cached->sock) {
		sock_release(cached->sock);
		cached->sock = NULL;
	}
	cached->next_seq = 0;
}

static u64 kfastblock_transport_next_monitor_seq(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return 1;

	if (!cached->next_seq)
		cached->next_seq = 1;
	return cached->next_seq++;
}

static unsigned long
kfastblock_transport_backoff_interval(const u32 fail_streak)
{
	unsigned long delay_ms = 100;
	u32 shifts = 0;

	if (fail_streak > 1)
		shifts = min_t(u32, fail_streak - 1, 5);
	delay_ms <<= shifts;
	if (delay_ms > 3000)
		delay_ms = 3000;
	return msecs_to_jiffies(delay_ms);
}

static void
kfastblock_transport_reset_osd_socket_backoff(struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	cached->fail_streak = 0;
	cached->last_error = 0;
	cached->last_failure_jiffies = 0;
	cached->backoff_until_jiffies = 0;
}

static void kfastblock_transport_reset_monitor_socket_backoff(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	cached->fail_streak = 0;
	cached->last_error = 0;
	cached->last_failure_jiffies = 0;
	cached->backoff_until_jiffies = 0;
}

static int kfastblock_transport_osd_backoff_active(
	const struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader)
{
	if (!cached || !leader || cached->sock || !cached->backoff_until_jiffies)
		return 0;
	if (cached->osd_id != leader->osd_id || cached->port != leader->port)
		return 0;
	if (strcmp(cached->address, leader->address))
		return 0;
	if (time_before(jiffies, cached->backoff_until_jiffies))
		return -EAGAIN;
	return 0;
}

static int kfastblock_transport_monitor_backoff_active(
	const struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint)
{
	if (!cached || !endpoint || cached->sock || !cached->backoff_until_jiffies)
		return 0;
	if (cached->port != endpoint->port)
		return 0;
	if (strcmp(cached->address, endpoint->host))
		return 0;
	if (time_before(jiffies, cached->backoff_until_jiffies))
		return -EAGAIN;
	return 0;
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

	cached->osd_id = leader->osd_id;
	cached->port = leader->port;
	strscpy(cached->address, leader->address, sizeof(cached->address));
	cached->fail_streak = min_t(u32, cached->fail_streak + 1, 8);
	cached->last_error = ret;
	cached->last_failure_jiffies = jiffies;
	backoff_jiffies = kfastblock_transport_backoff_interval(cached->fail_streak);
	cached->backoff_until_jiffies = jiffies + backoff_jiffies;
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

	cached->port = endpoint->port;
	strscpy(cached->address, endpoint->host, sizeof(cached->address));
	cached->fail_streak = min_t(u32, cached->fail_streak + 1, 8);
	cached->last_error = ret;
	cached->last_failure_jiffies = jiffies;
	backoff_jiffies = kfastblock_transport_backoff_interval(cached->fail_streak);
	cached->backoff_until_jiffies = jiffies + backoff_jiffies;
	kfastblock_volume_account_socket_backoff(vol, true, 0, endpoint->port,
				cached->fail_streak, backoff_jiffies, ret);
}

static void kfastblock_transport_mark_osd_socket_success(
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader)
{
	if (!cached || !leader)
		return;

	cached->osd_id = leader->osd_id;
	cached->port = leader->port;
	strscpy(cached->address, leader->address, sizeof(cached->address));
	kfastblock_transport_reset_osd_socket_backoff(cached);
}

static void kfastblock_transport_mark_monitor_socket_success(
	struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint)
{
	if (!cached || !endpoint)
		return;

	cached->port = endpoint->port;
	strscpy(cached->address, endpoint->host, sizeof(cached->address));
	kfastblock_transport_reset_monitor_socket_backoff(cached);
}

static int kfastblock_transport_connect_osd_cached(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	struct socket **sock_out)
{
	int ret;

	if (!cached || !leader || !sock_out)
		return -EINVAL;

	ret = kfastblock_transport_osd_backoff_active(cached, leader);
	if (ret)
		return ret;

	kfastblock_transport_close_cached_socket(cached);
	ret = kfastblock_transport_try_connect_host(leader->address, leader->port,
					 sock_out);
	if (ret) {
		kfastblock_transport_mark_osd_socket_failure(vol, cached, leader, ret);
		return ret;
	}

	cached->sock = *sock_out;
	cached->next_seq = 1;
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
	int ret;

	if (!vol || !spec || !cached_out || !sock_out)
		return -EINVAL;
	if (endpoint_index >= spec->nr_monitors)
		return -EINVAL;

	endpoint = &spec->monitors[endpoint_index];
	cached = &vol->monitor_cache[endpoint_index];
	if (cached->sock &&
	    cached->port == endpoint->port &&
	    strcmp(cached->address, endpoint->host) == 0) {
		*cached_out = cached;
		*sock_out = cached->sock;
		return 0;
	}

	ret = kfastblock_transport_monitor_backoff_active(cached, endpoint);
	if (ret)
		return ret;

	kfastblock_transport_close_cached_monitor_socket(cached);
	ret = kfastblock_transport_try_connect_monitor(endpoint, sock_out);
	if (ret) {
		kfastblock_transport_mark_monitor_socket_failure(vol, cached, endpoint, ret);
		return ret;
	}

	cached->sock = *sock_out;
	cached->next_seq = 1;
	kfastblock_transport_mark_monitor_socket_success(cached, endpoint);
	*cached_out = cached;
	return 0;
}

static bool kfastblock_transport_should_retry_monitor(const int ret)
{
	switch (ret) {
	case -EAGAIN:
	case -ETIMEDOUT:
	case -ECONNRESET:
	case -EPIPE:
	case -ECONNREFUSED:
	case -EHOSTUNREACH:
	case -ENETUNREACH:
	case -EPROTO:
	case -EIO:
		return true;
	default:
		return false;
	}
}

static struct kfastblock_cached_socket *
kfastblock_transport_find_cached_socket(struct kfastblock_volume *vol,
					const struct kfastblock_leader_info *leader)
{
	int i;

	if (!vol || !leader)
		return NULL;

	for (i = 0; i < KFASTBLOCK_MAX_SOCKET_CACHE; ++i) {
		struct kfastblock_cached_socket *cached = &vol->socket_cache[i];

		if (!cached->sock)
			continue;
		if (cached->osd_id != leader->osd_id || cached->port != leader->port)
			continue;
		if (strcmp(cached->address, leader->address))
			continue;
		return cached;
	}

	return NULL;
}

static struct kfastblock_cached_socket *
kfastblock_transport_get_socket_slot(struct kfastblock_volume *vol)
{
	int i;

	if (!vol)
		return NULL;

	for (i = 0; i < KFASTBLOCK_MAX_SOCKET_CACHE; ++i) {
		if (!vol->socket_cache[i].sock)
			return &vol->socket_cache[i];
	}

	return &vol->socket_cache[0];
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

enum kfastblock_transport_failure_action {
	KFASTBLOCK_TRANSPORT_FAILURE_DROP_SOCKET = 1U << 0,
	KFASTBLOCK_TRANSPORT_FAILURE_INVALIDATE_LEADER = 1U << 1,
	KFASTBLOCK_TRANSPORT_FAILURE_KICK_REFRESH = 1U << 2,
	KFASTBLOCK_TRANSPORT_FAILURE_RETRY = 1U << 3,
};

static unsigned int
kfastblock_transport_classify_object_failure(const int ret)
{
	switch (ret) {
	case -ENOLINK:
	case -EAGAIN:
	case -EHOSTDOWN:
	case -ETIMEDOUT:
	case -ECONNRESET:
	case -EPIPE:
		return KFASTBLOCK_TRANSPORT_FAILURE_DROP_SOCKET |
			KFASTBLOCK_TRANSPORT_FAILURE_INVALIDATE_LEADER |
			KFASTBLOCK_TRANSPORT_FAILURE_KICK_REFRESH |
			KFASTBLOCK_TRANSPORT_FAILURE_RETRY;
	case -EPROTO:
		return KFASTBLOCK_TRANSPORT_FAILURE_DROP_SOCKET |
			KFASTBLOCK_TRANSPORT_FAILURE_INVALIDATE_LEADER |
			KFASTBLOCK_TRANSPORT_FAILURE_KICK_REFRESH;
	case -EIO:
		return KFASTBLOCK_TRANSPORT_FAILURE_DROP_SOCKET |
			KFASTBLOCK_TRANSPORT_FAILURE_KICK_REFRESH;
	default:
		return 0;
	}
}

static unsigned int
kfastblock_transport_classify_leader_failure(const int ret)
{
	switch (ret) {
	case -ENOLINK:
	case -EAGAIN:
	case -EHOSTDOWN:
	case -ETIMEDOUT:
	case -ECONNRESET:
	case -EPIPE:
	case -EPROTO:
	case -EIO:
		return KFASTBLOCK_TRANSPORT_FAILURE_DROP_SOCKET |
			KFASTBLOCK_TRANSPORT_FAILURE_INVALIDATE_LEADER |
			KFASTBLOCK_TRANSPORT_FAILURE_KICK_REFRESH;
	default:
		return KFASTBLOCK_TRANSPORT_FAILURE_INVALIDATE_LEADER;
	}
}

static unsigned int
kfastblock_transport_classify_monitor_failure(const int ret)
{
	if (kfastblock_transport_should_retry_monitor(ret))
		return KFASTBLOCK_TRANSPORT_FAILURE_DROP_SOCKET;

	return 0;
}

static void kfastblock_transport_apply_object_failure(
	struct kfastblock_volume *vol,
	struct kfastblock_cluster_view *view,
	const struct kfastblock_object_extent *extent,
	const enum req_op op,
	const struct kfastblock_leader_info *leader,
	struct kfastblock_cached_socket *cached,
	const int ret,
	const unsigned int actions)
{
	if (!vol || !view || !extent || !actions)
		return;

	if (actions & KFASTBLOCK_TRANSPORT_FAILURE_DROP_SOCKET) {
		if (leader && cached && cached->sock) {
			kfastblock_transport_mark_osd_socket_failure(vol, cached, leader, ret);
			kfastblock_volume_account_socket_drop(
				vol, false, leader->osd_id, leader->port, ret);
		}
		if (cached)
			kfastblock_transport_close_cached_socket(cached);
	}
	if (actions & KFASTBLOCK_TRANSPORT_FAILURE_INVALIDATE_LEADER) {
		kfastblock_volume_account_leader_invalidate(vol, extent->pg_id, ret);
		kfastblock_meta_invalidate_pg_leader(view, view->image.pool_id,
				      extent->pg_id);
	}
	if (actions & KFASTBLOCK_TRANSPORT_FAILURE_KICK_REFRESH) {
		kfastblock_volume_account_refresh_kick(vol, extent->pg_id, ret);
		kfastblock_volume_kick_refresh(vol);
	}
	if (actions & KFASTBLOCK_TRANSPORT_FAILURE_RETRY)
		kfastblock_volume_account_object_retry(vol, op, extent->pg_id,
				   leader ? leader->osd_id : 0, extent->length, ret);
}

static void kfastblock_transport_apply_leader_failure(
	struct kfastblock_volume *vol,
	struct kfastblock_cluster_view *view,
	const u32 pool_id,
	const u32 pg_id,
	const struct kfastblock_leader_info *target,
	struct kfastblock_cached_socket *cached,
	const int ret,
	const unsigned int actions)
{
	if (!vol || !view || !actions)
		return;

	if (actions & KFASTBLOCK_TRANSPORT_FAILURE_DROP_SOCKET) {
		if (target && cached && cached->sock) {
			kfastblock_transport_mark_osd_socket_failure(vol, cached, target, ret);
			kfastblock_volume_account_socket_drop(vol, false,
				target->osd_id, target->port, ret);
		}
		if (cached)
			kfastblock_transport_close_cached_socket(cached);
	}
	if (actions & KFASTBLOCK_TRANSPORT_FAILURE_INVALIDATE_LEADER) {
		kfastblock_volume_account_leader_invalidate(vol, pg_id, ret);
		kfastblock_meta_invalidate_pg_leader(view, pool_id, pg_id);
	}
	if (actions & KFASTBLOCK_TRANSPORT_FAILURE_KICK_REFRESH) {
		kfastblock_volume_account_refresh_kick(vol, pg_id, ret);
		kfastblock_volume_kick_refresh(vol);
	}
}

static void kfastblock_transport_apply_monitor_failure(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_monitor_socket *cached,
	const int ret,
	const unsigned int actions)
{
	if (!vol || !actions)
		return;

	if (actions & KFASTBLOCK_TRANSPORT_FAILURE_DROP_SOCKET) {
		if (cached && cached->sock) {
			struct kfastblock_monitor_endpoint endpoint = {};

			endpoint.port = cached->port;
			strscpy(endpoint.host, cached->address, sizeof(endpoint.host));
			kfastblock_transport_mark_monitor_socket_failure(vol, cached, &endpoint, ret);
			kfastblock_volume_account_socket_drop(vol, true, 0, cached->port, ret);
		}
		if (cached)
			kfastblock_transport_close_cached_monitor_socket(cached);
	}
}

static blk_status_t kfastblock_transport_errno_to_blk_status(const int ret)
{
	switch (ret) {
	case 0:
		return BLK_STS_OK;
	case -EAGAIN:
	case -ESTALE:
	case -EBUSY:
		return BLK_STS_RESOURCE;
	case -EHOSTDOWN:
	case -ENOLINK:
	case -ETIMEDOUT:
	case -ECONNRESET:
	case -EPIPE:
		return BLK_STS_TRANSPORT;
	default:
		return BLK_STS_IOERR;
	}
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
	    extent->length == kf_req->vol->view.image.object_size)
		return REQ_OP_DISCARD;

	return REQ_OP_WRITE_ZEROES;
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

	*body = kmalloc(body_len, GFP_KERNEL);
	if (!*body)
		return -ENOMEM;
	ret = kfastblock_transport_recv_all(sock, *body, body_len);
	if (ret) {
		kfree(*body);
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
	u8 *req_body;
	u32 body_len;
	int ret;

	body_len = sizeof(req) + strlen(view->image.pool_name) + strlen(view->image.image_name);
	req_body = kmalloc(body_len, GFP_KERNEL);
	if (!req_body)
		return -ENOMEM;
	memcpy(req_body, &req, sizeof(req));
	memcpy(req_body + sizeof(req), view->image.pool_name, strlen(view->image.pool_name));
	memcpy(req_body + sizeof(req) + strlen(view->image.pool_name),
	       view->image.image_name, strlen(view->image.image_name));

	ret = kfastblock_transport_send_request(sock,
					KFASTBLOCK_RAW_SERVICE_MONITOR,
					KFASTBLOCK_RAW_OP_GET_IMAGE_INFO,
					seq,
					req_body,
					body_len);
	kfree(req_body);
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
		kfree(body);
		return ret;
	}
	if (!body || le32_to_cpu(rsp_hdr.body_len) != sizeof(rsp)) {
		kfree(body);
		return -EPROTO;
	}
	memcpy(&rsp, body, sizeof(rsp));
	kfree(body);

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
		kfree(body);
		return ret;
	}
	body_len = le32_to_cpu(rsp_hdr.body_len);
	if (!body || body_len < sizeof(rsp)) {
		kfree(body);
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
	kfree(body);
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
		kfree(body);
		return ret;
	}

	body_len = le32_to_cpu(rsp_hdr.body_len);
	if (!body || body_len < sizeof(rsp)) {
		kfree(body);
		return -EPROTO;
	}

	memcpy(&rsp, body, sizeof(rsp));
	address_len = le16_to_cpu(rsp.address_len);
	if (body_len != sizeof(rsp) + address_len ||
	    !address_len ||
	    address_len >= sizeof(leader->address)) {
		kfree(body);
		return -EPROTO;
	}

	leader->osd_id = le32_to_cpu(rsp.leader_id);
	leader->port = le16_to_cpu(rsp.leader_port);
	memcpy(leader->address, (u8 *)body + sizeof(rsp), address_len);
	leader->address[address_len] = '\0';
	kfree(body);

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
	u8 *req_body;
	u32 object_name_len;
	u32 data_len;
	u32 body_len;
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

	body_len = sizeof(req) + object_name_len + data_len;
	req_body = kmalloc(body_len, GFP_KERNEL);
	if (!req_body)
		return -ENOMEM;

	memcpy(req_body, &req, sizeof(req));
	memcpy(req_body + sizeof(req), extent->object_name, object_name_len);
	if (data_len)
		memcpy(req_body + sizeof(req) + object_name_len, data, data_len);

	ret = kfastblock_transport_send_request(sock,
					KFASTBLOCK_RAW_SERVICE_OSD,
					KFASTBLOCK_RAW_OSD_OP_WRITE_OBJECT,
					seq,
					req_body,
					body_len);
	kfree(req_body);
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_SERVICE_OSD,
					 KFASTBLOCK_RAW_OSD_OP_WRITE_OBJECT,
					 seq,
					 &rsp_hdr,
					 &body);
	kfree(body);
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
	u8 *req_body;
	u32 object_name_len;
	u32 body_len;
	u32 data_len;
	int ret;

	if (!sock || !extent || (!data && extent->length))
		return -EINVAL;

	object_name_len = strlen(extent->object_name);
	body_len = sizeof(req) + object_name_len;
	req_body = kmalloc(body_len, GFP_KERNEL);
	if (!req_body)
		return -ENOMEM;

	memcpy(req_body, &req, sizeof(req));
	memcpy(req_body + sizeof(req), extent->object_name, object_name_len);

	ret = kfastblock_transport_send_request(sock,
					KFASTBLOCK_RAW_SERVICE_OSD,
					KFASTBLOCK_RAW_OSD_OP_READ_OBJECT,
					seq,
					req_body,
					body_len);
	kfree(req_body);
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_SERVICE_OSD,
					 KFASTBLOCK_RAW_OSD_OP_READ_OBJECT,
					 seq,
					 &rsp_hdr,
					 &body);
	if (ret) {
		kfree(body);
		return ret;
	}

	body_len = le32_to_cpu(rsp_hdr.body_len);
	if (!body || body_len < sizeof(rsp)) {
		kfree(body);
		return -EPROTO;
	}

	memcpy(&rsp, body, sizeof(rsp));
	data_len = le32_to_cpu(rsp.data_len);
	if (body_len != sizeof(rsp) + data_len || data_len > extent->length) {
		kfree(body);
		return -EPROTO;
	}

	memset(data, 0, extent->length);
	if (data_len)
		memcpy(data, (u8 *)body + sizeof(rsp), data_len);
	kfree(body);
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
	u8 *req_body;
	u32 object_name_len;
	u32 body_len;
	int ret;

	if (!sock || !extent)
		return -EINVAL;

	object_name_len = strlen(extent->object_name);
	body_len = sizeof(req) + object_name_len;
	req_body = kmalloc(body_len, GFP_KERNEL);
	if (!req_body)
		return -ENOMEM;

	memcpy(req_body, &req, sizeof(req));
	memcpy(req_body + sizeof(req), extent->object_name, object_name_len);

	ret = kfastblock_transport_send_request(sock,
					KFASTBLOCK_RAW_SERVICE_OSD,
					KFASTBLOCK_RAW_OSD_OP_DELETE_OBJECT,
					seq,
					req_body,
					body_len);
	kfree(req_body);
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_SERVICE_OSD,
					 KFASTBLOCK_RAW_OSD_OP_DELETE_OBJECT,
					 seq,
					 &rsp_hdr,
					 &body);
	kfree(body);
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
	struct kfastblock_volume *vol,
	struct request *rq,
	const struct kfastblock_object_extent *extent,
	enum req_op op)
{
	struct kfastblock_cluster_view *view;
	struct kfastblock_leader_info leader = {};
	struct kfastblock_cached_socket *cached = NULL;
	struct socket *sock = NULL;
	void *buf = NULL;
	int ret;
	int attempt;
	u64 seq;

	if (!vol || !rq || !extent)
		return -EINVAL;
	view = &vol->view;

	if (op == REQ_OP_WRITE) {
		buf = kmalloc(extent->length, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		ret = kfastblock_transport_copy_request_data(
			rq, extent->request_offset, buf, extent->length, false);
		if (ret)
			goto out;
	} else if (op == REQ_OP_WRITE_ZEROES) {
		buf = kzalloc(extent->length, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
	} else if (op == REQ_OP_READ) {
		buf = kmalloc(extent->length, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
	}

	for (attempt = 0; attempt < 2; ++attempt) {
		ret = kfastblock_transport_get_pg_leader(vol, view->image.pool_id,
						 extent->pg_id, &leader);
		if (ret)
			goto out;

		cached = kfastblock_transport_find_cached_socket(vol, &leader);
		if (cached) {
			sock = cached->sock;
		} else {
			cached = kfastblock_transport_get_socket_slot(vol);
			if (!cached) {
				ret = -ENOMEM;
				goto out;
			}
			ret = kfastblock_transport_connect_osd_cached(vol, cached, &leader,
							      &sock);
			if (ret) {
				unsigned int actions =
					kfastblock_transport_classify_object_failure(ret);

				if (actions) {
					kfastblock_transport_apply_object_failure(
						vol, view, extent, op, &leader, cached, ret, actions);
					sock = NULL;
					if (actions & KFASTBLOCK_TRANSPORT_FAILURE_RETRY)
						continue;
				}
				goto out;
			}
		}

		mutex_lock(&cached->lock);
		seq = kfastblock_transport_next_seq(cached);
		if (op == REQ_OP_WRITE || op == REQ_OP_WRITE_ZEROES)
			ret = kfastblock_transport_write_object(sock, view->image.pool_id,
						 extent, buf, seq);
		else if (op == REQ_OP_READ)
			ret = kfastblock_transport_read_object(sock, view->image.pool_id,
						extent, buf, seq);
		else
			ret = kfastblock_transport_delete_object(sock,
							view->image.pool_id,
							extent, seq);
		mutex_unlock(&cached->lock);
		if (ret) {
			unsigned int actions =
				kfastblock_transport_classify_object_failure(ret);

			if (actions) {
				kfastblock_transport_apply_object_failure(
					vol, view, extent, op, &leader, cached, ret, actions);
				sock = NULL;
				if (actions & KFASTBLOCK_TRANSPORT_FAILURE_RETRY)
					continue;
			} else if (cached) {
				kfastblock_volume_account_socket_drop(vol, false,
					leader.osd_id, leader.port, ret);
				kfastblock_transport_close_cached_socket(cached);
				sock = NULL;
			}
		}
		if (!ret && op == REQ_OP_READ)
			ret = kfastblock_transport_copy_request_data(
				rq, extent->request_offset, buf, extent->length, true);
		goto out;
	}

out:
	if (ret)
		kfastblock_volume_account_object_error(
			vol, op, extent->pg_id, leader.osd_id, extent->length, ret);
	kfree(buf);
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

int kfastblock_transport_get_pg_leader(struct kfastblock_volume *vol,
				       u32 pool_id, u32 pg_id,
				       struct kfastblock_leader_info *leader)
{
	struct kfastblock_cluster_view *view;
	const struct kfastblock_pg_route *route;
	u32 i;
	int first_err = -ENOENT;
	int ret;

	if (!vol || !leader)
		return -EINVAL;
	view = &vol->view;

	ret = kfastblock_meta_get_pg_leader(view, pool_id, pg_id, leader);
	if (!ret)
		return 0;

	route = kfastblock_meta_find_pg_route(view, pool_id, pg_id);
	if (!route || !route->replica_count)
		return -ENOENT;

	for (i = 0; i < route->replica_count; ++i) {
		const struct kfastblock_pg_route *resolved_route;
		const struct kfastblock_osd_endpoint *osd;
		const struct kfastblock_osd_shard *shard;
		struct kfastblock_cached_socket *cached = NULL;
		struct socket *sock = NULL;
		struct kfastblock_leader_info target = {};
		u64 seq;

		ret = kfastblock_meta_resolve_pg_target(view, pool_id, pg_id,
						 route->osd_ids[i],
						 &resolved_route,
						 &osd,
						 &shard);
		if (ret) {
			first_err = ret;
			continue;
		}
		if (!(osd->flags & KFASTBLOCK_OSD_FLAG_IN) ||
		    !(osd->flags & KFASTBLOCK_OSD_FLAG_UP) ||
		    !osd->address[0] || !shard->port) {
			first_err = -EHOSTDOWN;
			continue;
		}

		target.osd_id = osd->osd_id;
		target.port = shard->port;
		strscpy(target.address, osd->address, sizeof(target.address));
		cached = kfastblock_transport_find_cached_socket(vol, &target);
		if (cached) {
			sock = cached->sock;
		} else {
			cached = kfastblock_transport_get_socket_slot(vol);
			if (!cached)
				return -ENOMEM;
			ret = kfastblock_transport_connect_osd_cached(vol, cached, &target,
							      &sock);
			if (ret) {
				kfastblock_transport_apply_leader_failure(
					vol, view, pool_id, pg_id, &target, cached, ret,
					kfastblock_transport_classify_leader_failure(ret));
				first_err = ret;
				continue;
			}
		}

		mutex_lock(&cached->lock);
		seq = kfastblock_transport_next_seq(cached);
		ret = kfastblock_transport_fetch_pg_leader_from_osd(sock,
						    resolved_route->pool_id,
						    resolved_route->pg_id,
						    seq,
						    leader);
		mutex_unlock(&cached->lock);
		kfastblock_volume_account_leader_query(
			vol, resolved_route->pg_id, target.osd_id, ret);
		if (!ret) {
			ret = kfastblock_meta_set_pg_leader(view, pool_id, pg_id,
						 leader);
			return ret ? ret : 0;
		}

		kfastblock_transport_apply_leader_failure(
			vol, view, pool_id, pg_id, &target, cached, ret,
			kfastblock_transport_classify_leader_failure(ret));
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
		kf_req->vol,
		rq,
		&kf_req->objects[object_index],
		op);
}

static void kfastblock_transport_complete_request(struct kfastblock_request *kf_req,
						  int ret)
{
	unsigned long flags;

	if (ret) {
		spin_lock_irqsave(&kf_req->status_lock, flags);
		if (!kf_req->status)
			kf_req->status = ret;
		spin_unlock_irqrestore(&kf_req->status_lock, flags);
	}

	if (atomic_dec_and_test(&kf_req->pending_objects)) {
		blk_status_t status =
			kfastblock_transport_errno_to_blk_status(kf_req->status);
		kfastblock_volume_account_io_complete(kf_req->vol, kf_req->status);
		blk_mq_end_request(kf_req->rq, status);
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

	down_read(&kf_req->vol->state_lock);
	ret = kfastblock_transport_submit_object(kf_req,
					       obj_work->object_index);
	up_read(&kf_req->vol->state_lock);
	kfastblock_transport_complete_request(kf_req, ret);
}

int kfastblock_transport_submit(struct kfastblock_request *kf_req)
{
	unsigned int i;

	if (!kf_req || !kf_req->rq || !kf_req->vol)
		return -EINVAL;

	if (!kf_req->nr_objects) {
		blk_mq_end_request(kf_req->rq, BLK_STS_OK);
		return 0;
	}

	kf_req->status = 0;
	atomic_set(&kf_req->pending_objects, kf_req->nr_objects);
	get_device(&kf_req->vol->dev);
	for (i = 0; i < kf_req->nr_objects; ++i) {
		INIT_WORK(&kf_req->object_works[i].work,
			  kfastblock_transport_object_work);
		if (!g_kfastblock_transport_wq ||
		    !queue_work(g_kfastblock_transport_wq,
				&kf_req->object_works[i].work))
			kfastblock_transport_complete_request(kf_req, -EBUSY);
	}

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

		ret = kfastblock_transport_get_monitor_socket(vol, &vol->spec, i,
					      &cached, &sock);
		if (ret) {
			first_err = ret;
			continue;
		}

		mutex_lock(&cached->lock);
		seq = kfastblock_transport_next_monitor_seq(cached);
		ret = kfastblock_transport_fetch_image_info(sock, &vol->view, seq);
		mutex_unlock(&cached->lock);
		if (!ret || ret == -ESTALE) {
			kfastblock_volume_account_image_refresh(vol, 0);
			return 0;
		}
		kfastblock_transport_apply_monitor_failure(
			vol, cached, ret,
			kfastblock_transport_classify_monitor_failure(ret));
		first_err = ret;
	}

	kfastblock_volume_account_image_refresh(vol, first_err);
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

		ret = kfastblock_transport_get_monitor_socket(vol, &vol->spec, i,
					      &cached, &sock);
		if (ret) {
			first_err = ret;
			continue;
		}

		mutex_lock(&cached->lock);
		seq = kfastblock_transport_next_monitor_seq(cached);
		ret = kfastblock_transport_fetch_cluster_map_from_monitor(sock,
					      &vol->view, seq);
		mutex_unlock(&cached->lock);
		if (!ret || ret == -ESTALE) {
			kfastblock_volume_account_cluster_refresh(vol, 0);
			return 0;
		}
		kfastblock_transport_apply_monitor_failure(
			vol, cached, ret,
			kfastblock_transport_classify_monitor_failure(ret));
		first_err = ret;
	}

	kfastblock_volume_account_cluster_refresh(vol, first_err);
	return first_err;
}
