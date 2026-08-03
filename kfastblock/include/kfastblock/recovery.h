#ifndef KFASTBLOCK_RECOVERY_H
#define KFASTBLOCK_RECOVERY_H

#include <linux/blkdev.h>
#include <linux/types.h>

struct kfastblock_volume;
struct kfastblock_cached_socket;
struct kfastblock_cached_monitor_socket;
struct kfastblock_leader_info;
struct kfastblock_object_extent;

enum kfastblock_recovery_action {
	KFASTBLOCK_RECOVERY_DROP_SOCKET = 1U << 0,
	KFASTBLOCK_RECOVERY_INVALIDATE_LEADER = 1U << 1,
	KFASTBLOCK_RECOVERY_KICK_REFRESH = 1U << 2,
	KFASTBLOCK_RECOVERY_RETRY = 1U << 3,
	/* Drop cached RDMA conn for peer so next I/O reconnects. */
	KFASTBLOCK_RECOVERY_INVALIDATE_RDMA = 1U << 4,
};

unsigned int kfastblock_recovery_classify_object_failure(int ret);
unsigned int kfastblock_recovery_classify_leader_failure(int ret);
unsigned int kfastblock_recovery_classify_monitor_failure(int ret);
bool kfastblock_recovery_prefetch_should_fail_request(int ret);
/* True for errno values that should drop/reconnect transport. */
bool kfastblock_recovery_is_transport_errno(int ret);
blk_status_t kfastblock_recovery_errno_to_blk_status(int ret);
int kfastblock_recovery_update_live_pg_leader(
	struct kfastblock_volume *vol,
	u32 pool_id,
	u32 pg_id,
	const struct kfastblock_leader_info *leader);
void kfastblock_recovery_invalidate_live_pg_leader(
	struct kfastblock_volume *vol,
	u32 pool_id,
	u32 pg_id);
void kfastblock_recovery_finalize_osd_socket(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	int ret,
	unsigned int actions);
void kfastblock_recovery_apply_object_failure(
	struct kfastblock_volume *vol,
	u32 pool_id,
	const struct kfastblock_object_extent *extent,
	enum req_op op,
	const struct kfastblock_leader_info *leader,
	int ret,
	unsigned int actions);
void kfastblock_recovery_apply_leader_failure(
	struct kfastblock_volume *vol,
	u32 pool_id,
	u32 pg_id,
	int ret,
	unsigned int actions);
void kfastblock_recovery_finalize_monitor_socket(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_monitor_socket *cached,
	int ret,
	unsigned int actions);

/*
 * Invalidate cached RDMA connection(s) for @leader (address:rdma_port).
 * Safe when leader is NULL (no-op). Does not require holding slot locks
 * from the caller; takes per-slot mutexes itself.
 */
void kfastblock_recovery_invalidate_rdma_for_leader(
	struct kfastblock_volume *vol,
	const struct kfastblock_leader_info *leader);

/* Close every RDMA cache slot on the volume (manual flush / detach). */
void kfastblock_recovery_flush_rdma_cache(struct kfastblock_volume *vol);

/* Format @actions bitmask into a short comma-separated token list. */
void kfastblock_recovery_format_actions(unsigned int actions,
					char *buf, size_t buf_len);

/* True when actions request RDMA cache invalidation. */
static inline bool
kfastblock_recovery_actions_invalidate_rdma(unsigned int actions)
{
	return (actions & KFASTBLOCK_RECOVERY_INVALIDATE_RDMA) != 0;
}

#endif
