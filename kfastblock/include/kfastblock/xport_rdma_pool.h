#ifndef KFASTBLOCK_XPORT_RDMA_POOL_H
#define KFASTBLOCK_XPORT_RDMA_POOL_H

#include <linux/mutex.h>
#include <linux/types.h>

#include "kfastblock/common.h"
#include "kfastblock/meta.h"

struct kfastblock_rdma_conn;

/* Default number of reusable RDMA connection slots per pool. */
#define KFASTBLOCK_RDMA_POOL_DEFAULT_SLOTS 8U
/* Default max IDLE connections retained; 0 means no limit beyond nr_slots. */
#define KFASTBLOCK_RDMA_POOL_DEFAULT_MAX_IDLE 4U

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
	u32 max_idle;
	u64 get_hits;
	u64 get_misses;
	u64 connect_ok;
	u64 connect_err;
	u64 reuse_hits;
	u64 idle_evictions;
};

struct kfastblock_rdma_pool {
	struct kfastblock_rdma_pool_slot *slots;
	u32 nr_slots;
	/* Max IDLE slots retained; excess LRU-evicted on put. 0 = unlimited. */
	u32 max_idle;
	u64 get_hits;
	u64 get_misses;
	u64 connect_ok;
	u64 connect_err;
	u64 idle_evictions;
};

/* Allocate slot array and initialize empty pool. nr_slots 0 => default. */
int kfastblock_rdma_pool_init(struct kfastblock_rdma_pool *pool, u32 nr_slots);

/* Disconnect all slots and free slot array. */
void kfastblock_rdma_pool_destroy(struct kfastblock_rdma_pool *pool);

/* Close connections but keep slot array (for detach / reset). */
void kfastblock_rdma_pool_close(struct kfastblock_rdma_pool *pool);

/*
 * Get a connected RDMA conn for leader. Reuses idle matching slot when
 * possible; otherwise connects a free/empty slot via public xport_rdma API.
 * Caller must put() when done. Returns NULL on failure.
 */
struct kfastblock_rdma_conn *
kfastblock_rdma_pool_get(struct kfastblock_rdma_pool *pool,
			 const struct kfastblock_leader_info *leader);

/* Return conn to pool; ok=false marks slot dead and disconnects. */
void kfastblock_rdma_pool_put(struct kfastblock_rdma_pool *pool,
			      struct kfastblock_rdma_conn *conn, bool ok);

void kfastblock_rdma_pool_snapshot(struct kfastblock_rdma_pool *pool,
				   struct kfastblock_rdma_pool_snapshot *snap);

const char *kfastblock_rdma_pool_slot_state_name(u8 state);

/* Count slots currently in @state (EMPTY/IDLE/BUSY/DEAD). */
u32 kfastblock_rdma_pool_count_state(struct kfastblock_rdma_pool *pool,
				     u8 state);

/*
 * Try reuse only: return idle matching connected conn without CM connect.
 * Returns NULL if no warm slot; caller may fall back to get().
 */
struct kfastblock_rdma_conn *
kfastblock_rdma_pool_try_get(struct kfastblock_rdma_pool *pool,
			     const struct kfastblock_leader_info *leader);

/* Reclaim DEAD slots to EMPTY (drop any leftover identity). Returns count. */
u32 kfastblock_rdma_pool_reclaim_dead(struct kfastblock_rdma_pool *pool);

/* True when any slot is BUSY (in-flight I/O). */
bool kfastblock_rdma_pool_has_busy(struct kfastblock_rdma_pool *pool);

/* Format compact pool stats into @buf for logs. */
int kfastblock_rdma_pool_format_stats(struct kfastblock_rdma_pool *pool,
				      char *buf, size_t buf_len);

/* Find slot owning @conn (does not take slot lock). Returns NULL if unknown. */
struct kfastblock_rdma_pool_slot *
kfastblock_rdma_pool_find_slot(struct kfastblock_rdma_pool *pool,
			       struct kfastblock_rdma_conn *conn);

/* Disconnect and free one idle matching slot (if any). Returns true if closed. */
bool kfastblock_rdma_pool_invalidate_leader(
	struct kfastblock_rdma_pool *pool,
	const struct kfastblock_leader_info *leader);

/*
 * Set max IDLE connections retained (0 = unlimited). Excess is reclaimed on
 * the next put/get path via LRU eviction. Returns previous value.
 */
u32 kfastblock_rdma_pool_set_max_idle(struct kfastblock_rdma_pool *pool,
				      u32 max_idle);

#endif
