#include <linux/errno.h>
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
