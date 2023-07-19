#include "tcp_messager.h"
#include "spdk/log.h"
#include "spdk/sock.h"
#include "stdlib.h"
#include "spdk/thread.h"
#include "spdk/string.h"

#define ADDR_STR_LEN 64

tcp_messager::tcp_messager(/* args */)
{
    SPDK_NOTICELOG("tcp_messager::tcp_messager()\n");
}

static int 
messager_start()
{
	SPDK_NOTICELOG("tcp_messager::start()\n");
	g_sock = spdk_sock_listen_ext("localhost", 12345, "posix", NULL);
    if (g_sock == NULL) {
		SPDK_ERRLOG("Cannot create server socket\n");
		return -1;
	}

    g_group = spdk_sock_group_create(NULL);
	if (g_group == NULL) {
		SPDK_ERRLOG("Cannot create sock group\n");
		spdk_sock_close(&g_sock);
		return -1;
	}

	char* buf = (char *)calloc(1, 1024);
	if (buf == NULL) {
		SPDK_ERRLOG("Cannot allocate memory for sock group\n");
		spdk_sock_close(&g_sock);
		return -1;
	}

//	spdk_sock_group_provide_buf(group, buf, 1024, NULL);

    g_accept_poller = SPDK_POLLER_REGISTER(tcp_sock_accept_poll, NULL, 1000000);
	SPDK_POLLER_REGISTER(tcp_sock_group_poll, NULL, 0);

	return 0;
}

void messager_stop()
{
    SPDK_NOTICELOG("tcp_messager::~tcp_messager()\n");
    spdk_poller_unregister(&g_accept_poller);
	spdk_sock_close(&g_sock);
	spdk_sock_group_close(&g_group);
}

int
tcp_sock_group_poll(void *arg)
{
	int rc;
	struct spdk_sock_group *group = (struct spdk_sock_group *)arg;
	rc = spdk_sock_group_poll(group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p\n", group);
	}

	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
tcp_sock_accept_poll(void *ctx)
{
	struct spdk_sock_group *group = (struct spdk_sock_group *)ctx;
	
	struct spdk_sock *sock;
	int rc;
	int count = 0;
	char saddr[ADDR_STR_LEN], caddr[ADDR_STR_LEN];
	uint16_t cport, sport;

	// if (!g_is_running) {
	// 	hello_sock_quit(ctx, 0);
	// 	return SPDK_POLLER_IDLE;
	// }

	while (1) {
		sock = spdk_sock_accept(g_sock);
		if (sock != NULL) {
			rc = spdk_sock_getaddr(sock, saddr, sizeof(saddr), &sport, caddr, sizeof(caddr), &cport);
			if (rc < 0) {
				SPDK_ERRLOG("Cannot get connection addresses\n");
				spdk_sock_close(&sock);
				return SPDK_POLLER_IDLE;
			}

			SPDK_NOTICELOG("Accepting a new connection from (%s, %hu) to (%s, %hu)\n",
				       caddr, cport, saddr, sport);

			rc = spdk_sock_group_add_sock(g_group, sock,
						      tcp_sock_cb, ctx);

			if (rc < 0) {
				spdk_sock_close(&sock);
				SPDK_ERRLOG("failed\n");
				break;
			}

			count++;
		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				SPDK_ERRLOG("accept error(%d): %s\n", errno, spdk_strerror(errno));
			}
			break;
		}
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
tcp_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	int rc;
	struct iovec iov = {};
	uint8_t buf[1024];

	rc = spdk_sock_recv(sock, buf, sizeof(buf));
	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return;
		}

		if (errno != ENOTCONN && errno != ECONNRESET) {
			SPDK_ERRLOG("spdk_sock_recv_zcopy() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	iov.iov_len = rc;

	if (iov.iov_len > 0) {
		SPDK_DEBUGLOG(SPDK_LOG_TCP, "Received %lu bytes\n", iov.iov_len);
		printf("content is %s\n", buf);
		Msg__Request *msg;
		msg = msg__request__unpack(NULL, iov.iov_len, buf);

		spdk_ring_enqueue(g_request_queue, (void **)&msg, 1, NULL);
		//封装成message，然后放入到osd的g_req_queue中
		// struct message *msg = (struct message *)calloc(1, sizeof(struct message));
		// msg->data = (char *)calloc(1, iov.iov_len);
		// memcpy(msg->data, buf, iov.iov_len);
		// msg->len = iov.iov_len;
		// msg->sock = sock;
		// msg->group = group;
		// spdk_ring_enqueue(g_request_queue, (void *)buf, 1, NULL);

		return;
	}

	/* Connection closed */
	SPDK_NOTICELOG("Connection closed\n");
	spdk_sock_group_remove_sock(group, sock);
	spdk_sock_close(&sock);
}
