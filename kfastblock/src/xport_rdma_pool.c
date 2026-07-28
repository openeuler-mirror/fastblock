#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kfastblock/xport_rdma.h"
#include "kfastblock/xport_rdma_pool.h"

/* Aggregate across all pools; exposed as module params for quick sysfs peek. */
static unsigned long kfastblock_rdma_pool_hit_total;
static unsigned long kfastblock_rdma_pool_miss_total;
static unsigned long kfastblock_rdma_pool_evict_total;
static unsigned long kfastblock_rdma_pool_reclaim_total;
static unsigned long kfastblock_rdma_pool_invalidate_broken_total;
static unsigned long kfastblock_rdma_pool_put_fail_total;
static unsigned long kfastblock_rdma_pool_destroy_busy_total;
static unsigned long kfastblock_rdma_pool_aged_out_total;
/* Default max_idle applied at pool_init (0 = unlimited). */
static unsigned int kfastblock_rdma_pool_max_idle_default =
	KFASTBLOCK_RDMA_POOL_DEFAULT_MAX_IDLE;
/*
 * Max age for IDLE conns before reuse is refused (seconds). 0 = no age limit.
 * Stale Soft-RoCE / RNIC peers often fail after long idle; prefer reconnect.
 */
static unsigned int kfastblock_rdma_pool_idle_max_age_s;

module_param_named(rdma_pool_hit, kfastblock_rdma_pool_hit_total, ulong, 0444);
MODULE_PARM_DESC(rdma_pool_hit, "RDMA pool get warm-hit total");
module_param_named(rdma_pool_miss, kfastblock_rdma_pool_miss_total, ulong, 0444);
MODULE_PARM_DESC(rdma_pool_miss, "RDMA pool get miss/cold-connect total");
module_param_named(rdma_pool_evict, kfastblock_rdma_pool_evict_total, ulong, 0444);
MODULE_PARM_DESC(rdma_pool_evict, "RDMA pool idle LRU eviction total");
module_param_named(rdma_pool_reclaim, kfastblock_rdma_pool_reclaim_total, ulong,
		   0444);
MODULE_PARM_DESC(rdma_pool_reclaim, "RDMA pool DEAD slot reclaim total");
module_param_named(rdma_pool_invalidate_broken,
		   kfastblock_rdma_pool_invalidate_broken_total, ulong, 0444);
MODULE_PARM_DESC(rdma_pool_invalidate_broken,
		 "RDMA pool invalidate_broken slot total");
module_param_named(rdma_pool_put_fail, kfastblock_rdma_pool_put_fail_total,
		   ulong, 0444);
MODULE_PARM_DESC(rdma_pool_put_fail,
		 "RDMA pool put with ok=false (conn discarded)");
module_param_named(rdma_pool_destroy_busy,
		   kfastblock_rdma_pool_destroy_busy_total, ulong, 0444);
MODULE_PARM_DESC(rdma_pool_destroy_busy,
		 "RDMA pool destroy with BUSY slots remaining");
module_param_named(rdma_pool_aged_out,
		   kfastblock_rdma_pool_aged_out_total, ulong, 0444);
MODULE_PARM_DESC(rdma_pool_aged_out,
		 "RDMA pool idle conn dropped due to max age");
module_param_named(rdma_pool_max_idle, kfastblock_rdma_pool_max_idle_default,
		   uint, 0644);
MODULE_PARM_DESC(rdma_pool_max_idle,
		 "Default max IDLE conns per RDMA pool (0=unlimited)");
module_param_named(rdma_pool_idle_max_age_s,
		   kfastblock_rdma_pool_idle_max_age_s, uint, 0644);
MODULE_PARM_DESC(rdma_pool_idle_max_age_s,
		 "Max IDLE connection age in seconds before drop (0=unlimited)");

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

	BUILD_BUG_ON(KFASTBLOCK_RDMA_POOL_DEFAULT_SLOTS == 0);
	BUILD_BUG_ON(KFASTBLOCK_RDMA_POOL_DEFAULT_MAX_IDLE > 256);

	if (!pool)
		return -EINVAL;
	if (!nr_slots)
		nr_slots = KFASTBLOCK_RDMA_POOL_DEFAULT_SLOTS;
	/* Hard cap to avoid runaway kcalloc under bad module params. */
	if (nr_slots > 256)
		nr_slots = 256;

	memset(pool, 0, sizeof(*pool));
	pool->slots = kcalloc(nr_slots, sizeof(*pool->slots), GFP_KERNEL);
	if (!pool->slots)
		return -ENOMEM;
	pool->nr_slots = nr_slots;
	pool->max_idle = kfastblock_rdma_pool_max_idle_default;
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
	u32 i, busy_n = 0;

	if (!pool)
		return;

	if (pool->slots) {
		for (i = 0; i < pool->nr_slots; ++i) {
			if (pool->slots[i].state ==
			    KFASTBLOCK_RDMA_POOL_SLOT_BUSY)
				busy_n++;
		}
		if (busy_n) {
			pr_warn("rdma_pool: destroy with %u busy slots\n",
				busy_n);
			kfastblock_rdma_pool_destroy_busy_total++;
		}
	}
	kfastblock_rdma_pool_close(pool);
	kfree(pool->slots);
	pool->slots = NULL;
	pool->nr_slots = 0;
	pool->max_idle = 0;
	/* Keep hit/miss counters for post-mortem via snapshot callers. */
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

static void kfastblock_rdma_pool_evict_idle_lru(
	struct kfastblock_rdma_pool *pool,
	struct kfastblock_rdma_pool_slot *skip);

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
	/* Defensive: address+rdma_port must both be present (has_rdma may lag). */
	if (!leader->address[0] || !leader->rdma_port) {
		pool->get_misses++;
		kfastblock_rdma_pool_miss_total++;
		return NULL;
	}

	/* Pass 1: warm reuse (no CM). */
	conn = kfastblock_rdma_pool_try_get(pool, leader);
	if (conn)
		return conn;

	/* Turn DEAD slots back into EMPTY before cold-connect scan. */
	(void)kfastblock_rdma_pool_reclaim_dead(pool);

	/* Pass 1b: if no empty slot, free one LRU idle to make room. */
	{
		u32 empty_n = kfastblock_rdma_pool_count_state(
			pool, KFASTBLOCK_RDMA_POOL_SLOT_EMPTY);
		u32 dead_n = kfastblock_rdma_pool_count_state(
			pool, KFASTBLOCK_RDMA_POOL_SLOT_DEAD);

		if (!empty_n && !dead_n)
			kfastblock_rdma_pool_evict_idle_lru(pool, NULL);
	}

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
				kfastblock_rdma_pool_miss_total++;
				return NULL;
			}
		}
		{
			ktime_t t0 = ktime_get();

			ret = kfastblock_rdma_conn_connect(slot->conn, leader);
			if (!ret) {
				unsigned long us =
					(unsigned long)ktime_us_delta(ktime_get(), t0);

				if (!pool->connect_lat_count ||
				    us < pool->connect_lat_us_min)
					pool->connect_lat_us_min = us;
				if (us > pool->connect_lat_us_max)
					pool->connect_lat_us_max = us;
				pool->connect_lat_us_total += us;
				pool->connect_lat_count++;
			}
		}
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
			kfastblock_rdma_pool_miss_total++;
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

	/*
	 * Phase 1: lockless scan for candidate victim.
	 * Reading state/last_use_jiffies without lock is safe here:
	 * worst case we pick a stale candidate and re-check under lock.
	 */
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		if (slot->state == KFASTBLOCK_RDMA_POOL_SLOT_IDLE) {
			idle_n++;
			if (slot != skip &&
			    (!victim ||
			     time_before(slot->last_use_jiffies, oldest))) {
				victim = slot;
				oldest = slot->last_use_jiffies;
			}
		}
	}

	if (idle_n <= pool->max_idle || !victim)
		return;

	/* Phase 2: confirm under lock and evict. */
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

	/* find_slot returns with slot->lock held. */
	if (slot->conn != conn) {
		mutex_unlock(&slot->lock);
		return;
	}
	if (ok && kfastblock_rdma_conn_is_usable(conn)) {
		slot->state = KFASTBLOCK_RDMA_POOL_SLOT_IDLE;
		slot->success_count++;
		slot->last_use_jiffies = jiffies;
		slot->last_error = 0;
		became_idle = true;
	} else {
		int err = kfastblock_rdma_conn_last_error(conn);

		kfastblock_rdma_pool_put_fail_total++;
		kfastblock_rdma_pool_slot_disconnect_locked(slot);
		slot->state = KFASTBLOCK_RDMA_POOL_SLOT_DEAD;
		slot->failure_count++;
		if (!ok)
			slot->last_error = err ? err : -EIO;
		else if (err)
			slot->last_error = err;
		else
			slot->last_error = -ENOTCONN;
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


static bool kfastblock_rdma_pool_slot_idle_aged_locked(
	const struct kfastblock_rdma_pool_slot *slot)
{
	unsigned int age_s;

	if (!slot || !kfastblock_rdma_pool_idle_max_age_s)
		return false;
	age_s = min_t(unsigned int, kfastblock_rdma_pool_idle_max_age_s, 86400U);
	return kfastblock_rdma_conn_is_aged(slot->conn, age_s);
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
		if (!slot->conn ||
		    !kfastblock_rdma_conn_is_usable(slot->conn) ||
		    !kfastblock_rdma_conn_matches_leader(slot->conn, leader)) {
			kfastblock_rdma_pool_slot_disconnect_locked(slot);
			slot->state = KFASTBLOCK_RDMA_POOL_SLOT_DEAD;
			slot->failure_count++;
			slot->last_error = -ENOTCONN;
			mutex_unlock(&slot->lock);
			continue;
		}
		/* Drop idle conns that sat too long (module param, 0=off). */
		if (kfastblock_rdma_pool_slot_idle_aged_locked(slot)) {
			kfastblock_rdma_pool_slot_disconnect_locked(slot);
			slot->state = KFASTBLOCK_RDMA_POOL_SLOT_DEAD;
			slot->failure_count++;
			slot->last_error = -ETIMEDOUT;
			pool->idle_evictions++;
			kfastblock_rdma_pool_evict_total++;
			kfastblock_rdma_pool_aged_out_total++;
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
			kfastblock_rdma_pool_reclaim_total++;
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
	{
		unsigned long long hits = snap.get_hits;
		unsigned long long misses = snap.get_misses;
		unsigned long long total = hits + misses;
		unsigned int hit_pct = total ? (unsigned int)((hits * 100ULL) / total) : 0;
		unsigned int util_pct = snap.total_slots ?
			(unsigned int)((snap.busy_slots * 100ULL) / snap.total_slots) : 0;
		unsigned long conn_lat_avg = snap.connect_lat_count ?
			(unsigned long)(snap.connect_lat_us_total /
					snap.connect_lat_count) : 0;

		return scnprintf(buf, buf_len,
			 "slots=%u empty=%u idle=%u busy=%u dead=%u connected=%u ready=%u max_idle=%u hits=%llu misses=%llu hit_pct=%u util_pct=%u evict=%llu conn_lat_us=%lu/%lu/%lu n=%u attempts=%llu",
			 snap.total_slots, snap.empty_slots, snap.idle_slots,
			 snap.busy_slots, snap.dead_slots, snap.connected_slots,
			 kfastblock_rdma_pool_ready_count(pool), snap.max_idle,
			 hits, misses, hit_pct, util_pct,
			 (unsigned long long)snap.idle_evictions,
			 snap.connect_lat_us_min, conn_lat_avg,
			 snap.connect_lat_us_max, snap.connect_lat_count,
			 (unsigned long long)snap.total_connect_attempts);
	}
}

void kfastblock_rdma_pool_dump_seq(struct seq_file *m, const char *prefix,
				   struct kfastblock_rdma_pool *pool)
{
	char buf[256];

	if (!m || !pool)
		return;
	if (!prefix)
		prefix = "";
	if (kfastblock_rdma_pool_format_stats(pool, buf, sizeof(buf)) > 0)
		seq_printf(m, "%srdma_pool.stats=%s\n", prefix, buf);
	seq_printf(m, "%srdma_pool.connect_ok=%llu\n", prefix,
		   (unsigned long long)pool->connect_ok);
	seq_printf(m, "%srdma_pool.connect_err=%llu\n", prefix,
		   (unsigned long long)pool->connect_err);
	seq_printf(m, "%srdma_pool.idle_evictions=%llu\n", prefix,
		   (unsigned long long)pool->idle_evictions);
}

struct kfastblock_rdma_pool_slot *
kfastblock_rdma_pool_find_slot(struct kfastblock_rdma_pool *pool,
			       struct kfastblock_rdma_conn *conn)
{
	u32 i;

	if (!pool || !pool->slots || !conn)
		return NULL;
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		if (slot->conn == conn)
			return slot;
		mutex_unlock(&slot->lock);
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
		/*
		 * Match by address+rdma_port only (ignore osd_id / usable).
		 * Never touch BUSY slots mid-I/O; leave them for put().
		 */
		if (slot->state != KFASTBLOCK_RDMA_POOL_SLOT_BUSY &&
		    slot->state != KFASTBLOCK_RDMA_POOL_SLOT_EMPTY &&
		    kfastblock_rdma_pool_slot_endpoint_eq_locked(slot,
								 leader)) {
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
	snap->connect_lat_us_min = pool->connect_lat_us_min;
	snap->connect_lat_us_max = pool->connect_lat_us_max;
	snap->connect_lat_us_total = pool->connect_lat_us_total;
	snap->connect_lat_count = pool->connect_lat_count;

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
		snap->total_connect_attempts += slot->connect_attempts;
		mutex_unlock(&slot->lock);
	}
	snap->reuse_hits = reuse;
}

u32 kfastblock_rdma_pool_set_max_idle(struct kfastblock_rdma_pool *pool,
				      u32 max_idle)
{
	u32 prev;

	if (!pool)
		return 0;
	prev = pool->max_idle;
	if (max_idle && pool->nr_slots && max_idle > pool->nr_slots)
		max_idle = pool->nr_slots;
	pool->max_idle = max_idle;
	/* Opportunistically reclaim if new cap is tighter. */
	if (pool->max_idle)
		kfastblock_rdma_pool_evict_idle_lru(pool, NULL);
	return prev;
}

u32 kfastblock_rdma_pool_ready_count(struct kfastblock_rdma_pool *pool)
{
	u32 i, n = 0;

	if (!pool || !pool->slots)
		return 0;
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		if (slot->state == KFASTBLOCK_RDMA_POOL_SLOT_IDLE &&
		    slot->conn && kfastblock_rdma_conn_is_usable(slot->conn) &&
		    !kfastblock_rdma_pool_slot_idle_aged_locked(slot))
			n++;
		mutex_unlock(&slot->lock);
	}
	return n;
}

u32 kfastblock_rdma_pool_invalidate_broken(struct kfastblock_rdma_pool *pool)
{
	u32 i, n = 0;

	if (!pool || !pool->slots)
		return 0;
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		if (slot->state == KFASTBLOCK_RDMA_POOL_SLOT_BUSY) {
			mutex_unlock(&slot->lock);
			continue;
		}
		if (slot->conn &&
		    !kfastblock_rdma_conn_is_usable(slot->conn)) {
			pr_debug("rdma_pool: invalidate slot %u state=%u\n",
				 i, slot->state);
			kfastblock_rdma_pool_slot_disconnect_locked(slot);
			slot->state = KFASTBLOCK_RDMA_POOL_SLOT_DEAD;
			slot->failure_count++;
			slot->last_error = -ENOTCONN;
			n++;
		}
		mutex_unlock(&slot->lock);
	}
	if (n)
		kfastblock_rdma_pool_invalidate_broken_total += n;
	return n;
}
