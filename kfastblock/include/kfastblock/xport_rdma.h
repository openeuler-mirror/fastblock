#ifndef KFASTBLOCK_XPORT_RDMA_H
#define KFASTBLOCK_XPORT_RDMA_H

#include <linux/types.h>

#include "kfastblock/meta.h"

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

#endif
