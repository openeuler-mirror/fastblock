#include <linux/errno.h>

#include "kfastblock/fault_rdma.h"

bool kfastblock_fault_rdma_take_force_tcp(
	struct kfastblock_fault_injection_state *state,
	int *err_out)
{
	int err = 0;

	if (!kfastblock_fault_injection_should_fail(
		    state, KFASTBLOCK_FAULT_FORCE_TCP, &err))
		return false;
	if (err_out)
		*err_out = err ? err : KFASTBLOCK_FAULT_RDMA_CONNECT_ERRNO;
	return true;
}

int kfastblock_fault_rdma_take_connect(
	struct kfastblock_fault_injection_state *state)
{
	int err = 0;

	if (!kfastblock_fault_injection_should_fail(
		    state, KFASTBLOCK_FAULT_RDMA_CONNECT, &err))
		return 0;
	return err ? err : KFASTBLOCK_FAULT_RDMA_CONNECT_ERRNO;
}

int kfastblock_fault_rdma_take_exchange(
	struct kfastblock_fault_injection_state *state)
{
	int err = 0;

	if (!kfastblock_fault_injection_should_fail(
		    state, KFASTBLOCK_FAULT_RDMA_EXCHANGE, &err))
		return 0;
	return err ? err : KFASTBLOCK_FAULT_RDMA_EXCHANGE_ERRNO;
}

int kfastblock_fault_rdma_take_send(
	struct kfastblock_fault_injection_state *state)
{
	int err = 0;

	if (!kfastblock_fault_injection_should_fail(
		    state, KFASTBLOCK_FAULT_RDMA_SEND, &err))
		return 0;
	return err ? err : -EIO;
}
