#ifndef KFASTBLOCK_XPORT_RDMA_POOL_H
#define KFASTBLOCK_XPORT_RDMA_POOL_H

#include <linux/mutex.h>
#include <linux/types.h>

#include "kfastblock/common.h"
#include "kfastblock/meta.h"

struct kfastblock_rdma_conn;

/* Default number of reusable RDMA connection slots per pool. */
#define KFASTBLOCK_RDMA_POOL_DEFAULT_SLOTS 8U

enum kfastblock_rdma_pool_slot_state {
	KFASTBLOCK_RDMA_POOL_SLOT_EMPTY = 0,
	KFASTBLOCK_RDMA_POOL_SLOT_IDLE = 1,
	KFASTBLOCK_RDMA_POOL_SLOT_BUSY = 2,
	KFASTBLOCK_RDMA_POOL_SLOT_DEAD = 3,
};

/*
 * One pooled RDMA connection keyed by leader address:rdma_port.
 * The underlying kfastblock_rdma_conn is owned by the slot.
 */
struct kfastblock_rdma_pool_slot {
	struct kfastblock_rdma_conn *conn;
	struct mutex lock;
	u32 osd_id;
	u16 rdma_port;
	char address[KFASTBLOCK_MAX_ADDR_LEN];
	u8 state;
	u32 connect_attempts;
	u32 reuse_hits;
	u32 success_count;
	u32 failure_count;
	s32 last_error;
	unsigned long last_use_jiffies;
	unsigned long last_connect_jiffies;
};

struct kfastblock_rdma_pool_snapshot {
	u32 total_slots;
	u32 empty_slots;
	u32 idle_slots;
	u32 busy_slots;
	u32 dead_slots;
	u32 connected_slots;
	u64 get_hits;
	u64 get_misses;
	u64 connect_ok;
	u64 connect_err;
	u64 reuse_hits;
};

struct kfastblock_rdma_pool {
	struct kfastblock_rdma_pool_slot *slots;
	u32 nr_slots;
	u64 get_hits;
	u64 get_misses;
	u64 connect_ok;
	u64 connect_err;
};

#endif
