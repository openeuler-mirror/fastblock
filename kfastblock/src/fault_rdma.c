#include <linux/errno.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "kfastblock/fault_rdma.h"

static unsigned long kfastblock_fault_rdma_force_tcp_total;
static unsigned long kfastblock_fault_rdma_connect_total;
static unsigned long kfastblock_fault_rdma_exchange_total;
static unsigned long kfastblock_fault_rdma_send_total;

module_param_named(fault_rdma_force_tcp, kfastblock_fault_rdma_force_tcp_total,
		   ulong, 0444);
MODULE_PARM_DESC(fault_rdma_force_tcp, "Fault injection: force TCP fallback count");
module_param_named(fault_rdma_connect, kfastblock_fault_rdma_connect_total,
		   ulong, 0444);
MODULE_PARM_DESC(fault_rdma_connect, "Fault injection: RDMA connect failure count");
module_param_named(fault_rdma_exchange, kfastblock_fault_rdma_exchange_total,
		   ulong, 0444);
MODULE_PARM_DESC(fault_rdma_exchange, "Fault injection: RDMA exchange failure count");
module_param_named(fault_rdma_send, kfastblock_fault_rdma_send_total,
		   ulong, 0444);
MODULE_PARM_DESC(fault_rdma_send, "Fault injection: RDMA send failure count");

bool kfastblock_fault_rdma_take_force_tcp(
	struct kfastblock_fault_injection_state *state,
	int *err_out)
{
	int err = 0;

	if (!kfastblock_fault_injection_should_fail(
		    state, KFASTBLOCK_FAULT_FORCE_TCP, &err))
		return false;
	kfastblock_fault_rdma_force_tcp_total++;
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
	kfastblock_fault_rdma_connect_total++;
	return err ? err : KFASTBLOCK_FAULT_RDMA_CONNECT_ERRNO;
}

int kfastblock_fault_rdma_take_exchange(
	struct kfastblock_fault_injection_state *state)
{
	int err = 0;

	if (!kfastblock_fault_injection_should_fail(
		    state, KFASTBLOCK_FAULT_RDMA_EXCHANGE, &err))
		return 0;
	kfastblock_fault_rdma_exchange_total++;
	return err ? err : KFASTBLOCK_FAULT_RDMA_EXCHANGE_ERRNO;
}

int kfastblock_fault_rdma_take_send(
	struct kfastblock_fault_injection_state *state)
{
	int err = 0;

	if (!kfastblock_fault_injection_should_fail(
		    state, KFASTBLOCK_FAULT_RDMA_SEND, &err))
		return 0;
	kfastblock_fault_rdma_send_total++;
	return err ? err : -EIO;
}
