#include <linux/string.h>

#include "kfastblock/connpool.h"
#include "kfastblock/meta.h"
#include "kfastblock/recovery.h"
#include "kfastblock/request.h"
#include "kfastblock/scheduler.h"
#include "kfastblock/volume.h"

static bool kfastblock_recovery_should_retry_monitor(int ret)
{
	switch (ret) {
	case -EAGAIN:
	case -ETIMEDOUT:
	case -ECONNRESET:
	case -EPIPE:
	case -ECONNREFUSED:
	case -EHOSTUNREACH:
	case -ENETUNREACH:
	case -EPROTO:
	case -EIO:
		return true;
	default:
		return false;
	}
}

unsigned int kfastblock_recovery_classify_object_failure(int ret)
{
	switch (ret) {
	case -ESTALE:
		return KFASTBLOCK_RECOVERY_KICK_REFRESH;
	case -ENOLINK:
	case -EAGAIN:
	case -EHOSTDOWN:
	case -ETIMEDOUT:
	case -ECONNRESET:
	case -EPIPE:
		return KFASTBLOCK_RECOVERY_DROP_SOCKET |
			KFASTBLOCK_RECOVERY_INVALIDATE_LEADER |
			KFASTBLOCK_RECOVERY_KICK_REFRESH |
			KFASTBLOCK_RECOVERY_RETRY;
	case -EPROTO:
		return KFASTBLOCK_RECOVERY_DROP_SOCKET |
			KFASTBLOCK_RECOVERY_INVALIDATE_LEADER |
			KFASTBLOCK_RECOVERY_KICK_REFRESH;
	case -EIO:
		return KFASTBLOCK_RECOVERY_DROP_SOCKET |
			KFASTBLOCK_RECOVERY_KICK_REFRESH;
	default:
		return 0;
	}
}

unsigned int kfastblock_recovery_classify_leader_failure(int ret)
{
	switch (ret) {
	case -ESTALE:
		return KFASTBLOCK_RECOVERY_INVALIDATE_LEADER |
			KFASTBLOCK_RECOVERY_KICK_REFRESH;
	case -ENOLINK:
	case -EAGAIN:
	case -EHOSTDOWN:
	case -ETIMEDOUT:
	case -ECONNRESET:
	case -EPIPE:
	case -EPROTO:
	case -EIO:
		return KFASTBLOCK_RECOVERY_DROP_SOCKET |
			KFASTBLOCK_RECOVERY_INVALIDATE_LEADER |
			KFASTBLOCK_RECOVERY_KICK_REFRESH;
	default:
		return KFASTBLOCK_RECOVERY_INVALIDATE_LEADER;
	}
}

unsigned int kfastblock_recovery_classify_monitor_failure(int ret)
{
	if (kfastblock_recovery_should_retry_monitor(ret))
		return KFASTBLOCK_RECOVERY_DROP_SOCKET;

	return 0;
}

bool kfastblock_recovery_prefetch_should_fail_request(int ret)
{
	switch (ret) {
	case -EINVAL:
	case -ENOMEM:
	case -E2BIG:
	case -EOPNOTSUPP:
	case -ENOENT:
		return true;
	default:
		return false;
	}
}

blk_status_t kfastblock_recovery_errno_to_blk_status(int ret)
{
	switch (ret) {
	case 0:
		return BLK_STS_OK;
	case -EAGAIN:
	case -ESTALE:
	case -EBUSY:
		return BLK_STS_RESOURCE;
	case -EHOSTDOWN:
	case -ENOLINK:
	case -ETIMEDOUT:
	case -ECONNRESET:
	case -EPIPE:
		return BLK_STS_TRANSPORT;
	default:
		return BLK_STS_IOERR;
	}
}

int kfastblock_recovery_update_live_pg_leader(
	struct kfastblock_volume *vol,
	u32 pool_id,
	u32 pg_id,
	const struct kfastblock_leader_info *leader)
{
	int ret;

	if (!vol || !leader)
		return -EINVAL;

	down_write(&vol->state_lock);
	ret = kfastblock_meta_set_pg_leader(&vol->view, pool_id, pg_id, leader);
	up_write(&vol->state_lock);
	return ret;
}

void kfastblock_recovery_invalidate_live_pg_leader(
	struct kfastblock_volume *vol,
	u32 pool_id,
	u32 pg_id)
{
	if (!vol)
		return;

	down_write(&vol->state_lock);
	kfastblock_meta_invalidate_pg_leader(&vol->view, pool_id, pg_id);
	up_write(&vol->state_lock);
}

static void kfastblock_recovery_mark_osd_socket_failure(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	int ret)
{
	unsigned long backoff_jiffies;

	if (!vol || !cached || !leader)
		return;

	backoff_jiffies = kfastblock_osd_conn_slot_mark_failure_locked(
		cached, leader, ret);
	kfastblock_volume_account_socket_backoff(vol, false, leader->osd_id,
						 leader->port, cached->fail_streak,
						 backoff_jiffies, ret);
}

static void kfastblock_recovery_mark_monitor_socket_failure(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint,
	int ret)
{
	unsigned long backoff_jiffies;

	if (!vol || !cached || !endpoint)
		return;

	backoff_jiffies = kfastblock_monitor_conn_slot_mark_failure_locked(
		cached, endpoint, ret);
	kfastblock_volume_account_socket_backoff(vol, true, 0, endpoint->port,
						 cached->fail_streak,
						 backoff_jiffies, ret);
}

void kfastblock_recovery_finalize_osd_socket(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	int ret,
	unsigned int actions)
{
	if (!cached)
		return;

	if (!ret) {
		mutex_unlock(&cached->lock);
		return;
	}

	if ((actions & KFASTBLOCK_RECOVERY_DROP_SOCKET) &&
	    leader && cached->sock) {
		kfastblock_recovery_mark_osd_socket_failure(vol, cached, leader, ret);
		kfastblock_volume_account_socket_drop(vol, false, leader->osd_id,
						      leader->port, ret);
	} else if (leader && cached->sock) {
		kfastblock_volume_account_socket_drop(vol, false, leader->osd_id,
						      leader->port, ret);
	}

	kfastblock_osd_conn_slot_close_locked(cached);
	mutex_unlock(&cached->lock);
}

void kfastblock_recovery_apply_object_failure(
	struct kfastblock_volume *vol,
	u32 pool_id,
	const struct kfastblock_object_extent *extent,
	enum req_op op,
	const struct kfastblock_leader_info *leader,
	int ret,
	unsigned int actions)
{
	if (!vol || !extent || !actions)
		return;

	if (actions & KFASTBLOCK_RECOVERY_INVALIDATE_LEADER) {
		kfastblock_volume_account_leader_invalidate(vol, extent->pg_id, ret);
		kfastblock_recovery_invalidate_live_pg_leader(vol, pool_id,
							      extent->pg_id);
	}
	if (actions & KFASTBLOCK_RECOVERY_KICK_REFRESH) {
		kfastblock_volume_account_refresh_kick(vol, extent->pg_id, ret);
		kfastblock_volume_kick_refresh(vol);
	}
	if (actions & KFASTBLOCK_RECOVERY_RETRY) {
		kfastblock_scheduler_note_retry(&vol->scheduler);
		kfastblock_volume_account_object_retry(vol, op, extent->pg_id,
						       leader ? leader->osd_id : 0,
						       extent->length, ret);
	}
}

void kfastblock_recovery_apply_leader_failure(
	struct kfastblock_volume *vol,
	u32 pool_id,
	u32 pg_id,
	int ret,
	unsigned int actions)
{
	if (!vol || !actions)
		return;

	if (actions & KFASTBLOCK_RECOVERY_INVALIDATE_LEADER) {
		kfastblock_volume_account_leader_invalidate(vol, pg_id, ret);
		kfastblock_recovery_invalidate_live_pg_leader(vol, pool_id, pg_id);
	}
	if (actions & KFASTBLOCK_RECOVERY_KICK_REFRESH) {
		kfastblock_volume_account_refresh_kick(vol, pg_id, ret);
		kfastblock_volume_kick_refresh(vol);
	}
}

void kfastblock_recovery_finalize_monitor_socket(
	struct kfastblock_volume *vol,
	struct kfastblock_cached_monitor_socket *cached,
	int ret,
	unsigned int actions)
{
	if (!cached)
		return;

	if (!ret) {
		mutex_unlock(&cached->lock);
		return;
	}

	if ((actions & KFASTBLOCK_RECOVERY_DROP_SOCKET) && vol) {
		if (cached->sock) {
			struct kfastblock_monitor_endpoint endpoint = {};

			endpoint.port = cached->port;
			strscpy(endpoint.host, cached->address,
				sizeof(endpoint.host));
			kfastblock_recovery_mark_monitor_socket_failure(
				vol, cached, &endpoint, ret);
			kfastblock_volume_account_socket_drop(vol, true, 0,
							      cached->port, ret);
		}
		kfastblock_monitor_conn_slot_close_locked(cached);
	}

	mutex_unlock(&cached->lock);
}
