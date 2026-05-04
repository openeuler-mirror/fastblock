#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/net.h>
#include <linux/string.h>

#include "kfastblock/connpool.h"

static enum kfastblock_conn_state kfastblock_conn_state_after_reset(bool has_sock,
								    bool connecting)
{
	if (connecting)
		return KFASTBLOCK_CONN_STATE_CONNECTING;
	if (has_sock)
		return KFASTBLOCK_CONN_STATE_READY;
	return KFASTBLOCK_CONN_STATE_EMPTY;
}

static u32 kfastblock_conn_health_increase(u32 health_score, u32 delta)
{
	if (health_score >= 100)
		return 100;
	return min_t(u32, health_score + delta, 100);
}

static u32 kfastblock_conn_health_decrease(u32 health_score)
{
	if (!health_score)
		return 0;
	if (health_score <= 10)
		return 0;
	return health_score - 10;
}

static void kfastblock_conn_pool_snapshot_init(
	struct kfastblock_conn_pool_snapshot *snapshot,
	u32 total_slots)
{
	if (!snapshot)
		return;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->total_slots = total_slots;
	snapshot->min_health_score = U32_MAX;
}

const char *kfastblock_conn_state_name(enum kfastblock_conn_state state)
{
	switch (state) {
	case KFASTBLOCK_CONN_STATE_EMPTY:
		return "empty";
	case KFASTBLOCK_CONN_STATE_CONNECTING:
		return "connecting";
	case KFASTBLOCK_CONN_STATE_READY:
		return "ready";
	case KFASTBLOCK_CONN_STATE_BACKOFF:
		return "backoff";
	default:
		return "unknown";
	}
}

unsigned long kfastblock_conn_backoff_interval(u32 fail_streak)
{
	unsigned long delay_ms = 100;
	u32 shifts = 0;

	if (fail_streak > 1)
		shifts = min_t(u32, fail_streak - 1, 5);
	delay_ms <<= shifts;
	if (delay_ms > 3000)
		delay_ms = 3000;
	return msecs_to_jiffies(delay_ms);
}

void kfastblock_osd_conn_slot_init(struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	memset(cached, 0, sizeof(*cached));
	mutex_init(&cached->lock);
	cached->health_score = 50;
	cached->state = KFASTBLOCK_CONN_STATE_EMPTY;
}

void kfastblock_monitor_conn_slot_init(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	memset(cached, 0, sizeof(*cached));
	mutex_init(&cached->lock);
	cached->health_score = 50;
	cached->state = KFASTBLOCK_CONN_STATE_EMPTY;
}

static void kfastblock_osd_conn_slot_clear_identity_locked(
	struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	memset(cached->address, 0, sizeof(cached->address));
	cached->osd_id = 0;
	cached->port = 0;
}

static void kfastblock_monitor_conn_slot_clear_identity_locked(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	memset(cached->address, 0, sizeof(cached->address));
	cached->port = 0;
}

void kfastblock_osd_conn_slot_close_locked(
	struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	if (cached->sock) {
		sock_release(cached->sock);
		cached->sock = NULL;
	}
	cached->connecting = false;
	cached->next_seq = 0;
	if (cached->backoff_until_jiffies &&
	    time_before(jiffies, cached->backoff_until_jiffies))
		cached->state = KFASTBLOCK_CONN_STATE_BACKOFF;
	else
		cached->state = KFASTBLOCK_CONN_STATE_EMPTY;
}

void kfastblock_monitor_conn_slot_close_locked(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	if (cached->sock) {
		sock_release(cached->sock);
		cached->sock = NULL;
	}
	cached->next_seq = 0;
	if (cached->backoff_until_jiffies &&
	    time_before(jiffies, cached->backoff_until_jiffies))
		cached->state = KFASTBLOCK_CONN_STATE_BACKOFF;
	else
		cached->state = KFASTBLOCK_CONN_STATE_EMPTY;
}

void kfastblock_osd_conn_slot_close(struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	mutex_lock(&cached->lock);
	kfastblock_osd_conn_slot_close_locked(cached);
	mutex_unlock(&cached->lock);
}

void kfastblock_monitor_conn_slot_close(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	mutex_lock(&cached->lock);
	kfastblock_monitor_conn_slot_close_locked(cached);
	mutex_unlock(&cached->lock);
}

void kfastblock_osd_conn_slot_reset_backoff_locked(
	struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	cached->fail_streak = 0;
	cached->last_error = 0;
	cached->last_failure_jiffies = 0;
	cached->backoff_until_jiffies = 0;
	cached->state = kfastblock_conn_state_after_reset(cached->sock != NULL,
							  cached->connecting);
}

void kfastblock_monitor_conn_slot_reset_backoff_locked(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	cached->fail_streak = 0;
	cached->last_error = 0;
	cached->last_failure_jiffies = 0;
	cached->backoff_until_jiffies = 0;
	cached->state = kfastblock_conn_state_after_reset(cached->sock != NULL,
							  false);
}

void kfastblock_osd_conn_slot_reset_backoff(
	struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	mutex_lock(&cached->lock);
	kfastblock_osd_conn_slot_reset_backoff_locked(cached);
	mutex_unlock(&cached->lock);
}

void kfastblock_monitor_conn_slot_reset_backoff(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	mutex_lock(&cached->lock);
	kfastblock_monitor_conn_slot_reset_backoff_locked(cached);
	mutex_unlock(&cached->lock);
}

bool kfastblock_osd_conn_slot_matches_locked(
	const struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader)
{
	if (!cached || !leader || !cached->sock || cached->connecting)
		return false;
	return cached->osd_id == leader->osd_id &&
		cached->port == leader->port &&
		strcmp(cached->address, leader->address) == 0;
}

bool kfastblock_monitor_conn_slot_matches_locked(
	const struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint)
{
	if (!cached || !endpoint || !cached->sock)
		return false;
	return cached->port == endpoint->port &&
		strcmp(cached->address, endpoint->host) == 0;
}

void kfastblock_osd_conn_slot_begin_connect_locked(
	struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	cached->connecting = true;
	cached->connect_attempts++;
	cached->last_use_jiffies = jiffies;
	cached->state = KFASTBLOCK_CONN_STATE_CONNECTING;
}

void kfastblock_monitor_conn_slot_begin_connect_locked(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	cached->connect_attempts++;
	cached->last_use_jiffies = jiffies;
	cached->state = KFASTBLOCK_CONN_STATE_CONNECTING;
}

u64 kfastblock_osd_conn_slot_next_seq_locked(
	struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return 1;

	if (!cached->next_seq)
		cached->next_seq = 1;
	cached->last_use_jiffies = jiffies;
	return cached->next_seq++;
}

u64 kfastblock_monitor_conn_slot_next_seq_locked(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return 1;

	if (!cached->next_seq)
		cached->next_seq = 1;
	cached->last_use_jiffies = jiffies;
	return cached->next_seq++;
}

int kfastblock_osd_conn_slot_backoff_active_locked(
	const struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	unsigned long *remaining_jiffies)
{
	if (!cached || !leader || cached->sock || !cached->backoff_until_jiffies)
		return 0;
	if (cached->osd_id != leader->osd_id || cached->port != leader->port)
		return 0;
	if (strcmp(cached->address, leader->address))
		return 0;
	if (time_before(jiffies, cached->backoff_until_jiffies)) {
		if (remaining_jiffies)
			*remaining_jiffies =
				cached->backoff_until_jiffies - jiffies;
		return -EAGAIN;
	}
	return 0;
}

int kfastblock_monitor_conn_slot_backoff_active_locked(
	const struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint,
	unsigned long *remaining_jiffies)
{
	if (!cached || !endpoint || cached->sock || !cached->backoff_until_jiffies)
		return 0;
	if (cached->port != endpoint->port)
		return 0;
	if (strcmp(cached->address, endpoint->host))
		return 0;
	if (time_before(jiffies, cached->backoff_until_jiffies)) {
		if (remaining_jiffies)
			*remaining_jiffies =
				cached->backoff_until_jiffies - jiffies;
		return -EAGAIN;
	}
	return 0;
}

unsigned long kfastblock_osd_conn_slot_mark_failure_locked(
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	int ret)
{
	unsigned long backoff_jiffies;

	if (!cached || !leader)
		return 0;

	cached->osd_id = leader->osd_id;
	cached->port = leader->port;
	strscpy(cached->address, leader->address, sizeof(cached->address));
	cached->fail_streak = min_t(u32, cached->fail_streak + 1, 8);
	cached->last_error = ret;
	cached->last_failure_jiffies = jiffies;
	backoff_jiffies = kfastblock_conn_backoff_interval(cached->fail_streak);
	cached->backoff_until_jiffies = jiffies + backoff_jiffies;
	cached->failure_count++;
	cached->health_score =
		kfastblock_conn_health_decrease(cached->health_score);
	cached->connecting = false;
	cached->state = KFASTBLOCK_CONN_STATE_BACKOFF;
	return backoff_jiffies;
}

unsigned long kfastblock_monitor_conn_slot_mark_failure_locked(
	struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint,
	int ret)
{
	unsigned long backoff_jiffies;

	if (!cached || !endpoint)
		return 0;

	cached->port = endpoint->port;
	strscpy(cached->address, endpoint->host, sizeof(cached->address));
	cached->fail_streak = min_t(u32, cached->fail_streak + 1, 8);
	cached->last_error = ret;
	cached->last_failure_jiffies = jiffies;
	backoff_jiffies = kfastblock_conn_backoff_interval(cached->fail_streak);
	cached->backoff_until_jiffies = jiffies + backoff_jiffies;
	cached->failure_count++;
	cached->health_score =
		kfastblock_conn_health_decrease(cached->health_score);
	cached->state = KFASTBLOCK_CONN_STATE_BACKOFF;
	return backoff_jiffies;
}

void kfastblock_osd_conn_slot_mark_success_locked(
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader)
{
	if (!cached || !leader)
		return;

	cached->osd_id = leader->osd_id;
	cached->port = leader->port;
	strscpy(cached->address, leader->address, sizeof(cached->address));
	kfastblock_osd_conn_slot_reset_backoff_locked(cached);
	cached->success_count++;
	cached->health_score =
		kfastblock_conn_health_increase(cached->health_score, 8);
	cached->last_connect_jiffies = jiffies;
	cached->last_use_jiffies = jiffies;
	cached->connecting = false;
	cached->state = KFASTBLOCK_CONN_STATE_READY;
}

void kfastblock_monitor_conn_slot_mark_success_locked(
	struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint)
{
	if (!cached || !endpoint)
		return;

	cached->port = endpoint->port;
	strscpy(cached->address, endpoint->host, sizeof(cached->address));
	kfastblock_monitor_conn_slot_reset_backoff_locked(cached);
	cached->success_count++;
	cached->health_score =
		kfastblock_conn_health_increase(cached->health_score, 8);
	cached->last_connect_jiffies = jiffies;
	cached->last_use_jiffies = jiffies;
	cached->state = KFASTBLOCK_CONN_STATE_READY;
}

void kfastblock_osd_conn_slot_note_reuse_locked(
	struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return;

	cached->reuse_hits++;
	cached->last_use_jiffies = jiffies;
	cached->health_score =
		kfastblock_conn_health_increase(cached->health_score, 1);
	cached->state = KFASTBLOCK_CONN_STATE_READY;
}

void kfastblock_monitor_conn_slot_note_reuse_locked(
	struct kfastblock_cached_monitor_socket *cached)
{
	if (!cached)
		return;

	cached->reuse_hits++;
	cached->last_use_jiffies = jiffies;
	cached->health_score =
		kfastblock_conn_health_increase(cached->health_score, 1);
	cached->state = KFASTBLOCK_CONN_STATE_READY;
}

static void kfastblock_conn_pool_account_locked(
	struct kfastblock_conn_pool_snapshot *snapshot,
	u8 state,
	struct socket *sock,
	u32 health_score,
	u32 connect_attempts,
	u32 reuse_hits,
	u32 success_count,
	u32 failure_count,
	unsigned long last_use_jiffies)
{
	if (!snapshot)
		return;

	switch (state) {
	case KFASTBLOCK_CONN_STATE_EMPTY:
		snapshot->empty_slots++;
		break;
	case KFASTBLOCK_CONN_STATE_CONNECTING:
		snapshot->connecting_slots++;
		break;
	case KFASTBLOCK_CONN_STATE_READY:
		snapshot->ready_slots++;
		break;
	case KFASTBLOCK_CONN_STATE_BACKOFF:
		snapshot->backoff_slots++;
		break;
	default:
		break;
	}

	if (sock)
		snapshot->active_sockets++;
	snapshot->connect_attempts += connect_attempts;
	snapshot->reuse_hits += reuse_hits;
	snapshot->success_count += success_count;
	snapshot->failure_count += failure_count;
	if (health_score < snapshot->min_health_score)
		snapshot->min_health_score = health_score;
	if (health_score > snapshot->max_health_score)
		snapshot->max_health_score = health_score;
	snapshot->avg_health_score += health_score;
	if (!snapshot->oldest_last_use_jiffies ||
	    time_before(last_use_jiffies, snapshot->oldest_last_use_jiffies))
		snapshot->oldest_last_use_jiffies = last_use_jiffies;
	if (time_after(last_use_jiffies, snapshot->newest_last_use_jiffies))
		snapshot->newest_last_use_jiffies = last_use_jiffies;
}

static void kfastblock_conn_pool_finalize_average(
	struct kfastblock_conn_pool_snapshot *snapshot)
{
	if (!snapshot)
		return;

	if (snapshot->min_health_score == U32_MAX)
		snapshot->min_health_score = 0;
	if (snapshot->total_slots)
		snapshot->avg_health_score /= snapshot->total_slots;
}

void kfastblock_osd_conn_pool_init(struct kfastblock_cached_socket *slots,
				   u32 nr_slots)
{
	u32 i;

	if (!slots)
		return;

	for (i = 0; i < nr_slots; ++i)
		kfastblock_osd_conn_slot_init(&slots[i]);
}

void kfastblock_monitor_conn_pool_init(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots)
{
	u32 i;

	if (!slots)
		return;

	for (i = 0; i < nr_slots; ++i)
		kfastblock_monitor_conn_slot_init(&slots[i]);
}

void kfastblock_osd_conn_pool_close(struct kfastblock_cached_socket *slots,
				    u32 nr_slots)
{
	u32 i;

	if (!slots)
		return;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_socket *cached = &slots[i];

		mutex_lock(&cached->lock);
		kfastblock_osd_conn_slot_close_locked(cached);
		kfastblock_osd_conn_slot_clear_identity_locked(cached);
		kfastblock_osd_conn_slot_reset_backoff_locked(cached);
		cached->state = KFASTBLOCK_CONN_STATE_EMPTY;
		mutex_unlock(&cached->lock);
	}
}

void kfastblock_monitor_conn_pool_close(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots)
{
	u32 i;

	if (!slots)
		return;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_monitor_socket *cached = &slots[i];

		mutex_lock(&cached->lock);
		kfastblock_monitor_conn_slot_close_locked(cached);
		kfastblock_monitor_conn_slot_clear_identity_locked(cached);
		kfastblock_monitor_conn_slot_reset_backoff_locked(cached);
		cached->state = KFASTBLOCK_CONN_STATE_EMPTY;
		mutex_unlock(&cached->lock);
	}
}

void kfastblock_osd_conn_pool_reset_backoff(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots)
{
	u32 i;

	if (!slots)
		return;

	for (i = 0; i < nr_slots; ++i)
		kfastblock_osd_conn_slot_reset_backoff(&slots[i]);
}

void kfastblock_monitor_conn_pool_reset_backoff(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots)
{
	u32 i;

	if (!slots)
		return;

	for (i = 0; i < nr_slots; ++i)
		kfastblock_monitor_conn_slot_reset_backoff(&slots[i]);
}

void kfastblock_osd_conn_pool_snapshot(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	struct kfastblock_conn_pool_snapshot *snapshot)
{
	u32 i;

	kfastblock_conn_pool_snapshot_init(snapshot, nr_slots);
	if (!slots || !snapshot)
		return;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_socket *cached = &slots[i];

		mutex_lock(&cached->lock);
		kfastblock_conn_pool_account_locked(snapshot, cached->state,
						    cached->sock,
						    cached->health_score,
						    cached->connect_attempts,
						    cached->reuse_hits,
						    cached->success_count,
						    cached->failure_count,
						    cached->last_use_jiffies);
		mutex_unlock(&cached->lock);
	}
	kfastblock_conn_pool_finalize_average(snapshot);
}

void kfastblock_monitor_conn_pool_snapshot(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots,
	struct kfastblock_conn_pool_snapshot *snapshot)
{
	u32 i;

	kfastblock_conn_pool_snapshot_init(snapshot, nr_slots);
	if (!slots || !snapshot)
		return;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_monitor_socket *cached = &slots[i];

		mutex_lock(&cached->lock);
		kfastblock_conn_pool_account_locked(snapshot, cached->state,
						    cached->sock,
						    cached->health_score,
						    cached->connect_attempts,
						    cached->reuse_hits,
						    cached->success_count,
						    cached->failure_count,
						    cached->last_use_jiffies);
		mutex_unlock(&cached->lock);
	}
	kfastblock_conn_pool_finalize_average(snapshot);
}

static struct kfastblock_cached_socket *
kfastblock_osd_conn_pool_acquire_match_common(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	const struct kfastblock_leader_info *leader,
	struct socket **sock_out,
	bool nonblocking)
{
	u32 i;

	if (sock_out)
		*sock_out = NULL;
	if (!slots || !leader)
		return NULL;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_socket *cached = &slots[i];

		if (nonblocking) {
			if (!mutex_trylock(&cached->lock))
				continue;
		} else {
			mutex_lock(&cached->lock);
		}
		if (!kfastblock_osd_conn_slot_matches_locked(cached, leader)) {
			mutex_unlock(&cached->lock);
			continue;
		}
		kfastblock_osd_conn_slot_note_reuse_locked(cached);
		if (sock_out)
			*sock_out = cached->sock;
		return cached;
	}

	return NULL;
}

struct kfastblock_cached_socket *kfastblock_osd_conn_pool_acquire_match(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	const struct kfastblock_leader_info *leader,
	struct socket **sock_out)
{
	return kfastblock_osd_conn_pool_acquire_match_common(
		slots, nr_slots, leader, sock_out, false);
}

struct kfastblock_cached_socket *kfastblock_osd_conn_pool_try_acquire_match(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	const struct kfastblock_leader_info *leader,
	struct socket **sock_out)
{
	return kfastblock_osd_conn_pool_acquire_match_common(
		slots, nr_slots, leader, sock_out, true);
}

struct kfastblock_cached_monitor_socket *
kfastblock_monitor_conn_pool_acquire_match(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots,
	const struct kfastblock_monitor_endpoint *endpoint,
	struct socket **sock_out)
{
	u32 i;

	if (sock_out)
		*sock_out = NULL;
	if (!slots || !endpoint)
		return NULL;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_monitor_socket *cached = &slots[i];

		mutex_lock(&cached->lock);
		if (!kfastblock_monitor_conn_slot_matches_locked(cached, endpoint)) {
			mutex_unlock(&cached->lock);
			continue;
		}
		kfastblock_monitor_conn_slot_note_reuse_locked(cached);
		if (sock_out)
			*sock_out = cached->sock;
		return cached;
	}

	return NULL;
}

int kfastblock_osd_conn_pool_check_backoff(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	const struct kfastblock_leader_info *leader,
	unsigned long *remaining_jiffies)
{
	u32 i;

	if (remaining_jiffies)
		*remaining_jiffies = 0;
	if (!slots || !leader)
		return -EINVAL;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_socket *cached = &slots[i];
		int ret;

		mutex_lock(&cached->lock);
		ret = kfastblock_osd_conn_slot_backoff_active_locked(
			cached, leader, remaining_jiffies);
		mutex_unlock(&cached->lock);
		if (ret)
			return ret;
	}

	return 0;
}

u32 kfastblock_osd_conn_pool_endpoint_active_count(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	const struct kfastblock_leader_info *leader)
{
	u32 i;
	u32 count = 0;

	if (!slots || !leader)
		return 0;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_socket *cached = &slots[i];

		mutex_lock(&cached->lock);
		if (cached->osd_id == leader->osd_id &&
		    cached->port == leader->port &&
		    !strcmp(cached->address, leader->address) &&
		    (cached->sock || cached->connecting))
			count++;
		mutex_unlock(&cached->lock);
	}

	return count;
}

int kfastblock_monitor_conn_pool_check_backoff(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots,
	const struct kfastblock_monitor_endpoint *endpoint,
	unsigned long *remaining_jiffies)
{
	u32 i;

	if (remaining_jiffies)
		*remaining_jiffies = 0;
	if (!slots || !endpoint)
		return -EINVAL;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_monitor_socket *cached = &slots[i];
		int ret;

		mutex_lock(&cached->lock);
		ret = kfastblock_monitor_conn_slot_backoff_active_locked(
			cached, endpoint, remaining_jiffies);
		mutex_unlock(&cached->lock);
		if (ret)
			return ret;
	}

	return 0;
}

static u32 kfastblock_osd_conn_slot_reserve_rank_locked(
	const struct kfastblock_cached_socket *cached)
{
	if (!cached)
		return U32_MAX;
	if (cached->sock || cached->connecting)
		return U32_MAX;
	if (cached->state == KFASTBLOCK_CONN_STATE_EMPTY &&
	    !cached->osd_id && !cached->address[0] && !cached->port)
		return 0;
	if (cached->state == KFASTBLOCK_CONN_STATE_EMPTY)
		return 1;
	if (cached->state == KFASTBLOCK_CONN_STATE_BACKOFF)
		return 2;
	return 3;
}

static bool kfastblock_osd_conn_slot_better_candidate_locked(
	const struct kfastblock_cached_socket *candidate,
	const struct kfastblock_cached_socket *best,
	u32 candidate_rank,
	u32 best_rank)
{
	if (!candidate)
		return false;
	if (!best)
		return true;
	if (candidate_rank != best_rank)
		return candidate_rank < best_rank;
	if (candidate->health_score != best->health_score)
		return candidate->health_score < best->health_score;
	if (candidate->last_use_jiffies != best->last_use_jiffies)
		return time_before(candidate->last_use_jiffies,
				   best->last_use_jiffies);
	return candidate->failure_count > best->failure_count;
}

struct kfastblock_cached_socket *kfastblock_osd_conn_pool_reserve(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots)
{
	u32 i;
	u32 best_rank = U32_MAX;
	struct kfastblock_cached_socket *best = NULL;

	if (!slots)
		return NULL;

	for (i = 0; i < nr_slots; ++i) {
		struct kfastblock_cached_socket *cached = &slots[i];
		u32 rank;

		if (!mutex_trylock(&cached->lock))
			continue;
		rank = kfastblock_osd_conn_slot_reserve_rank_locked(cached);
		if (rank == U32_MAX) {
			mutex_unlock(&cached->lock);
			continue;
		}
		if (kfastblock_osd_conn_slot_better_candidate_locked(cached, best,
								 rank,
								 best_rank)) {
			if (best)
				mutex_unlock(&best->lock);
			best = cached;
			best_rank = rank;
			continue;
		}
		mutex_unlock(&cached->lock);
	}

	if (!best)
		return NULL;

	kfastblock_osd_conn_slot_begin_connect_locked(best);
	return best;
}
