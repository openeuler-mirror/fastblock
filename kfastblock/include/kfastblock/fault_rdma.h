#ifndef KFASTBLOCK_FAULT_RDMA_H
#define KFASTBLOCK_FAULT_RDMA_H

#include "kfastblock/fault.h"

/*
 * RDMA-specific fault helpers for transport / recovery test hooks.
 * Kept separate from the core fault engine so xport paths can include
 * a small surface without pulling volume plumbing.
 */

/* Default injected errno: CM connect failure / exchange deadline. */
#define KFASTBLOCK_FAULT_RDMA_CONNECT_ERRNO (-ENOTCONN)
#define KFASTBLOCK_FAULT_RDMA_EXCHANGE_ERRNO (-ETIMEDOUT)

/*
 * If FORCE_TCP is armed, consume one budget hit and return true.
 * Caller should skip RDMA selection and use TCP.
 */
bool kfastblock_fault_rdma_take_force_tcp(
	struct kfastblock_fault_injection_state *state,
	int *err_out);

/*
 * If RDMA_CONNECT is armed, consume one budget hit and return injected err.
 * Returns 0 when not armed.
 */
int kfastblock_fault_rdma_take_connect(
	struct kfastblock_fault_injection_state *state);

/*
 * If RDMA_EXCHANGE is armed, consume one budget hit and return injected err.
 * Returns 0 when not armed.
 */
int kfastblock_fault_rdma_take_exchange(
	struct kfastblock_fault_injection_state *state);

/*
 * If RDMA_SEND is armed, consume one budget hit and return injected err.
 * Returns 0 when not armed.
 */
int kfastblock_fault_rdma_take_send(
	struct kfastblock_fault_injection_state *state);

#endif
