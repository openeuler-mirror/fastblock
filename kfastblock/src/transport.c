#include <linux/byteorder/little_endian.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/kernel.h>
#include <linux/net.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/uio.h>

#include "kfastblock/common.h"
#include "kfastblock/rawproto.h"
#include "kfastblock/transport.h"

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
					     u8 opcode,
					     u64 seq,
					     const void *body,
					     u32 body_len)
{
	struct kfastblock_raw_header hdr = {
		.magic = cpu_to_le32(KFASTBLOCK_RAW_MAGIC),
		.version_major = KFASTBLOCK_RAW_VERSION_MAJOR,
		.version_minor = KFASTBLOCK_RAW_VERSION_MINOR,
		.service = KFASTBLOCK_RAW_SERVICE_MONITOR,
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
	if (hdr->service != KFASTBLOCK_RAW_SERVICE_MONITOR)
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
					KFASTBLOCK_RAW_OP_GET_IMAGE_INFO,
					seq,
					req_body,
					body_len);
	kfree(req_body);
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_OP_GET_IMAGE_INFO,
					 seq,
					 &rsp_hdr,
					 &body);
	if (ret)
		return ret;
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

static int kfastblock_transport_skip_bytes(const u8 *body,
					 size_t body_len,
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

static int kfastblock_transport_fetch_cluster_map(struct socket *sock,
					  struct kfastblock_cluster_view *view,
					  u64 seq)
{
	struct kfastblock_raw_get_cluster_map_req req = {
		.osdmap_epoch = cpu_to_le64(view->osdmap_epoch),
		.pgmap_epoch = cpu_to_le64(view->pgmap_epoch),
	};
	struct kfastblock_raw_cluster_map_rsp_hdr rsp;
	struct kfastblock_raw_header rsp_hdr;
	void *body = NULL;
	u8 *cursor;
	u32 pool_pg_count = 0;
	u32 osd_count;
	u32 pg_count;
	u32 i;
	size_t offset = 0;
	int ret;
	const size_t shard_entry_size = sizeof(struct kfastblock_raw_osd_shard_entry);

	ret = kfastblock_transport_send_request(sock,
					KFASTBLOCK_RAW_OP_GET_CLUSTER_MAP,
					seq,
					&req,
					sizeof(req));
	if (ret)
		return ret;

	ret = kfastblock_transport_recv_response(sock,
					 KFASTBLOCK_RAW_OP_GET_CLUSTER_MAP,
					 seq,
					 &rsp_hdr,
					 &body);
	if (ret)
		return ret;
	if (!body || le32_to_cpu(rsp_hdr.body_len) < sizeof(rsp)) {
		kfree(body);
		return -EPROTO;
	}

	cursor = body;
	memcpy(&rsp, cursor, sizeof(rsp));
	offset += sizeof(rsp);
	view->osdmap_epoch = le64_to_cpu(rsp.osdmap_epoch);
	view->pgmap_epoch = le64_to_cpu(rsp.pgmap_epoch);
	osd_count = le32_to_cpu(rsp.osd_count);
	pg_count = le32_to_cpu(rsp.pg_count);

	for (i = 0; i < osd_count; ++i) {
		struct kfastblock_raw_osd_entry_hdr osd_hdr;
		u16 address_len;
		u16 shard_count;

		if (offset + sizeof(osd_hdr) > le32_to_cpu(rsp_hdr.body_len)) {
			kfree(body);
			return -EPROTO;
		}
		memcpy(&osd_hdr, cursor + offset, sizeof(osd_hdr));
		offset += sizeof(osd_hdr);
		address_len = le16_to_cpu(osd_hdr.address_len);
		shard_count = le16_to_cpu(osd_hdr.shard_count);
		ret = kfastblock_transport_skip_bytes(cursor,
					 le32_to_cpu(rsp_hdr.body_len),
					 &offset,
					 address_len + ((size_t)shard_count * shard_entry_size));
		if (ret) {
			kfree(body);
			return ret;
		}
	}

	for (i = 0; i < pg_count; ++i) {
		struct kfastblock_raw_pg_entry_hdr pg_hdr;
		u32 replica_count;
		u32 pool_id;

		if (offset + sizeof(pg_hdr) > le32_to_cpu(rsp_hdr.body_len)) {
			kfree(body);
			return -EPROTO;
		}
		memcpy(&pg_hdr, cursor + offset, sizeof(pg_hdr));
		offset += sizeof(pg_hdr);
		pool_id = le32_to_cpu(pg_hdr.pool_id);
		replica_count = le32_to_cpu(pg_hdr.replica_count);
		if (pool_id == view->image.pool_id)
			++pool_pg_count;
		ret = kfastblock_transport_skip_bytes(cursor,
					 le32_to_cpu(rsp_hdr.body_len),
					 &offset,
					 (size_t)replica_count * sizeof(__le32));
		if (ret) {
			kfree(body);
			return ret;
		}
	}

	kfree(body);
	if (!pool_pg_count)
		return -ENOENT;
	view->image.pg_count = pool_pg_count;
	return 0;
}

int kfastblock_transport_init(void)
{
	return 0;
}

void kfastblock_transport_exit(void)
{
}

int kfastblock_transport_sockaddr_from_endpoint(
	const struct kfastblock_monitor_endpoint *endpoint,
	struct sockaddr_in *addr)
{
	int ret;

	if (!endpoint || !addr || !endpoint->host[0] || !endpoint->port)
		return -EINVAL;

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(endpoint->port);
	ret = in4_pton(endpoint->host, -1, (u8 *)&addr->sin_addr.s_addr,
		       -1, NULL);
	if (!ret)
		return -EINVAL;

	return 0;
}

int kfastblock_transport_try_connect_monitor(
	const struct kfastblock_monitor_endpoint *endpoint,
	struct socket **sock)
{
	struct sockaddr_in addr;
	struct socket *local_sock;
	int ret;

	if (!sock)
		return -EINVAL;

	ret = kfastblock_transport_sockaddr_from_endpoint(endpoint, &addr);
	if (ret)
		return ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &local_sock);
	if (ret)
		return ret;

	ret = kernel_connect(local_sock, (struct sockaddr *)&addr, sizeof(addr), 0);
	if (ret) {
		sock_release(local_sock);
		return ret;
	}

	*sock = local_sock;
	return 0;
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
			ret = kfastblock_transport_fetch_cluster_map(sock, view, seq++);
		sock_release(sock);
		if (!ret)
			return 0;
		first_err = ret;
	}

	return first_err;
}

int kfastblock_transport_submit(struct kfastblock_request *kf_req)
{
	/*
	 * TODO:
	 * 1. serialize parent request into per-object OSD requests
	 * 2. route each object by pg/leader
	 * 3. aggregate sub-request completions back into the blk request
	 */
	return -EOPNOTSUPP;
}
