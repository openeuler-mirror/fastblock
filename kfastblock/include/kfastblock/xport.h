#ifndef KFASTBLOCK_XPORT_H
#define KFASTBLOCK_XPORT_H

#include <linux/types.h>

#include "kfastblock/common.h"
#include "kfastblock/meta.h"

/*
 * OSD data-plane transport backend.
 * Monitor control-plane is always raw TCP and is out of scope here.
 */
struct kfastblock_xport_ops {
	const char *name;
	u32 transport_id; /* KFASTBLOCK_OSD_TRANSPORT_* */
	/* Return 0 if this backend can talk to leader endpoint. */
	int (*probe)(const struct kfastblock_leader_info *leader);
};

const struct kfastblock_xport_ops *
kfastblock_xport_ops_lookup(u32 transport_id);
const struct kfastblock_xport_ops *kfastblock_xport_tcp_ops(void);
const struct kfastblock_xport_ops *kfastblock_xport_rdma_ops(void);

/*
 * Choose a backend for one OSD endpoint according to attach preference.
 * AUTO prefers RDMA when probe succeeds, otherwise TCP.
 */
const struct kfastblock_xport_ops *
kfastblock_xport_select(u32 preference,
			const struct kfastblock_leader_info *leader);

/*
 * Same as select, but fills @reason with a short stable token for logs:
 * "auto-rdma", "auto-tcp", "forced-rdma", "forced-tcp", "invalid-tcp".
 * @reason_len includes the trailing NUL; ignored if reason is NULL.
 */
const struct kfastblock_xport_ops *
kfastblock_xport_select_explained(u32 preference,
				  const struct kfastblock_leader_info *leader,
				  char *reason, size_t reason_len);

/* True when preference is RDMA or AUTO (caller still probes). */
static inline bool kfastblock_xport_prefers_rdma(u32 preference)
{
	return preference == KFASTBLOCK_OSD_TRANSPORT_RDMA ||
	       preference == KFASTBLOCK_OSD_TRANSPORT_AUTO;
}

static inline const char *kfastblock_xport_preference_name(u32 preference)
{
	switch (preference) {
	case KFASTBLOCK_OSD_TRANSPORT_TCP:
		return "tcp";
	case KFASTBLOCK_OSD_TRANSPORT_RDMA:
		return "rdma";
	case KFASTBLOCK_OSD_TRANSPORT_AUTO:
		return "auto";
	default:
		return "unknown";
	}
}

/* True for TCP / RDMA / AUTO; false for unknown values. */
static inline bool kfastblock_xport_preference_valid(u32 preference)
{
	return preference <= KFASTBLOCK_OSD_TRANSPORT_MAX;
}

/* Clamp unknown preference to default (TCP). */
static inline u32 kfastblock_xport_preference_clamp(u32 preference)
{
	if (!kfastblock_xport_preference_valid(preference))
		return KFASTBLOCK_DEFAULT_OSD_TRANSPORT;
	return preference;
}

/* Safe ops name for logs; never returns NULL. */
static inline const char *
kfastblock_xport_ops_name(const struct kfastblock_xport_ops *ops)
{
	if (!ops || !ops->name)
		return "none";
	return ops->name;
}

/* Transport id from ops, or TCP when ops is NULL. */
static inline u32
kfastblock_xport_ops_id(const struct kfastblock_xport_ops *ops)
{
	if (!ops)
		return KFASTBLOCK_OSD_TRANSPORT_TCP;
	return ops->transport_id;
}

/* True when selected ops is the RDMA backend. */
static inline bool
kfastblock_xport_ops_is_rdma(const struct kfastblock_xport_ops *ops)
{
	return ops && ops->transport_id == KFASTBLOCK_OSD_TRANSPORT_RDMA;
}

/* True when selected ops is the TCP backend. */
static inline bool
kfastblock_xport_ops_is_tcp(const struct kfastblock_xport_ops *ops)
{
	return !ops || ops->transport_id == KFASTBLOCK_OSD_TRANSPORT_TCP;
}

/* Short-TTL RDMA probe cache (address:rdma_port -> result). */
#define KFASTBLOCK_XPORT_PROBE_CACHE_TTL_MS 2000U
#define KFASTBLOCK_XPORT_PROBE_CACHE_SIZE 16U

void kfastblock_xport_probe_cache_invalidate(void);
u64 kfastblock_xport_probe_cache_hits(void);
u64 kfastblock_xport_probe_cache_misses(void);
/* Number of currently valid (non-expired) probe cache entries. */
u32 kfastblock_xport_probe_cache_valid_count(void);

/* Format leader endpoint for logs: "addr:tcp/rdma" into @buf. */
int kfastblock_xport_format_leader(const struct kfastblock_leader_info *leader,
				   char *buf, size_t buf_len);

#endif
