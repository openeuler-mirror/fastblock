#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "kfastblock/xport.h"

struct kfastblock_xport_probe_cache_entry {
	char address[KFASTBLOCK_MAX_ADDR_LEN];
	u16 rdma_port;
	int result;
	unsigned long expire_jiffies;
	bool valid;
};

static struct kfastblock_xport_probe_cache_entry
	kfastblock_xport_probe_cache[KFASTBLOCK_XPORT_PROBE_CACHE_SIZE];
static DEFINE_SPINLOCK(kfastblock_xport_probe_cache_lock);
static u64 kfastblock_xport_probe_cache_hit_count;
static u64 kfastblock_xport_probe_cache_miss_count;

void kfastblock_xport_probe_cache_invalidate(void)
{
	unsigned long flags;
	u32 i;

	spin_lock_irqsave(&kfastblock_xport_probe_cache_lock, flags);
	for (i = 0; i < KFASTBLOCK_XPORT_PROBE_CACHE_SIZE; ++i)
		kfastblock_xport_probe_cache[i].valid = false;
	spin_unlock_irqrestore(&kfastblock_xport_probe_cache_lock, flags);
}

u64 kfastblock_xport_probe_cache_hits(void)
{
	return READ_ONCE(kfastblock_xport_probe_cache_hit_count);
}

u64 kfastblock_xport_probe_cache_misses(void)
{
	return READ_ONCE(kfastblock_xport_probe_cache_miss_count);
}

/* Return true and fill *result when a non-expired cache entry matches. */
static bool kfastblock_xport_probe_cache_lookup(
	const struct kfastblock_leader_info *leader, int *result)
{
	unsigned long flags;
	u32 i;
	bool hit = false;

	if (!leader || !result)
		return false;

	spin_lock_irqsave(&kfastblock_xport_probe_cache_lock, flags);
	for (i = 0; i < KFASTBLOCK_XPORT_PROBE_CACHE_SIZE; ++i) {
		struct kfastblock_xport_probe_cache_entry *e =
			&kfastblock_xport_probe_cache[i];

		if (!e->valid)
			continue;
		if (time_after(jiffies, e->expire_jiffies)) {
			e->valid = false;
			continue;
		}
		if (e->rdma_port != leader->rdma_port)
			continue;
		if (strncmp(e->address, leader->address,
			    KFASTBLOCK_MAX_ADDR_LEN) != 0)
			continue;
		*result = e->result;
		hit = true;
		kfastblock_xport_probe_cache_hit_count++;
		break;
	}
	if (!hit)
		kfastblock_xport_probe_cache_miss_count++;
	spin_unlock_irqrestore(&kfastblock_xport_probe_cache_lock, flags);
	return hit;
}

/* Store probe result; overwrite first free/expired or slot 0 (round-robin-ish). */
static void kfastblock_xport_probe_cache_store(
	const struct kfastblock_leader_info *leader, int result)
{
	unsigned long flags;
	u32 i;
	struct kfastblock_xport_probe_cache_entry *slot = NULL;
	unsigned long ttl =
		msecs_to_jiffies(KFASTBLOCK_XPORT_PROBE_CACHE_TTL_MS);

	if (!leader || !leader->address[0])
		return;

	spin_lock_irqsave(&kfastblock_xport_probe_cache_lock, flags);
	for (i = 0; i < KFASTBLOCK_XPORT_PROBE_CACHE_SIZE; ++i) {
		struct kfastblock_xport_probe_cache_entry *e =
			&kfastblock_xport_probe_cache[i];

		if (e->valid && e->rdma_port == leader->rdma_port &&
		    strncmp(e->address, leader->address,
			    KFASTBLOCK_MAX_ADDR_LEN) == 0) {
			slot = e;
			break;
		}
		if (!slot && (!e->valid || time_after(jiffies, e->expire_jiffies)))
			slot = e;
	}
	if (!slot)
		slot = &kfastblock_xport_probe_cache[0];
	strscpy(slot->address, leader->address, sizeof(slot->address));
	slot->rdma_port = leader->rdma_port;
	slot->result = result;
	slot->expire_jiffies = jiffies + ttl;
	slot->valid = true;
	spin_unlock_irqrestore(&kfastblock_xport_probe_cache_lock, flags);
}

static int kfastblock_xport_tcp_probe(const struct kfastblock_leader_info *leader)
{
	if (!kfastblock_leader_has_tcp(leader))
		return -EINVAL;
	return 0;
}

/*
 * Cheap RDMA capability probe: only validate that the leader advertises
 * address + rdma_port. Do NOT open an RDMA CM connection here.
 *
 * Why: xport_select() may run on every I/O path decision / meta refresh.
 * Full connect/disconnect would thrash CM, allocate QP/MR, and add multi-
 * second latency on failure. Real I/O still performs a full connect via
 * kfastblock_rdma_conn_connect() (transport / RDMA pool owns that).
 */
static int kfastblock_xport_rdma_probe(const struct kfastblock_leader_info *leader)
{
	int cached;
	int ret;

	if (kfastblock_xport_probe_cache_lookup(leader, &cached))
		return cached;
	if (!kfastblock_leader_has_rdma(leader))
		ret = -ENOTCONN;
	else
		ret = 0;
	kfastblock_xport_probe_cache_store(leader, ret);
	return ret;
}

static const struct kfastblock_xport_ops kfastblock_xport_tcp = {
	.name = "tcp",
	.transport_id = KFASTBLOCK_OSD_TRANSPORT_TCP,
	.probe = kfastblock_xport_tcp_probe,
};

static const struct kfastblock_xport_ops kfastblock_xport_rdma = {
	.name = "rdma",
	.transport_id = KFASTBLOCK_OSD_TRANSPORT_RDMA,
	.probe = kfastblock_xport_rdma_probe,
};

const struct kfastblock_xport_ops *kfastblock_xport_tcp_ops(void)
{
	return &kfastblock_xport_tcp;
}

const struct kfastblock_xport_ops *kfastblock_xport_rdma_ops(void)
{
	return &kfastblock_xport_rdma;
}

const struct kfastblock_xport_ops *
kfastblock_xport_ops_lookup(u32 transport_id)
{
	switch (transport_id) {
	case KFASTBLOCK_OSD_TRANSPORT_TCP:
		return &kfastblock_xport_tcp;
	case KFASTBLOCK_OSD_TRANSPORT_RDMA:
		return &kfastblock_xport_rdma;
	case KFASTBLOCK_OSD_TRANSPORT_AUTO:
		/* Prefer RDMA when available; caller still falls back. */
		return &kfastblock_xport_rdma;
	default:
		return NULL;
	}
}

const struct kfastblock_xport_ops *
kfastblock_xport_select_explained(u32 preference,
				  const struct kfastblock_leader_info *leader,
				  char *reason, size_t reason_len)
{
	const struct kfastblock_xport_ops *ops;
	const char *why = "forced-tcp";

	if (!kfastblock_xport_preference_valid(preference)) {
		preference = kfastblock_xport_preference_clamp(preference);
		why = "invalid-tcp";
	} else if (preference == KFASTBLOCK_OSD_TRANSPORT_AUTO) {
		ops = &kfastblock_xport_rdma;
		if (ops->probe && !ops->probe(leader)) {
			if (reason && reason_len)
				strscpy(reason, "auto-rdma", reason_len);
			return ops;
		}
		if (reason && reason_len)
			strscpy(reason, "auto-tcp", reason_len);
		return &kfastblock_xport_tcp;
	} else if (preference == KFASTBLOCK_OSD_TRANSPORT_RDMA) {
		why = "forced-rdma";
	}

	ops = kfastblock_xport_ops_lookup(preference);
	if (!ops) {
		ops = &kfastblock_xport_tcp;
		why = "invalid-tcp";
	}
	if (reason && reason_len)
		strscpy(reason, why, reason_len);
	return ops;
}

const struct kfastblock_xport_ops *
kfastblock_xport_select(u32 preference,
			const struct kfastblock_leader_info *leader)
{
	return kfastblock_xport_select_explained(preference, leader, NULL, 0);
}
