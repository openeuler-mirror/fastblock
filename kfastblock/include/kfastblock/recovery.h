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
};

unsigned int kfastblock_recovery_classify_object_failure(int ret);
unsigned int kfastblock_recovery_classify_leader_failure(int ret);
unsigned int kfastblock_recovery_classify_monitor_failure(int ret);
bool kfastblock_recovery_prefetch_should_fail_request(int ret);
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

#endif
