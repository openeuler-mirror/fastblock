#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kfastblock/xport_rdma.h"
#include "kfastblock/xport_rdma_pool.h"

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

static bool kfastblock_rdma_pool_slot_matches_locked(
	const struct kfastblock_rdma_pool_slot *slot,
	const struct kfastblock_leader_info *leader)
{
	if (!slot || !leader)
		return false;
	if (!slot->conn || slot->state == KFASTBLOCK_RDMA_POOL_SLOT_EMPTY)
		return false;
	if (slot->osd_id != leader->osd_id)
		return false;
	if (slot->rdma_port != leader->rdma_port)
		return false;
	return strncmp(slot->address, leader->address,
		       KFASTBLOCK_MAX_ADDR_LEN) == 0;
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

	/* Pass 1: reuse idle matching connected slot. */
	for (i = 0; i < pool->nr_slots; ++i) {
		struct kfastblock_rdma_pool_slot *slot = &pool->slots[i];

		mutex_lock(&slot->lock);
		if (slot->state == KFASTBLOCK_RDMA_POOL_SLOT_IDLE &&
		    kfastblock_rdma_pool_slot_matches_locked(slot, leader) &&
		    kfastblock_rdma_conn_is_connected(slot->conn)) {
			slot->state = KFASTBLOCK_RDMA_POOL_SLOT_BUSY;
			slot->reuse_hits++;
			slot->last_use_jiffies = jiffies;
			pool->get_hits++;
			conn = slot->conn;
			mutex_unlock(&slot->lock);
			return conn;
		}
		mutex_unlock(&slot->lock);
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
		conn = slot->conn;
		mutex_unlock(&slot->lock);
		return conn;
	}

	pool->get_misses++;
	return NULL;
}
