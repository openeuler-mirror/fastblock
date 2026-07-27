#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kfastblock/xport_rdma.h"
#include "kfastblock/xport_rdma_pool.h"

/* Aggregate across all pools; exposed as module params for quick sysfs peek. */
static unsigned long kfastblock_rdma_pool_hit_total;
static unsigned long kfastblock_rdma_pool_miss_total;
static unsigned long kfastblock_rdma_pool_evict_total;

module_param_named(rdma_pool_hit, kfastblock_rdma_pool_hit_total, ulong, 0444);
MODULE_PARM_DESC(rdma_pool_hit, "RDMA pool get warm-hit total");
module_param_named(rdma_pool_miss, kfastblock_rdma_pool_miss_total, ulong, 0444);
MODULE_PARM_DESC(rdma_pool_miss, "RDMA pool get miss/cold-connect total");
module_param_named(rdma_pool_evict, kfastblock_rdma_pool_evict_total, ulong, 0444);
MODULE_PARM_DESC(rdma_pool_evict, "RDMA pool idle LRU eviction total");

const char *kfastblock_rdma_pool_slot_state_name(u8 state)
{
	switch (state) {
	case KFASTBLOCK_RDMA_POOL_SLOT_EMPTY:
		return "empty";
	case KFASTBLOCK_RDMA_POOL_SLOT_IDLE:
		return "idle";
	case KFASTBLOCK_RDMA_POOL_SLOT_BUSY:
		return "busy";
	case KFASTBLOCK_RDMA_POOL_SLOT_DEAD:
		return "dead";
	default:
		return "unknown";
	}
}

static void kfastblock_rdma_pool_slot_init(struct kfastblock_rdma_pool_slot *slot)
{
	if (!slot)
		return;

	memset(slot, 0, sizeof(*slot));
	mutex_init(&slot->lock);
	slot->state = KFASTBLOCK_RDMA_POOL_SLOT_EMPTY;
}

int kfastblock_rdma_pool_init(struct kfastblock_rdma_pool *pool, u32 nr_slots)
{
	u32 i;

	if (!pool)
		return -EINVAL;
	if (!nr_slots)
		nr_slots = KFASTBLOCK_RDMA_POOL_DEFAULT_SLOTS;

	memset(pool, 0, sizeof(*pool));
	pool->slots = kcalloc(nr_slots, sizeof(*pool->slots), GFP_KERNEL);
	if (!pool->slots)
		return -ENOMEM;
	pool->nr_slots = nr_slots;
	pool->max_idle = KFASTBLOCK_RDMA_POOL_DEFAULT_MAX_IDLE;
	if (pool->max_idle > nr_slots)
		pool->max_idle = nr_slots;
	for (i = 0; i < nr_slots; ++i)
		kfastblock_rdma_pool_slot_init(&pool->slots[i]);
	return 0;
}

static void kfastblock_rdma_pool_slot_clear_identity(
	struct kfastblock_rdma_pool_slot *slot)
{
	if (!slot)
		return;
	memset(slot->address, 0, sizeof(slot->address));
	slot->osd_id = 0;
	slot->rdma_port = 0;
}

static void kfastblock_rdma_pool_slot_disconnect_locked(
	struct kfastblock_rdma_pool_slot *slot)
{
	if (!slot)
		return;
	if (slot->conn) {
		kfastblock_rdma_conn_free(slot->conn);
		slot->conn = NULL;
	}
	slot->state = KFASTBLOCK_RDMA_POOL_SLOT_EMPTY;
	kfastblock_rdma_pool_slot_clear_identity(slot);
}

void kfastblock_rdma_pool_close(struct kfastblock_rdma_pool *pool)
{
	u32 i;

	if (!pool || !pool->slots)
		return;
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		kfastblock_rdma_pool_slot_disconnect_locked(slot);
		mutex_unlock(&slot->lock);
	}
}

void kfastblock_rdma_pool_destroy(struct kfastblock_rdma_pool *pool)
{
	if (!pool)
		return;
	kfastblock_rdma_pool_close(pool);
	kfree(pool->slots);
	pool->slots = NULL;
	pool->nr_slots = 0;
}

/*
 * Endpoint identity is address + rdma_port (raw RDMA listen key).
 * osd_id is advisory: mismatch means map churn — treat as non-match so
 * try_get falls through to reconnect rather than reuse a wrong peer.
 */
static bool kfastblock_rdma_pool_slot_endpoint_eq_locked(
	const struct kfastblock_rdma_pool_slot *slot,
	const struct kfastblock_leader_info *leader)
{
	if (!slot || !leader)
		return false;
	if (!leader->address[0] || !leader->rdma_port)
		return false;
	if (slot->rdma_port != leader->rdma_port)
		return false;
	return strncmp(slot->address, leader->address,
		       KFASTBLOCK_MAX_ADDR_LEN) == 0;
}

static bool kfastblock_rdma_pool_slot_matches_locked(
	const struct kfastblock_rdma_pool_slot *slot,
	const struct kfastblock_leader_info *leader)
{
	if (!slot || !leader)
		return false;
	if (!slot->conn || slot->state == KFASTBLOCK_RDMA_POOL_SLOT_EMPTY ||
	    slot->state == KFASTBLOCK_RDMA_POOL_SLOT_DEAD)
		return false;
	if (!kfastblock_rdma_pool_slot_endpoint_eq_locked(slot, leader))
		return false;
	/* Same endpoint but different osd_id: stale binding, do not reuse. */
	if (slot->osd_id && leader->osd_id && slot->osd_id != leader->osd_id)
		return false;
	return true;
}

static void kfastblock_rdma_pool_slot_bind_locked(
	struct kfastblock_rdma_pool_slot *slot,
	const struct kfastblock_leader_info *leader)
{
	if (!slot || !leader)
		return;
	slot->osd_id = leader->osd_id;
	slot->rdma_port = leader->rdma_port;
	strscpy(slot->address, leader->address, sizeof(slot->address));
}

/*
 * Acquire an established RDMA conn for leader.
 * Prefer idle matching slot (reuse); otherwise connect into an empty slot.
 * Returns held conn (slot BUSY); caller must put().
 */
struct kfastblock_rdma_conn *
kfastblock_rdma_pool_get(struct kfastblock_rdma_pool *pool,
			 const struct kfastblock_leader_info *leader)
{
	u32 i;
	struct kfastblock_rdma_conn *conn;
	int ret;

	if (!pool || !pool->slots || !kfastblock_leader_has_rdma(leader))
		return NULL;

	/* Pass 1: warm reuse (no CM). */
	conn = kfastblock_rdma_pool_try_get(pool, leader);
	if (conn)
		return conn;

	/* Pass 2: connect into first empty/dead slot. */
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		if (slot->state != KFASTBLOCK_RDMA_POOL_SLOT_EMPTY &&
		    slot->state != KFASTBLOCK_RDMA_POOL_SLOT_DEAD) {
			mutex_unlock(&slot->lock);
			continue;
		}

		slot->connect_attempts++;
		if (!slot->conn) {
			slot->conn = kfastblock_rdma_conn_alloc();
			if (!slot->conn) {
				slot->last_error = -ENOMEM;
				slot->state = KFASTBLOCK_RDMA_POOL_SLOT_EMPTY;
				pool->connect_err++;
				mutex_unlock(&slot->lock);
				pool->get_misses++;
				return NULL;
			}
		}
		ret = kfastblock_rdma_conn_connect(slot->conn, leader);
		if (ret) {
			slot->last_error = ret;
			slot->failure_count++;
			kfastblock_rdma_conn_free(slot->conn);
			slot->conn = NULL;
			slot->state = KFASTBLOCK_RDMA_POOL_SLOT_EMPTY;
			kfastblock_rdma_pool_slot_clear_identity(slot);
			pool->connect_err++;
			mutex_unlock(&slot->lock);
			pool->get_misses++;
			return NULL;
		}

		kfastblock_rdma_pool_slot_bind_locked(slot, leader);
		slot->state = KFASTBLOCK_RDMA_POOL_SLOT_BUSY;
		slot->last_connect_jiffies = jiffies;
		slot->last_use_jiffies = jiffies;
		slot->last_error = 0;
		pool->connect_ok++;
		pool->get_misses++;
		kfastblock_rdma_pool_miss_total++;
		conn = slot->conn;
		mutex_unlock(&slot->lock);
		return conn;
	}

	pool->get_misses++;
	kfastblock_rdma_pool_miss_total++;
	return NULL;
}

/*
 * If idle count exceeds max_idle, disconnect the least-recently-used IDLE
 * slot (not @skip). Caller must NOT hold any slot lock.
 */
static void kfastblock_rdma_pool_evict_idle_lru(
	struct kfastblock_rdma_pool *pool,
	struct kfastblock_rdma_pool_slot *skip)
{
	u32 i, idle_n = 0;
	struct kfastblock_rdma_pool_slot *victim = NULL;
	unsigned long oldest = 0;

	if (!pool || !pool->slots || !pool->max_idle)
		return;

	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		if (slot->state == KFASTBLOCK_RDMA_POOL_SLOT_IDLE) {
			idle_n++;
			if (slot != skip &&
			    (!victim ||
			     time_before(slot->last_use_jiffies, oldest))) {
				victim = slot;
				oldest = slot->last_use_jiffies;
			}
		}
		mutex_unlock(&slot->lock);
	}

	if (idle_n <= pool->max_idle || !victim)
		return;

	mutex_lock(&victim->lock);
	if (victim->state == KFASTBLOCK_RDMA_POOL_SLOT_IDLE) {
		kfastblock_rdma_pool_slot_disconnect_locked(victim);
		pool->idle_evictions++;
		kfastblock_rdma_pool_evict_total++;
	}
	mutex_unlock(&victim->lock);
}

void kfastblock_rdma_pool_put(struct kfastblock_rdma_pool *pool,
			      struct kfastblock_rdma_conn *conn, bool ok)
{
	struct kfastblock_rdma_pool_slot *slot;
	bool became_idle = false;

	if (!pool || !pool->slots || !conn)
		return;

	slot = kfastblock_rdma_pool_find_slot(pool, conn);
	if (!slot)
		return;

	mutex_lock(&slot->lock);
	if (slot->conn != conn) {
		mutex_unlock(&slot->lock);
		return;
	}
	if (ok && kfastblock_rdma_conn_is_connected(conn)) {
		slot->state = KFASTBLOCK_RDMA_POOL_SLOT_IDLE;
		slot->success_count++;
		slot->last_use_jiffies = jiffies;
		slot->last_error = 0;
		became_idle = true;
	} else {
		kfastblock_rdma_pool_slot_disconnect_locked(slot);
		slot->state = KFASTBLOCK_RDMA_POOL_SLOT_DEAD;
		slot->failure_count++;
		if (!ok)
			slot->last_error = -EIO;
	}
	mutex_unlock(&slot->lock);

	/* Over max_idle: drop LRU idle (keep the just-returned slot). */
	if (became_idle)
		kfastblock_rdma_pool_evict_idle_lru(pool, slot);
}

u32 kfastblock_rdma_pool_count_state(struct kfastblock_rdma_pool *pool,
				     u8 state)
{
	u32 i, n = 0;

	if (!pool || !pool->slots)
		return 0;
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		if (slot->state == state)
			n++;
		mutex_unlock(&slot->lock);
	}
	return n;
}

struct kfastblock_rdma_conn *
kfastblock_rdma_pool_try_get(struct kfastblock_rdma_pool *pool,
			     const struct kfastblock_leader_info *leader)
{
	u32 i;

	if (!pool || !pool->slots || !kfastblock_leader_has_rdma(leader))
		return NULL;

	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];
		struct kfastblock_rdma_conn *conn;

		mutex_lock(&slot->lock);
		if (slot->state != KFASTBLOCK_RDMA_POOL_SLOT_IDLE) {
			mutex_unlock(&slot->lock);
			continue;
		}
		if (!kfastblock_rdma_pool_slot_matches_locked(slot, leader)) {
			mutex_unlock(&slot->lock);
			continue;
		}
		/* Broken idle conn: invalidate so get() can reconnect. */
		if (!slot->conn || !kfastblock_rdma_conn_is_connected(slot->conn) ||
		    kfastblock_rdma_conn_last_error(slot->conn)) {
			kfastblock_rdma_pool_slot_disconnect_locked(slot);
			slot->state = KFASTBLOCK_RDMA_POOL_SLOT_DEAD;
			slot->failure_count++;
			slot->last_error = -ENOTCONN;
			mutex_unlock(&slot->lock);
			continue;
		}
		slot->state = KFASTBLOCK_RDMA_POOL_SLOT_BUSY;
		slot->reuse_hits++;
		slot->last_use_jiffies = jiffies;
		pool->get_hits++;
		kfastblock_rdma_pool_hit_total++;
		conn = slot->conn;
		mutex_unlock(&slot->lock);
		return conn;
	}
	return NULL;
}

u32 kfastblock_rdma_pool_reclaim_dead(struct kfastblock_rdma_pool *pool)
{
	u32 i, n = 0;

	if (!pool || !pool->slots)
		return 0;
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		if (slot->state == KFASTBLOCK_RDMA_POOL_SLOT_DEAD) {
			if (slot->conn) {
				kfastblock_rdma_conn_free(slot->conn);
				slot->conn = NULL;
			}
			kfastblock_rdma_pool_slot_clear_identity(slot);
			slot->state = KFASTBLOCK_RDMA_POOL_SLOT_EMPTY;
			n++;
		}
		mutex_unlock(&slot->lock);
	}
	return n;
}

bool kfastblock_rdma_pool_has_busy(struct kfastblock_rdma_pool *pool)
{
	return kfastblock_rdma_pool_count_state(
		       pool, KFASTBLOCK_RDMA_POOL_SLOT_BUSY) > 0;
}

int kfastblock_rdma_pool_format_stats(struct kfastblock_rdma_pool *pool,
				      char *buf, size_t buf_len)
{
	struct kfastblock_rdma_pool_snapshot snap;

	if (!buf || !buf_len)
		return -EINVAL;
	if (!pool) {
		buf[0] = '\0';
		return -EINVAL;
	}
	kfastblock_rdma_pool_snapshot(pool, &snap);
	return scnprintf(buf, buf_len,
			 "slots=%u empty=%u idle=%u busy=%u dead=%u connected=%u max_idle=%u hits=%llu misses=%llu evict=%llu",
			 snap.total_slots, snap.empty_slots, snap.idle_slots,
			 snap.busy_slots, snap.dead_slots, snap.connected_slots,
			 snap.max_idle,
			 (unsigned long long)snap.get_hits,
			 (unsigned long long)snap.get_misses,
			 (unsigned long long)snap.idle_evictions);
}

struct kfastblock_rdma_pool_slot *
kfastblock_rdma_pool_find_slot(struct kfastblock_rdma_pool *pool,
			       struct kfastblock_rdma_conn *conn)
{
	u32 i;

	if (!pool || !pool->slots || !conn)
		return NULL;
	for (i = 0; i < pool->nr_slots; ++i) {
		if (pool->slots[i].conn == conn)
			return &pool->slots[i];
	}
	return NULL;
}

bool kfastblock_rdma_pool_invalidate_leader(
	struct kfastblock_rdma_pool *pool,
	const struct kfastblock_leader_info *leader)
{
	u32 i;
	bool closed = false;

	if (!pool || !pool->slots || !leader)
		return false;
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		if (slot->state != KFASTBLOCK_RDMA_POOL_SLOT_BUSY &&
		    kfastblock_rdma_pool_slot_matches_locked(slot, leader)) {
			kfastblock_rdma_pool_slot_disconnect_locked(slot);
			closed = true;
		}
		mutex_unlock(&slot->lock);
	}
	return closed;
}

void kfastblock_rdma_pool_snapshot(struct kfastblock_rdma_pool *pool,
				   struct kfastblock_rdma_pool_snapshot *snap)
{
	u32 i;
	u64 reuse = 0;

	if (!snap)
		return;
	memset(snap, 0, sizeof(*snap));
	if (!pool || !pool->slots)
		return;

	snap->total_slots = pool->nr_slots;
	snap->max_idle = pool->max_idle;
	snap->get_hits = pool->get_hits;
	snap->get_misses = pool->get_misses;
	snap->connect_ok = pool->connect_ok;
	snap->connect_err = pool->connect_err;
	snap->idle_evictions = pool->idle_evictions;

	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		switch (slot->state) {
		case KFASTBLOCK_RDMA_POOL_SLOT_EMPTY:
			snap->empty_slots++;
			break;
		case KFASTBLOCK_RDMA_POOL_SLOT_IDLE:
			snap->idle_slots++;
			break;
		case KFASTBLOCK_RDMA_POOL_SLOT_BUSY:
			snap->busy_slots++;
			break;
		case KFASTBLOCK_RDMA_POOL_SLOT_DEAD:
			snap->dead_slots++;
			break;
		default:
			break;
		}
		if (slot->conn && kfastblock_rdma_conn_is_connected(slot->conn))
			snap->connected_slots++;
		reuse += slot->reuse_hits;
		mutex_unlock(&slot->lock);
	}
	snap->reuse_hits = reuse;
}
