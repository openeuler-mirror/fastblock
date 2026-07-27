#ifndef KFASTBLOCK_XPORT_RDMA_H
#define KFASTBLOCK_XPORT_RDMA_H

#include <linux/types.h>

#include "kfastblock/meta.h"

/* raw header (24) + max object body (~4MiB) + margin */
#define KFASTBLOCK_RDMA_BUF_LEN ((4U * 1024U * 1024U) + 4096U)

struct kfastblock_rdma_conn;

/* Allocate an idle RDMA connection object (not connected yet). */
struct kfastblock_rdma_conn *
kfastblock_rdma_conn_alloc(void);

void kfastblock_rdma_conn_free(struct kfastblock_rdma_conn *conn);

/*
 * Resolve and connect to leader->address:leader->rdma_port via RDMA CM.
 * On success the connection is ESTABLISHED; caller must disconnect/free.
 * Fails if peer has no raw RDMA listener or CM setup errors out.
 */
int kfastblock_rdma_conn_connect(struct kfastblock_rdma_conn *conn,
				 const struct kfastblock_leader_info *leader);

void kfastblock_rdma_conn_disconnect(struct kfastblock_rdma_conn *conn);

bool kfastblock_rdma_conn_is_connected(const struct kfastblock_rdma_conn *conn);

/*
 * Send one contiguous buffer (raw header+body) over RDMA SEND.
 * Returns 0 on success. Not fully wired until CQ completion path lands.
 */
int kfastblock_rdma_conn_send(struct kfastblock_rdma_conn *conn,
			      const void *buf, u32 len);

/*
 * Wait for one RDMA RECV completion into @buf (capacity @buf_len).
 * Returns received length on success, negative errno on failure.
 */
int kfastblock_rdma_conn_recv(struct kfastblock_rdma_conn *conn,
			      void *buf, u32 buf_len);

/*
 * Send request frame then wait for a response with matching seq.
 * @req / @req_len is the full raw header+body request.
 * @rsp / @rsp_cap receives the full raw header+body response.
 * Returns response length on success, negative errno on failure.
 */
int kfastblock_rdma_conn_exchange(struct kfastblock_rdma_conn *conn,
				  const void *req, u32 req_len,
				  void *rsp, u32 rsp_cap, u64 expect_seq);

/* Peer address helpers for diagnostics. */
const char *kfastblock_rdma_conn_peer_addr(const struct kfastblock_rdma_conn *conn);
u16 kfastblock_rdma_conn_peer_port(const struct kfastblock_rdma_conn *conn);
int kfastblock_rdma_conn_last_error(const struct kfastblock_rdma_conn *conn);

#endif
