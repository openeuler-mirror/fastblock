#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "kfastblock/xport.h"

/* Overridable probe-cache TTL (ms); 0 disables caching. */
static unsigned int kfastblock_xport_probe_cache_ttl_ms =
	KFASTBLOCK_XPORT_PROBE_CACHE_TTL_MS;
module_param_named(xport_probe_cache_ttl_ms, kfastblock_xport_probe_cache_ttl_ms,
		   uint, 0644);
MODULE_PARM_DESC(xport_probe_cache_ttl_ms,
		 "RDMA xport probe cache TTL in ms (0=disable)");

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

/* Read-only hit/miss counters for operators (also available via diag dump). */
module_param_named(xport_probe_cache_hits, kfastblock_xport_probe_cache_hit_count,
		   ullong, 0444);
MODULE_PARM_DESC(xport_probe_cache_hits, "RDMA xport probe cache hit count");
module_param_named(xport_probe_cache_misses,
		   kfastblock_xport_probe_cache_miss_count, ullong, 0444);
MODULE_PARM_DESC(xport_probe_cache_misses, "RDMA xport probe cache miss count");

void kfastblock_xport_probe_cache_invalidate(void)
{
	unsigned long flags;
	u32 i;

	spin_lock_irqsave(&kfastblock_xport_probe_cache_lock, flags);
	for (i = 0; i < KFASTBLOCK_XPORT_PROBE_CACHE_SIZE; ++i)
		kfastblock_xport_probe_cache[i].valid = false;
	spin_unlock_irqrestore(&kfastblock_xport_probe_cache_lock, flags);
}

void kfastblock_xport_probe_cache_invalidate_leader(
	const struct kfastblock_leader_info *leader)
{
	unsigned long flags;
	u32 i;

	if (!leader)
		return;
	spin_lock_irqsave(&kfastblock_xport_probe_cache_lock, flags);
	for (i = 0; i < KFASTBLOCK_XPORT_PROBE_CACHE_SIZE; ++i) {
		struct kfastblock_xport_probe_cache_entry *e =
			&kfastblock_xport_probe_cache[i];

		if (!e->valid)
			continue;
		if (e->rdma_port != leader->rdma_port)
			continue;
		if (strncmp(e->address, leader->address,
			    KFASTBLOCK_MAX_ADDR_LEN) != 0)
			continue;
		e->valid = false;
	}
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

u32 kfastblock_xport_probe_cache_valid_count(void)
{
	unsigned long flags;
	u32 i, n = 0;

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
		n++;
	}
	spin_unlock_irqrestore(&kfastblock_xport_probe_cache_lock, flags);
	return n;
}

int kfastblock_xport_format_leader(const struct kfastblock_leader_info *leader,
				   char *buf, size_t buf_len)
{
	if (!buf || !buf_len)
		return -EINVAL;
	if (!leader) {
		buf[0] = '\0';
		return -EINVAL;
	}
	return scnprintf(buf, buf_len, "%s tcp=%u rdma=%u osd=%u",
			 leader->address[0] ? leader->address : "-",
			 leader->port, leader->rdma_port, leader->osd_id);
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
	unsigned long ttl;

	if (!leader || !leader->address[0])
		return;
	if (!kfastblock_xport_probe_cache_ttl_ms)
		return;
	ttl = msecs_to_jiffies(kfastblock_xport_probe_cache_ttl_ms);

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

	if (kfastblock_xport_probe_cache_ttl_ms &&
	    kfastblock_xport_probe_cache_lookup(leader, &cached))
		return cached;
	if (!kfastblock_leader_has_rdma(leader))
		ret = -ENOTCONN;
	else
		ret = 0;
	if (kfastblock_xport_probe_cache_ttl_ms)
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

/*
 * Select + format a one-line decision string into @line for pr_debug callers.
 * Example: "pref=auto reason=auto-rdma ops=rdma leader=1.2.3.4 tcp=7000 rdma=7100 osd=1"
 */
int kfastblock_xport_select_describe(u32 preference,
				     const struct kfastblock_leader_info *leader,
				     char *line, size_t line_len)
{
	const struct kfastblock_xport_ops *ops;
	char reason[24];
	char endpoint[KFASTBLOCK_MAX_ADDR_LEN + 64];

	if (!line || !line_len)
		return -EINVAL;
	ops = kfastblock_xport_select_explained(preference, leader, reason,
						sizeof(reason));
	kfastblock_xport_format_leader(leader, endpoint, sizeof(endpoint));
	return scnprintf(line, line_len, "pref=%s reason=%s ops=%s leader={%s}",
			 kfastblock_xport_preference_name(preference), reason,
			 kfastblock_xport_ops_name(ops), endpoint);
}
