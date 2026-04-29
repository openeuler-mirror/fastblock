#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RAW_MAGIC 0x46425257U
#define RAW_VERSION_MAJOR 1U
#define RAW_VERSION_MINOR 0U

#define RAW_SERVICE_MONITOR 1U
#define RAW_SERVICE_OSD 2U

#define RAW_OP_GET_IMAGE_INFO 1U
#define RAW_OP_GET_CLUSTER_MAP 2U

#define RAW_OSD_OP_GET_LEADER 1U
#define RAW_OSD_OP_READ_OBJECT 2U
#define RAW_OSD_OP_WRITE_OBJECT 3U
#define RAW_OSD_OP_DELETE_OBJECT 4U

#define RAW_FLAG_RESPONSE (1U << 0)

#define RAW_STATUS_OK 0U
#define RAW_STATUS_INVALID_REQUEST 1U
#define RAW_STATUS_NOT_FOUND 2U
#define RAW_STATUS_STALE_EPOCH 3U
#define RAW_STATUS_RETRY_LATER 4U
#define RAW_STATUS_NOT_LEADER 5U
#define RAW_STATUS_PG_INITIALIZING 6U
#define RAW_STATUS_OSD_DOWN 7U
#define RAW_STATUS_INTERNAL_ERROR 8U

#define RAW_IMAGE_FLAG_READ_ONLY (1U << 0)

#define MAX_ADDR_LEN 512

struct raw_header {
	uint32_t magic;
	uint8_t version_major;
	uint8_t version_minor;
	uint8_t service;
	uint8_t opcode;
	uint32_t flags;
	uint64_t seq;
	uint32_t status;
	uint32_t body_len;
} __attribute__((packed));

struct raw_get_image_info_req {
	uint64_t image_epoch;
	uint16_t pool_name_len;
	uint16_t image_name_len;
} __attribute__((packed));

struct raw_get_image_info_rsp {
	uint64_t image_epoch;
	uint32_t pool_id;
	uint32_t block_size;
	uint32_t object_size;
	uint32_t flags;
	uint64_t size_bytes;
} __attribute__((packed));

struct raw_get_cluster_map_req {
	uint64_t osdmap_epoch;
	uint32_t pool_id;
	uint32_t reserved;
	uint64_t pgmap_epoch;
} __attribute__((packed));

struct raw_cluster_map_rsp_hdr {
	uint64_t osdmap_epoch;
	uint64_t pgmap_epoch;
	uint32_t osd_count;
	uint32_t pg_count;
} __attribute__((packed));

struct raw_osd_entry_hdr {
	uint32_t osd_id;
	uint32_t flags;
	uint16_t address_len;
	uint16_t shard_count;
} __attribute__((packed));

struct raw_osd_shard_entry {
	uint32_t shard_id;
	uint16_t port;
	uint16_t core_id;
} __attribute__((packed));

struct raw_pg_entry_hdr {
	uint32_t pool_id;
	uint32_t pg_id;
	uint64_t version;
	uint32_t state;
	uint32_t primary_shard;
	uint32_t replica_count;
} __attribute__((packed));

struct raw_get_leader_req {
	uint32_t pool_id;
	uint32_t pg_id;
} __attribute__((packed));

struct raw_get_leader_rsp {
	uint32_t leader_id;
	uint16_t leader_port;
	uint16_t address_len;
} __attribute__((packed));

struct raw_read_object_req {
	uint32_t pool_id;
	uint32_t pg_id;
	uint64_t offset;
	uint32_t length;
	uint16_t object_name_len;
	uint16_t reserved;
} __attribute__((packed));

struct raw_read_object_rsp {
	uint32_t data_len;
	uint32_t reserved;
} __attribute__((packed));

struct raw_write_object_req {
	uint32_t pool_id;
	uint32_t pg_id;
	uint64_t offset;
	uint32_t data_len;
	uint16_t object_name_len;
	uint16_t reserved;
} __attribute__((packed));

struct raw_delete_object_req {
	uint32_t pool_id;
	uint32_t pg_id;
	uint16_t object_name_len;
	uint16_t reserved;
} __attribute__((packed));

struct config {
	const char *command;
	char *addr;
	char *pool_name;
	char *image_name;
	char *object_name;
	char *data;
	char *data_file;
	char *output_file;
	uint32_t pool_id;
	uint32_t pg_id;
	uint32_t count;
	uint64_t image_epoch;
	uint64_t osdmap_epoch;
	uint64_t pgmap_epoch;
	uint64_t offset;
	uint32_t length;
	bool pool_id_set;
	bool pg_id_set;
	bool image_epoch_set;
	bool osdmap_epoch_set;
	bool pgmap_epoch_set;
	bool offset_set;
	bool length_set;
	bool count_set;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s <get-image-info|get-cluster-map|get-leader|pipeline-get-leader|pipeline-read-object|pipeline-write-object|pipeline-delete-object|read-object|write-object|delete-object> [options]\n",
		prog);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  --addr <host:port>\n");
	fprintf(stderr, "  --pool-name <name>\n");
	fprintf(stderr, "  --image-name <name>\n");
	fprintf(stderr, "  --pool-id <id>\n");
	fprintf(stderr, "  --pg-id <id>\n");
	fprintf(stderr, "  --image-epoch <epoch>\n");
	fprintf(stderr, "  --osdmap-epoch <epoch>\n");
	fprintf(stderr, "  --pgmap-epoch <epoch>\n");
	fprintf(stderr, "  --object <name>\n");
	fprintf(stderr, "  --offset <bytes>\n");
	fprintf(stderr, "  --length <bytes>\n");
	fprintf(stderr, "  --count <n>\n");
	fprintf(stderr, "  --data <string>\n");
	fprintf(stderr, "  --data-file <path>\n");
	fprintf(stderr, "  --output <path>\n");
	fprintf(stderr, "\nExamples:\n");
	fprintf(stderr, "  %s get-image-info --addr 127.0.0.1:3334 --pool-name pool1 --image-name img1\n", prog);
	fprintf(stderr, "  %s get-cluster-map --addr 127.0.0.1:3334 --pool-id 1\n", prog);
	fprintf(stderr, "  %s get-leader --addr 127.0.0.1:12001 --pool-id 1 --pg-id 7\n", prog);
	fprintf(stderr, "  %s pipeline-get-leader --addr 127.0.0.1:12001 --pool-id 1 --pg-id 7 --count 16\n", prog);
	fprintf(stderr, "  %s pipeline-read-object --addr 127.0.0.1:12001 --pool-id 1 --pg-id 7 --object obj.0001 --length 4096 --count 8\n", prog);
	fprintf(stderr, "  %s pipeline-write-object --addr 127.0.0.1:12001 --pool-id 1 --pg-id 7 --object obj.0001 --data hello --count 8\n", prog);
	fprintf(stderr, "  %s pipeline-delete-object --addr 127.0.0.1:12001 --pool-id 1 --pg-id 7 --object obj.0001 --count 8\n", prog);
	fprintf(stderr, "  %s write-object --addr 127.0.0.1:12001 --pool-id 1 --pg-id 7 --object obj.0001 --offset 0 --data hello\n", prog);
	fprintf(stderr, "  %s read-object --addr 127.0.0.1:12001 --pool-id 1 --pg-id 7 --object obj.0001 --offset 0 --length 5\n", prog);
}

static const char *status_name(uint32_t status)
{
	switch (status) {
	case RAW_STATUS_OK:
		return "ok";
	case RAW_STATUS_INVALID_REQUEST:
		return "invalid_request";
	case RAW_STATUS_NOT_FOUND:
		return "not_found";
	case RAW_STATUS_STALE_EPOCH:
		return "stale_epoch";
	case RAW_STATUS_RETRY_LATER:
		return "retry_later";
	case RAW_STATUS_NOT_LEADER:
		return "not_leader";
	case RAW_STATUS_PG_INITIALIZING:
		return "pg_initializing";
	case RAW_STATUS_OSD_DOWN:
		return "osd_down";
	case RAW_STATUS_INTERNAL_ERROR:
		return "internal_error";
	default:
		return "unknown";
	}
}

static int parse_u32(const char *value, uint32_t *out)
{
	char *end = NULL;
	unsigned long parsed;

	if (!value || !out)
		return -EINVAL;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || !end || *end != '\0' || parsed > UINT32_MAX)
		return -EINVAL;
	*out = (uint32_t)parsed;
	return 0;
}

static int parse_u64(const char *value, uint64_t *out)
{
	char *end = NULL;
	unsigned long long parsed;

	if (!value || !out)
		return -EINVAL;
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno || !end || *end != '\0')
		return -EINVAL;
	*out = (uint64_t)parsed;
	return 0;
}

static int parse_host_port(const char *addr, char *host, size_t host_len,
			   char *port, size_t port_len)
{
	const char *colon;
	size_t host_part_len;

	if (!addr || !host || !port)
		return -EINVAL;
	colon = strrchr(addr, ':');
	if (!colon || colon == addr || colon[1] == '\0')
		return -EINVAL;

	host_part_len = (size_t)(colon - addr);
	if (host_part_len >= host_len || strlen(colon + 1) >= port_len)
		return -ENAMETOOLONG;

	memcpy(host, addr, host_part_len);
	host[host_part_len] = '\0';
	strcpy(port, colon + 1);
	return 0;
}

static int connect_addr(const char *addr)
{
	char host[MAX_ADDR_LEN];
	char port[32];
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	struct addrinfo *it;
	int fd = -1;
	int ret;

	ret = parse_host_port(addr, host, sizeof(host), port, sizeof(port));
	if (ret)
		return ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	ret = getaddrinfo(host, port, &hints, &result);
	if (ret)
		return -EINVAL;

	for (it = result; it; it = it->ai_next) {
		fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, it->ai_addr, it->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}

	freeaddrinfo(result);
	if (fd >= 0)
		return fd;
	return -errno ? -errno : -ECONNREFUSED;
}

static int send_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t sent = 0;

	while (sent < len) {
		ssize_t rc = send(fd, p + sent, len - sent, MSG_NOSIGNAL);

		if (rc > 0) {
			sent += (size_t)rc;
			continue;
		}
		if (rc < 0 && errno == EINTR)
			continue;
		return -errno ? -errno : -EIO;
	}

	return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
	unsigned char *p = buf;
	size_t recvd = 0;

	while (recvd < len) {
		ssize_t rc = recv(fd, p + recvd, len - recvd, 0);

		if (rc > 0) {
			recvd += (size_t)rc;
			continue;
		}
		if (rc == 0)
			return -ECONNRESET;
		if (errno == EINTR)
			continue;
		return -errno ? -errno : -EIO;
	}

	return 0;
}

static int send_message(int fd, uint8_t service, uint8_t opcode, uint64_t seq,
			const void *body, uint32_t body_len)
{
	struct raw_header hdr;
	int ret;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = htole32(RAW_MAGIC);
	hdr.version_major = RAW_VERSION_MAJOR;
	hdr.version_minor = RAW_VERSION_MINOR;
	hdr.service = service;
	hdr.opcode = opcode;
	hdr.flags = htole32(0);
	hdr.seq = htole64(seq);
	hdr.status = htole32(0);
	hdr.body_len = htole32(body_len);

	ret = send_all(fd, &hdr, sizeof(hdr));
	if (ret)
		return ret;
	if (!body_len)
		return 0;
	return send_all(fd, body, body_len);
}

static int recv_message_any(int fd, uint8_t service, uint8_t opcode,
			    struct raw_header *rsp_hdr, unsigned char **body_out)
{
	struct raw_header hdr;
	unsigned char *body = NULL;
	uint32_t body_len;
	int ret;

	if (!rsp_hdr || !body_out)
		return -EINVAL;

	ret = recv_all(fd, &hdr, sizeof(hdr));
	if (ret)
		return ret;

	if (le32toh(hdr.magic) != RAW_MAGIC)
		return -EPROTO;
	if (hdr.version_major != RAW_VERSION_MAJOR ||
	    hdr.version_minor != RAW_VERSION_MINOR)
		return -EPROTO;
	if (hdr.service != service || hdr.opcode != opcode)
		return -EPROTO;
	if ((le32toh(hdr.flags) & RAW_FLAG_RESPONSE) == 0)
		return -EPROTO;

	body_len = le32toh(hdr.body_len);
	if (body_len) {
		body = malloc(body_len);
		if (!body)
			return -ENOMEM;
		ret = recv_all(fd, body, body_len);
		if (ret) {
			free(body);
			return ret;
		}
	}

	*rsp_hdr = hdr;
	*body_out = body;
	return 0;
}

static int recv_message(int fd, uint8_t service, uint8_t opcode, uint64_t seq,
			struct raw_header *rsp_hdr, unsigned char **body_out)
{
	int ret;

	ret = recv_message_any(fd, service, opcode, rsp_hdr, body_out);
	if (ret)
		return ret;
	if (le64toh(rsp_hdr->seq) != seq) {
		free(*body_out);
		*body_out = NULL;
		return -EPROTO;
	}
	return 0;
}

static int load_file(const char *path, unsigned char **buf_out, size_t *len_out)
{
	int fd;
	struct stat st;
	unsigned char *buf;
	ssize_t nread;
	size_t off = 0;

	if (!path || !buf_out || !len_out)
		return -EINVAL;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return -errno;
	}
	if (st.st_size < 0) {
		close(fd);
		return -EINVAL;
	}

	buf = malloc((size_t)st.st_size);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	while (off < (size_t)st.st_size) {
		nread = read(fd, buf + off, (size_t)st.st_size - off);
		if (nread > 0) {
			off += (size_t)nread;
			continue;
		}
		if (nread < 0 && errno == EINTR)
			continue;
		free(buf);
		close(fd);
		return nread == 0 ? -EIO : -errno;
	}

	close(fd);
	*buf_out = buf;
	*len_out = off;
	return 0;
}

static int write_file(const char *path, const unsigned char *buf, size_t len)
{
	int fd;
	size_t off = 0;

	if (!path)
		return -EINVAL;
	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		return -errno;
	while (off < len) {
		ssize_t nwritten = write(fd, buf + off, len - off);

		if (nwritten > 0) {
			off += (size_t)nwritten;
			continue;
		}
		if (nwritten < 0 && errno == EINTR)
			continue;
		close(fd);
		return nwritten == 0 ? -EIO : -errno;
	}
	close(fd);
	return 0;
}

static void hexdump(const unsigned char *buf, size_t len)
{
	size_t i;
	size_t j;

	for (i = 0; i < len; i += 16) {
		printf("%08zx  ", i);
		for (j = 0; j < 16; ++j) {
			if (i + j < len)
				printf("%02x ", buf[i + j]);
			else
				printf("   ");
		}
		printf(" ");
		for (j = 0; j < 16 && i + j < len; ++j) {
			unsigned char c = buf[i + j];

			printf("%c", isprint(c) ? c : '.');
		}
		printf("\n");
	}
}

static int ensure_addr(const struct config *cfg)
{
	if (cfg->addr)
		return 0;
	fprintf(stderr, "--addr is required\n");
	return -EINVAL;
}

static int ensure_pool_pg(const struct config *cfg)
{
	if (!cfg->pool_id_set || !cfg->pg_id_set) {
		fprintf(stderr, "--pool-id and --pg-id are required\n");
		return -EINVAL;
	}
	return 0;
}

static int cmd_get_image_info(const struct config *cfg)
{
	struct raw_get_image_info_req req;
	struct raw_get_image_info_rsp rsp;
	struct raw_header hdr;
	unsigned char *body = NULL;
	unsigned char *req_body = NULL;
	size_t pool_len;
	size_t image_len;
	size_t body_len;
	int fd = -1;
	int ret;

	if (!cfg->pool_name || !cfg->image_name) {
		fprintf(stderr, "--pool-name and --image-name are required\n");
		return -EINVAL;
	}
	ret = ensure_addr(cfg);
	if (ret)
		return ret;

	pool_len = strlen(cfg->pool_name);
	image_len = strlen(cfg->image_name);
	body_len = sizeof(req) + pool_len + image_len;
	req_body = calloc(1, body_len);
	if (!req_body)
		return -ENOMEM;

	req.image_epoch = htole64(cfg->image_epoch_set ? cfg->image_epoch : 0);
	req.pool_name_len = htole16((uint16_t)pool_len);
	req.image_name_len = htole16((uint16_t)image_len);
	memcpy(req_body, &req, sizeof(req));
	memcpy(req_body + sizeof(req), cfg->pool_name, pool_len);
	memcpy(req_body + sizeof(req) + pool_len, cfg->image_name, image_len);

	fd = connect_addr(cfg->addr);
	if (fd < 0) {
		ret = fd;
		goto out;
	}

	ret = send_message(fd, RAW_SERVICE_MONITOR, RAW_OP_GET_IMAGE_INFO, 1,
			   req_body, (uint32_t)body_len);
	if (ret)
		goto out;
	ret = recv_message(fd, RAW_SERVICE_MONITOR, RAW_OP_GET_IMAGE_INFO, 1,
			   &hdr, &body);
	if (ret)
		goto out;

	printf("status=%s(%u)\n", status_name(le32toh(hdr.status)),
	       le32toh(hdr.status));
	if (le32toh(hdr.status) != RAW_STATUS_OK) {
		ret = -EIO;
		goto out;
	}
	if (le32toh(hdr.body_len) != sizeof(rsp)) {
		ret = -EPROTO;
		goto out;
	}

	memcpy(&rsp, body, sizeof(rsp));
	printf("image_epoch=%llu\n",
	       (unsigned long long)le64toh(rsp.image_epoch));
	printf("pool_id=%u\n", le32toh(rsp.pool_id));
	printf("block_size=%u\n", le32toh(rsp.block_size));
	printf("object_size=%u\n", le32toh(rsp.object_size));
	printf("size_bytes=%llu\n",
	       (unsigned long long)le64toh(rsp.size_bytes));
	printf("read_only=%u\n",
	       !!(le32toh(rsp.flags) & RAW_IMAGE_FLAG_READ_ONLY));

out:
	free(req_body);
	free(body);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int cmd_get_cluster_map(const struct config *cfg)
{
	struct raw_get_cluster_map_req req;
	struct raw_cluster_map_rsp_hdr rsp;
	struct raw_header hdr;
	unsigned char *body = NULL;
	unsigned char *cursor;
	unsigned char *end;
	uint32_t osd_count;
	uint32_t pg_count;
	int fd = -1;
	int ret;
	uint32_t i;

	ret = ensure_addr(cfg);
	if (ret)
		return ret;
	if (!cfg->pool_id_set) {
		fprintf(stderr, "--pool-id is required\n");
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));
	req.osdmap_epoch = htole64(cfg->osdmap_epoch_set ? cfg->osdmap_epoch : 0);
	req.pool_id = htole32(cfg->pool_id);
	req.pgmap_epoch = htole64(cfg->pgmap_epoch_set ? cfg->pgmap_epoch : 0);

	fd = connect_addr(cfg->addr);
	if (fd < 0)
		return fd;

	ret = send_message(fd, RAW_SERVICE_MONITOR, RAW_OP_GET_CLUSTER_MAP, 1,
			   &req, sizeof(req));
	if (ret)
		goto out;
	ret = recv_message(fd, RAW_SERVICE_MONITOR, RAW_OP_GET_CLUSTER_MAP, 1,
			   &hdr, &body);
	if (ret)
		goto out;

	printf("status=%s(%u)\n", status_name(le32toh(hdr.status)),
	       le32toh(hdr.status));
	if (le32toh(hdr.status) != RAW_STATUS_OK) {
		ret = -EIO;
		goto out;
	}
	if (le32toh(hdr.body_len) < sizeof(rsp)) {
		ret = -EPROTO;
		goto out;
	}

	memcpy(&rsp, body, sizeof(rsp));
	osd_count = le32toh(rsp.osd_count);
	pg_count = le32toh(rsp.pg_count);
	printf("osdmap_epoch=%llu\n",
	       (unsigned long long)le64toh(rsp.osdmap_epoch));
	printf("pgmap_epoch=%llu\n",
	       (unsigned long long)le64toh(rsp.pgmap_epoch));
	printf("osd_count=%u\n", osd_count);
	printf("pg_count=%u\n", pg_count);

	cursor = body + sizeof(rsp);
	end = body + le32toh(hdr.body_len);
	for (i = 0; i < osd_count; ++i) {
		struct raw_osd_entry_hdr osd_hdr;
		uint16_t address_len;
		uint16_t shard_count;
		char *address;
		uint32_t j;

		if ((size_t)(end - cursor) < sizeof(osd_hdr)) {
			ret = -EPROTO;
			goto out;
		}
		memcpy(&osd_hdr, cursor, sizeof(osd_hdr));
		cursor += sizeof(osd_hdr);
		address_len = le16toh(osd_hdr.address_len);
		shard_count = le16toh(osd_hdr.shard_count);
		if ((size_t)(end - cursor) < address_len +
		    shard_count * sizeof(struct raw_osd_shard_entry)) {
			ret = -EPROTO;
			goto out;
		}
		address = calloc(1, (size_t)address_len + 1);
		if (!address) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(address, cursor, address_len);
		cursor += address_len;
		printf("osd[%u]: id=%u flags=0x%x address=%s shard_count=%u\n",
		       i, le32toh(osd_hdr.osd_id), le32toh(osd_hdr.flags), address,
		       shard_count);
		free(address);
		for (j = 0; j < shard_count; ++j) {
			struct raw_osd_shard_entry shard;

			memcpy(&shard, cursor, sizeof(shard));
			cursor += sizeof(shard);
			printf("  shard=%u port=%u core=%u\n",
			       le32toh(shard.shard_id), le16toh(shard.port),
			       le16toh(shard.core_id));
		}
	}

	for (i = 0; i < pg_count; ++i) {
		struct raw_pg_entry_hdr pg_hdr;
		uint32_t replica_count;
		uint32_t j;

		if ((size_t)(end - cursor) < sizeof(pg_hdr)) {
			ret = -EPROTO;
			goto out;
		}
		memcpy(&pg_hdr, cursor, sizeof(pg_hdr));
		cursor += sizeof(pg_hdr);
		replica_count = le32toh(pg_hdr.replica_count);
		if ((size_t)(end - cursor) < replica_count * sizeof(uint32_t)) {
			ret = -EPROTO;
			goto out;
		}
		printf("pg[%u]: pool=%u pg=%u version=%llu state=%u primary_shard=%u replicas=",
		       i, le32toh(pg_hdr.pool_id), le32toh(pg_hdr.pg_id),
		       (unsigned long long)le64toh(pg_hdr.version),
		       le32toh(pg_hdr.state), le32toh(pg_hdr.primary_shard));
		for (j = 0; j < replica_count; ++j) {
			uint32_t osd_id;

			memcpy(&osd_id, cursor, sizeof(osd_id));
			cursor += sizeof(osd_id);
			printf("%s%u", j ? "," : "", le32toh(osd_id));
		}
		printf("\n");
	}

	if (cursor != end) {
		fprintf(stderr, "warning: trailing %zu bytes in cluster-map response\n",
			(size_t)(end - cursor));
	}

out:
	free(body);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int cmd_get_leader(const struct config *cfg)
{
	struct raw_get_leader_req req;
	struct raw_get_leader_rsp rsp;
	struct raw_header hdr;
	unsigned char *body = NULL;
	char *address = NULL;
	uint16_t address_len;
	int fd = -1;
	int ret;

	ret = ensure_addr(cfg);
	if (ret)
		return ret;
	ret = ensure_pool_pg(cfg);
	if (ret)
		return ret;

	req.pool_id = htole32(cfg->pool_id);
	req.pg_id = htole32(cfg->pg_id);

	fd = connect_addr(cfg->addr);
	if (fd < 0)
		return fd;

	ret = send_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_GET_LEADER, 1,
			   &req, sizeof(req));
	if (ret)
		goto out;
	ret = recv_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_GET_LEADER, 1,
			   &hdr, &body);
	if (ret)
		goto out;

	printf("status=%s(%u)\n", status_name(le32toh(hdr.status)),
	       le32toh(hdr.status));
	if (le32toh(hdr.status) != RAW_STATUS_OK) {
		ret = -EIO;
		goto out;
	}
	if (le32toh(hdr.body_len) < sizeof(rsp)) {
		ret = -EPROTO;
		goto out;
	}
	memcpy(&rsp, body, sizeof(rsp));
	address_len = le16toh(rsp.address_len);
	if (le32toh(hdr.body_len) != sizeof(rsp) + address_len) {
		ret = -EPROTO;
		goto out;
	}
	address = calloc(1, (size_t)address_len + 1);
	if (!address) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(address, body + sizeof(rsp), address_len);
	printf("leader_id=%u\n", le32toh(rsp.leader_id));
	printf("leader_port=%u\n", le16toh(rsp.leader_port));
	printf("leader_addr=%s\n", address);

out:
	free(address);
	free(body);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int cmd_pipeline_get_leader(const struct config *cfg)
{
	struct raw_get_leader_req req;
	struct raw_get_leader_rsp rsp;
	struct raw_header hdr;
	unsigned char *body = NULL;
	bool *seen = NULL;
	char *address = NULL;
	uint32_t count;
	uint32_t received = 0;
	uint32_t ok_count = 0;
	uint32_t err_count = 0;
	uint16_t address_len;
	int fd = -1;
	int ret;

	ret = ensure_addr(cfg);
	if (ret)
		return ret;
	ret = ensure_pool_pg(cfg);
	if (ret)
		return ret;

	count = cfg->count_set ? cfg->count : 8;
	if (count == 0) {
		fprintf(stderr, "--count must be greater than 0\n");
		return -EINVAL;
	}

	seen = calloc(count + 1, sizeof(*seen));
	if (!seen)
		return -ENOMEM;

	req.pool_id = htole32(cfg->pool_id);
	req.pg_id = htole32(cfg->pg_id);

	fd = connect_addr(cfg->addr);
	if (fd < 0) {
		ret = fd;
		goto out;
	}

	for (received = 0; received < count; ++received) {
		ret = send_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_GET_LEADER,
				   (uint64_t)received + 1, &req, sizeof(req));
		if (ret)
			goto out;
	}

	printf("pipeline_count=%u\n", count);
	for (received = 0; received < count; ++received) {
		uint64_t seq;

		ret = recv_message_any(fd, RAW_SERVICE_OSD, RAW_OSD_OP_GET_LEADER,
				       &hdr, &body);
		if (ret)
			goto out;

		seq = le64toh(hdr.seq);
		if (seq == 0 || seq > count || seen[seq]) {
			ret = -EPROTO;
			goto out;
		}
		seen[seq] = true;

		printf("seq=%llu status=%s(%u)\n",
		       (unsigned long long)seq,
		       status_name(le32toh(hdr.status)),
		       le32toh(hdr.status));
		if (le32toh(hdr.status) != RAW_STATUS_OK) {
			err_count++;
			free(body);
			body = NULL;
			continue;
		}
		ok_count++;
		if (le32toh(hdr.body_len) < sizeof(rsp)) {
			ret = -EPROTO;
			goto out;
		}

		memcpy(&rsp, body, sizeof(rsp));
		address_len = le16toh(rsp.address_len);
		if (le32toh(hdr.body_len) != sizeof(rsp) + address_len) {
			ret = -EPROTO;
			goto out;
		}
		address = calloc(1, (size_t)address_len + 1);
		if (!address) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(address, body + sizeof(rsp), address_len);
		printf("  leader_id=%u leader_port=%u leader_addr=%s\n",
		       le32toh(rsp.leader_id), le16toh(rsp.leader_port), address);
		free(address);
		address = NULL;
		free(body);
		body = NULL;
	}
	printf("pipeline_ok=%u pipeline_err=%u\n", ok_count, err_count);
	if (!ret && err_count)
		ret = -EIO;

out:
	free(address);
	free(body);
	free(seen);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int cmd_pipeline_read_object(const struct config *cfg)
{
	struct raw_read_object_req req;
	struct raw_read_object_rsp rsp;
	struct raw_header hdr;
	unsigned char *body = NULL;
	unsigned char *req_body = NULL;
	bool *seen = NULL;
	size_t object_len;
	size_t req_len;
	uint32_t count;
	uint32_t received = 0;
	uint32_t ok_count = 0;
	uint32_t err_count = 0;
	uint32_t data_len;
	int fd = -1;
	int ret;

	ret = ensure_addr(cfg);
	if (ret)
		return ret;
	ret = ensure_pool_pg(cfg);
	if (ret)
		return ret;
	if (!cfg->object_name || !cfg->length_set) {
		fprintf(stderr, "--object and --length are required\n");
		return -EINVAL;
	}

	count = cfg->count_set ? cfg->count : 8;
	if (count == 0) {
		fprintf(stderr, "--count must be greater than 0\n");
		return -EINVAL;
	}

	seen = calloc(count + 1, sizeof(*seen));
	if (!seen)
		return -ENOMEM;

	object_len = strlen(cfg->object_name);
	req_len = sizeof(req) + object_len;
	req_body = calloc(1, req_len);
	if (!req_body) {
		ret = -ENOMEM;
		goto out;
	}

	req.pool_id = htole32(cfg->pool_id);
	req.pg_id = htole32(cfg->pg_id);
	req.offset = htole64(cfg->offset_set ? cfg->offset : 0);
	req.length = htole32(cfg->length);
	req.object_name_len = htole16((uint16_t)object_len);
	memcpy(req_body, &req, sizeof(req));
	memcpy(req_body + sizeof(req), cfg->object_name, object_len);

	fd = connect_addr(cfg->addr);
	if (fd < 0) {
		ret = fd;
		goto out;
	}

	for (received = 0; received < count; ++received) {
		ret = send_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_READ_OBJECT,
				   (uint64_t)received + 1, req_body,
				   (uint32_t)req_len);
		if (ret)
			goto out;
	}

	printf("pipeline_count=%u\n", count);
	for (received = 0; received < count; ++received) {
		uint64_t seq;

		ret = recv_message_any(fd, RAW_SERVICE_OSD, RAW_OSD_OP_READ_OBJECT,
				       &hdr, &body);
		if (ret)
			goto out;

		seq = le64toh(hdr.seq);
		if (seq == 0 || seq > count || seen[seq]) {
			ret = -EPROTO;
			goto out;
		}
		seen[seq] = true;
		printf("seq=%llu status=%s(%u)\n",
		       (unsigned long long)seq,
		       status_name(le32toh(hdr.status)),
		       le32toh(hdr.status));
		if (le32toh(hdr.status) != RAW_STATUS_OK) {
			err_count++;
			free(body);
			body = NULL;
			continue;
		}
		ok_count++;
		if (le32toh(hdr.body_len) < sizeof(rsp)) {
			ret = -EPROTO;
			goto out;
		}

		memcpy(&rsp, body, sizeof(rsp));
		data_len = le32toh(rsp.data_len);
		if (le32toh(hdr.body_len) != sizeof(rsp) + data_len) {
			ret = -EPROTO;
			goto out;
		}
		printf("  data_len=%u\n", data_len);
		if (cfg->output_file) {
			ret = write_file(cfg->output_file, body + sizeof(rsp), data_len);
			if (ret)
				goto out;
		}
		free(body);
		body = NULL;
	}
	printf("pipeline_ok=%u pipeline_err=%u\n", ok_count, err_count);
	if (!ret && err_count)
		ret = -EIO;

out:
	free(body);
	free(req_body);
	free(seen);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int cmd_read_object(const struct config *cfg)
{
	struct raw_read_object_req req;
	struct raw_read_object_rsp rsp;
	struct raw_header hdr;
	unsigned char *body = NULL;
	unsigned char *req_body = NULL;
	size_t object_len;
	size_t req_len;
	uint32_t data_len;
	int fd = -1;
	int ret;

	ret = ensure_addr(cfg);
	if (ret)
		return ret;
	ret = ensure_pool_pg(cfg);
	if (ret)
		return ret;
	if (!cfg->object_name || !cfg->length_set) {
		fprintf(stderr, "--object and --length are required\n");
		return -EINVAL;
	}

	object_len = strlen(cfg->object_name);
	req_len = sizeof(req) + object_len;
	req_body = calloc(1, req_len);
	if (!req_body)
		return -ENOMEM;

	req.pool_id = htole32(cfg->pool_id);
	req.pg_id = htole32(cfg->pg_id);
	req.offset = htole64(cfg->offset_set ? cfg->offset : 0);
	req.length = htole32(cfg->length);
	req.object_name_len = htole16((uint16_t)object_len);
	memcpy(req_body, &req, sizeof(req));
	memcpy(req_body + sizeof(req), cfg->object_name, object_len);

	fd = connect_addr(cfg->addr);
	if (fd < 0) {
		ret = fd;
		goto out;
	}
	ret = send_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_READ_OBJECT, 1,
			   req_body, (uint32_t)req_len);
	if (ret)
		goto out;
	ret = recv_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_READ_OBJECT, 1,
			   &hdr, &body);
	if (ret)
		goto out;

	printf("status=%s(%u)\n", status_name(le32toh(hdr.status)),
	       le32toh(hdr.status));
	if (le32toh(hdr.status) != RAW_STATUS_OK) {
		ret = -EIO;
		goto out;
	}
	if (le32toh(hdr.body_len) < sizeof(rsp)) {
		ret = -EPROTO;
		goto out;
	}
	memcpy(&rsp, body, sizeof(rsp));
	data_len = le32toh(rsp.data_len);
	if (le32toh(hdr.body_len) != sizeof(rsp) + data_len) {
		ret = -EPROTO;
		goto out;
	}
	printf("data_len=%u\n", data_len);
	if (cfg->output_file)
		ret = write_file(cfg->output_file, body + sizeof(rsp), data_len);
	else
		hexdump(body + sizeof(rsp), data_len);

out:
	free(req_body);
	free(body);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int cmd_write_object(const struct config *cfg)
{
	struct raw_write_object_req req;
	struct raw_header hdr;
	unsigned char *payload = NULL;
	unsigned char *rsp_body = NULL;
	unsigned char *data = NULL;
	size_t object_len;
	size_t data_len = 0;
	size_t req_len;
	int fd = -1;
	int ret;

	ret = ensure_addr(cfg);
	if (ret)
		return ret;
	ret = ensure_pool_pg(cfg);
	if (ret)
		return ret;
	if (!cfg->object_name || (!cfg->data && !cfg->data_file)) {
		fprintf(stderr, "--object and one of --data/--data-file are required\n");
		return -EINVAL;
	}

	if (cfg->data_file) {
		ret = load_file(cfg->data_file, &data, &data_len);
		if (ret)
			return ret;
	} else {
		data_len = strlen(cfg->data);
		data = malloc(data_len);
		if (!data) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(data, cfg->data, data_len);
	}

	object_len = strlen(cfg->object_name);
	req_len = sizeof(req) + object_len + data_len;
	payload = calloc(1, req_len);
	if (!payload) {
		ret = -ENOMEM;
		goto out;
	}

	req.pool_id = htole32(cfg->pool_id);
	req.pg_id = htole32(cfg->pg_id);
	req.offset = htole64(cfg->offset_set ? cfg->offset : 0);
	req.data_len = htole32((uint32_t)data_len);
	req.object_name_len = htole16((uint16_t)object_len);
	memcpy(payload, &req, sizeof(req));
	memcpy(payload + sizeof(req), cfg->object_name, object_len);
	memcpy(payload + sizeof(req) + object_len, data, data_len);

	fd = connect_addr(cfg->addr);
	if (fd < 0) {
		ret = fd;
		goto out;
	}
	ret = send_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_WRITE_OBJECT, 1,
			   payload, (uint32_t)req_len);
	if (ret)
		goto out;
	ret = recv_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_WRITE_OBJECT, 1,
			   &hdr, &rsp_body);
	if (ret)
		goto out;
	printf("status=%s(%u)\n", status_name(le32toh(hdr.status)),
	       le32toh(hdr.status));
	if (le32toh(hdr.status) != RAW_STATUS_OK)
		ret = -EIO;

out:
	free(data);
	free(payload);
	free(rsp_body);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int cmd_pipeline_write_object(const struct config *cfg)
{
	struct raw_write_object_req req;
	struct raw_header hdr;
	unsigned char *payload = NULL;
	unsigned char *rsp_body = NULL;
	unsigned char *data = NULL;
	bool *seen = NULL;
	size_t object_len;
	size_t data_len = 0;
	size_t req_len;
	uint32_t count;
	uint32_t received = 0;
	uint32_t ok_count = 0;
	uint32_t err_count = 0;
	int fd = -1;
	int ret;

	ret = ensure_addr(cfg);
	if (ret)
		return ret;
	ret = ensure_pool_pg(cfg);
	if (ret)
		return ret;
	if (!cfg->object_name || (!cfg->data && !cfg->data_file)) {
		fprintf(stderr, "--object and one of --data/--data-file are required\n");
		return -EINVAL;
	}

	count = cfg->count_set ? cfg->count : 8;
	if (count == 0) {
		fprintf(stderr, "--count must be greater than 0\n");
		return -EINVAL;
	}

	seen = calloc(count + 1, sizeof(*seen));
	if (!seen)
		return -ENOMEM;

	if (cfg->data_file) {
		ret = load_file(cfg->data_file, &data, &data_len);
		if (ret)
			goto out;
	} else {
		data_len = strlen(cfg->data);
		data = malloc(data_len);
		if (!data) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(data, cfg->data, data_len);
	}

	object_len = strlen(cfg->object_name);
	req_len = sizeof(req) + object_len + data_len;
	payload = calloc(1, req_len);
	if (!payload) {
		ret = -ENOMEM;
		goto out;
	}

	req.pool_id = htole32(cfg->pool_id);
	req.pg_id = htole32(cfg->pg_id);
	req.offset = htole64(cfg->offset_set ? cfg->offset : 0);
	req.data_len = htole32((uint32_t)data_len);
	req.object_name_len = htole16((uint16_t)object_len);
	memcpy(payload, &req, sizeof(req));
	memcpy(payload + sizeof(req), cfg->object_name, object_len);
	memcpy(payload + sizeof(req) + object_len, data, data_len);

	fd = connect_addr(cfg->addr);
	if (fd < 0) {
		ret = fd;
		goto out;
	}

	for (received = 0; received < count; ++received) {
		ret = send_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_WRITE_OBJECT,
				   (uint64_t)received + 1, payload,
				   (uint32_t)req_len);
		if (ret)
			goto out;
	}

	printf("pipeline_count=%u\n", count);
	for (received = 0; received < count; ++received) {
		uint64_t seq;

		ret = recv_message_any(fd, RAW_SERVICE_OSD, RAW_OSD_OP_WRITE_OBJECT,
				       &hdr, &rsp_body);
		if (ret)
			goto out;

		seq = le64toh(hdr.seq);
		if (seq == 0 || seq > count || seen[seq]) {
			ret = -EPROTO;
			goto out;
		}
		seen[seq] = true;
		printf("seq=%llu status=%s(%u)\n",
		       (unsigned long long)seq,
		       status_name(le32toh(hdr.status)),
		       le32toh(hdr.status));
		if (le32toh(hdr.status) == RAW_STATUS_OK)
			ok_count++;
		else
			err_count++;
		free(rsp_body);
		rsp_body = NULL;
	}
	printf("pipeline_ok=%u pipeline_err=%u\n", ok_count, err_count);
	if (!ret && err_count)
		ret = -EIO;

out:
	free(rsp_body);
	free(payload);
	free(data);
	free(seen);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int cmd_pipeline_delete_object(const struct config *cfg)
{
	struct raw_delete_object_req req;
	struct raw_header hdr;
	unsigned char *req_body = NULL;
	unsigned char *rsp_body = NULL;
	bool *seen = NULL;
	size_t object_len;
	size_t req_len;
	uint32_t count;
	uint32_t received = 0;
	uint32_t ok_count = 0;
	uint32_t err_count = 0;
	int fd = -1;
	int ret;

	ret = ensure_addr(cfg);
	if (ret)
		return ret;
	ret = ensure_pool_pg(cfg);
	if (ret)
		return ret;
	if (!cfg->object_name) {
		fprintf(stderr, "--object is required\n");
		return -EINVAL;
	}

	count = cfg->count_set ? cfg->count : 8;
	if (count == 0) {
		fprintf(stderr, "--count must be greater than 0\n");
		return -EINVAL;
	}

	seen = calloc(count + 1, sizeof(*seen));
	if (!seen)
		return -ENOMEM;

	object_len = strlen(cfg->object_name);
	req_len = sizeof(req) + object_len;
	req_body = calloc(1, req_len);
	if (!req_body) {
		ret = -ENOMEM;
		goto out;
	}

	req.pool_id = htole32(cfg->pool_id);
	req.pg_id = htole32(cfg->pg_id);
	req.object_name_len = htole16((uint16_t)object_len);
	memcpy(req_body, &req, sizeof(req));
	memcpy(req_body + sizeof(req), cfg->object_name, object_len);

	fd = connect_addr(cfg->addr);
	if (fd < 0) {
		ret = fd;
		goto out;
	}

	for (received = 0; received < count; ++received) {
		ret = send_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_DELETE_OBJECT,
				   (uint64_t)received + 1, req_body,
				   (uint32_t)req_len);
		if (ret)
			goto out;
	}

	printf("pipeline_count=%u\n", count);
	for (received = 0; received < count; ++received) {
		uint64_t seq;

		ret = recv_message_any(fd, RAW_SERVICE_OSD,
				       RAW_OSD_OP_DELETE_OBJECT, &hdr, &rsp_body);
		if (ret)
			goto out;

		seq = le64toh(hdr.seq);
		if (seq == 0 || seq > count || seen[seq]) {
			ret = -EPROTO;
			goto out;
		}
		seen[seq] = true;
		printf("seq=%llu status=%s(%u)\n",
		       (unsigned long long)seq,
		       status_name(le32toh(hdr.status)),
		       le32toh(hdr.status));
		if (le32toh(hdr.status) == RAW_STATUS_OK)
			ok_count++;
		else
			err_count++;
		free(rsp_body);
		rsp_body = NULL;
	}
	printf("pipeline_ok=%u pipeline_err=%u\n", ok_count, err_count);
	if (!ret && err_count)
		ret = -EIO;

out:
	free(req_body);
	free(rsp_body);
	free(seen);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int cmd_delete_object(const struct config *cfg)
{
	struct raw_delete_object_req req;
	struct raw_header hdr;
	unsigned char *body = NULL;
	unsigned char *req_body = NULL;
	size_t object_len;
	size_t req_len;
	int fd = -1;
	int ret;

	ret = ensure_addr(cfg);
	if (ret)
		return ret;
	ret = ensure_pool_pg(cfg);
	if (ret)
		return ret;
	if (!cfg->object_name) {
		fprintf(stderr, "--object is required\n");
		return -EINVAL;
	}

	object_len = strlen(cfg->object_name);
	req_len = sizeof(req) + object_len;
	req_body = calloc(1, req_len);
	if (!req_body)
		return -ENOMEM;

	req.pool_id = htole32(cfg->pool_id);
	req.pg_id = htole32(cfg->pg_id);
	req.object_name_len = htole16((uint16_t)object_len);
	memcpy(req_body, &req, sizeof(req));
	memcpy(req_body + sizeof(req), cfg->object_name, object_len);

	fd = connect_addr(cfg->addr);
	if (fd < 0) {
		ret = fd;
		goto out;
	}
	ret = send_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_DELETE_OBJECT, 1,
			   req_body, (uint32_t)req_len);
	if (ret)
		goto out;
	ret = recv_message(fd, RAW_SERVICE_OSD, RAW_OSD_OP_DELETE_OBJECT, 1,
			   &hdr, &body);
	if (ret)
		goto out;
	printf("status=%s(%u)\n", status_name(le32toh(hdr.status)),
	       le32toh(hdr.status));
	if (le32toh(hdr.status) != RAW_STATUS_OK)
		ret = -EIO;

out:
	free(req_body);
	free(body);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int parse_args(int argc, char **argv, struct config *cfg)
{
	enum {
		OPT_ADDR = 1000,
		OPT_POOL_NAME,
		OPT_IMAGE_NAME,
		OPT_POOL_ID,
		OPT_PG_ID,
		OPT_IMAGE_EPOCH,
		OPT_OSDMAP_EPOCH,
		OPT_PGMAP_EPOCH,
		OPT_OBJECT,
		OPT_OFFSET,
		OPT_LENGTH,
		OPT_COUNT,
		OPT_DATA,
		OPT_DATA_FILE,
		OPT_OUTPUT,
	};
	static const struct option long_opts[] = {
		{"addr", required_argument, NULL, OPT_ADDR},
		{"pool-name", required_argument, NULL, OPT_POOL_NAME},
		{"image-name", required_argument, NULL, OPT_IMAGE_NAME},
		{"pool-id", required_argument, NULL, OPT_POOL_ID},
		{"pg-id", required_argument, NULL, OPT_PG_ID},
		{"image-epoch", required_argument, NULL, OPT_IMAGE_EPOCH},
		{"osdmap-epoch", required_argument, NULL, OPT_OSDMAP_EPOCH},
		{"pgmap-epoch", required_argument, NULL, OPT_PGMAP_EPOCH},
		{"object", required_argument, NULL, OPT_OBJECT},
		{"offset", required_argument, NULL, OPT_OFFSET},
		{"length", required_argument, NULL, OPT_LENGTH},
		{"count", required_argument, NULL, OPT_COUNT},
		{"data", required_argument, NULL, OPT_DATA},
		{"data-file", required_argument, NULL, OPT_DATA_FILE},
		{"output", required_argument, NULL, OPT_OUTPUT},
		{0, 0, 0, 0},
	};
	int opt;
	int ret;

	if (argc < 2)
		return -EINVAL;

	cfg->command = argv[1];
	optind = 2;
	while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
		switch (opt) {
		case OPT_ADDR:
			cfg->addr = strdup(optarg);
			break;
		case OPT_POOL_NAME:
			cfg->pool_name = strdup(optarg);
			break;
		case OPT_IMAGE_NAME:
			cfg->image_name = strdup(optarg);
			break;
		case OPT_OBJECT:
			cfg->object_name = strdup(optarg);
			break;
		case OPT_DATA:
			cfg->data = strdup(optarg);
			break;
		case OPT_DATA_FILE:
			cfg->data_file = strdup(optarg);
			break;
		case OPT_OUTPUT:
			cfg->output_file = strdup(optarg);
			break;
		case OPT_POOL_ID:
			ret = parse_u32(optarg, &cfg->pool_id);
			if (ret)
				return ret;
			cfg->pool_id_set = true;
			break;
		case OPT_PG_ID:
			ret = parse_u32(optarg, &cfg->pg_id);
			if (ret)
				return ret;
			cfg->pg_id_set = true;
			break;
		case OPT_IMAGE_EPOCH:
			ret = parse_u64(optarg, &cfg->image_epoch);
			if (ret)
				return ret;
			cfg->image_epoch_set = true;
			break;
		case OPT_OSDMAP_EPOCH:
			ret = parse_u64(optarg, &cfg->osdmap_epoch);
			if (ret)
				return ret;
			cfg->osdmap_epoch_set = true;
			break;
		case OPT_PGMAP_EPOCH:
			ret = parse_u64(optarg, &cfg->pgmap_epoch);
			if (ret)
				return ret;
			cfg->pgmap_epoch_set = true;
			break;
		case OPT_OFFSET:
			ret = parse_u64(optarg, &cfg->offset);
			if (ret)
				return ret;
			cfg->offset_set = true;
			break;
		case OPT_LENGTH:
			ret = parse_u32(optarg, &cfg->length);
			if (ret)
				return ret;
			cfg->length_set = true;
			break;
		case OPT_COUNT:
			ret = parse_u32(optarg, &cfg->count);
			if (ret)
				return ret;
			cfg->count_set = true;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static void free_config(struct config *cfg)
{
	free(cfg->addr);
	free(cfg->pool_name);
	free(cfg->image_name);
	free(cfg->object_name);
	free(cfg->data);
	free(cfg->data_file);
	free(cfg->output_file);
}

int main(int argc, char **argv)
{
	struct config cfg = {0};
	int ret;

	ret = parse_args(argc, argv, &cfg);
	if (ret) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (strcmp(cfg.command, "get-image-info") == 0)
		ret = cmd_get_image_info(&cfg);
	else if (strcmp(cfg.command, "get-cluster-map") == 0)
		ret = cmd_get_cluster_map(&cfg);
	else if (strcmp(cfg.command, "get-leader") == 0)
		ret = cmd_get_leader(&cfg);
	else if (strcmp(cfg.command, "pipeline-get-leader") == 0)
		ret = cmd_pipeline_get_leader(&cfg);
	else if (strcmp(cfg.command, "pipeline-read-object") == 0)
		ret = cmd_pipeline_read_object(&cfg);
	else if (strcmp(cfg.command, "pipeline-write-object") == 0)
		ret = cmd_pipeline_write_object(&cfg);
	else if (strcmp(cfg.command, "pipeline-delete-object") == 0)
		ret = cmd_pipeline_delete_object(&cfg);
	else if (strcmp(cfg.command, "read-object") == 0)
		ret = cmd_read_object(&cfg);
	else if (strcmp(cfg.command, "write-object") == 0)
		ret = cmd_write_object(&cfg);
	else if (strcmp(cfg.command, "delete-object") == 0)
		ret = cmd_delete_object(&cfg);
	else {
		fprintf(stderr, "unknown command: %s\n", cfg.command);
		usage(argv[0]);
		ret = -EINVAL;
	}

	if (ret)
		fprintf(stderr, "error: %s (%d)\n", strerror(-ret), ret);
	free_config(&cfg);
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
