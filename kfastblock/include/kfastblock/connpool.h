#ifndef KFASTBLOCK_CONNPOOL_H
#define KFASTBLOCK_CONNPOOL_H

#include <linux/mutex.h>
#include <linux/socket.h>
#include <linux/types.h>

#include "kfastblock/control.h"
#include "kfastblock/meta.h"

enum kfastblock_conn_state {
	KFASTBLOCK_CONN_STATE_EMPTY = 0,
	KFASTBLOCK_CONN_STATE_CONNECTING = 1,
	KFASTBLOCK_CONN_STATE_READY = 2,
	KFASTBLOCK_CONN_STATE_BACKOFF = 3,
};

struct kfastblock_conn_pool_snapshot {
	u32 total_slots;
	u32 empty_slots;
	u32 connecting_slots;
	u32 ready_slots;
	u32 backoff_slots;
	u32 active_sockets;
	u32 min_health_score;
	u32 max_health_score;
	u32 avg_health_score;
	u64 connect_attempts;
	u64 reuse_hits;
	u64 success_count;
	u64 failure_count;
	unsigned long oldest_last_use_jiffies;
	unsigned long newest_last_use_jiffies;
};

struct kfastblock_cached_socket {
	u32 osd_id;
	u16 port;
	char address[KFASTBLOCK_MAX_ADDR_LEN];
	struct socket *sock;
	struct mutex lock;
	bool connecting;
	u64 next_seq;
	u32 fail_streak;
	s32 last_error;
	unsigned long last_failure_jiffies;
	unsigned long backoff_until_jiffies;
	unsigned long last_connect_jiffies;
	unsigned long last_use_jiffies;
	u32 connect_attempts;
	u32 reuse_hits;
	u32 success_count;
	u32 failure_count;
	u32 health_score;
	u8 state;
};

struct kfastblock_cached_monitor_socket {
	u16 port;
	char address[KFASTBLOCK_MAX_ADDR_LEN];
	struct socket *sock;
	struct mutex lock;
	u64 next_seq;
	u32 fail_streak;
	s32 last_error;
	unsigned long last_failure_jiffies;
	unsigned long backoff_until_jiffies;
	unsigned long last_connect_jiffies;
	unsigned long last_use_jiffies;
	u32 connect_attempts;
	u32 reuse_hits;
	u32 success_count;
	u32 failure_count;
	u32 health_score;
	u8 state;
};

const char *kfastblock_conn_state_name(enum kfastblock_conn_state state);
unsigned long kfastblock_conn_backoff_interval(u32 fail_streak);

void kfastblock_osd_conn_pool_init(struct kfastblock_cached_socket *slots,
				   u32 nr_slots);
void kfastblock_monitor_conn_pool_init(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots);
void kfastblock_osd_conn_pool_close(struct kfastblock_cached_socket *slots,
				    u32 nr_slots);
void kfastblock_monitor_conn_pool_close(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots);
void kfastblock_osd_conn_pool_reset_backoff(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots);
void kfastblock_monitor_conn_pool_reset_backoff(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots);
struct kfastblock_cached_socket *kfastblock_osd_conn_pool_acquire_match(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	const struct kfastblock_leader_info *leader,
	struct socket **sock_out);
struct kfastblock_cached_socket *kfastblock_osd_conn_pool_try_acquire_match(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	const struct kfastblock_leader_info *leader,
	struct socket **sock_out);
struct kfastblock_cached_monitor_socket *
kfastblock_monitor_conn_pool_acquire_match(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots,
	const struct kfastblock_monitor_endpoint *endpoint,
	struct socket **sock_out);
int kfastblock_osd_conn_pool_check_backoff(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	const struct kfastblock_leader_info *leader,
	unsigned long *remaining_jiffies);
u32 kfastblock_osd_conn_pool_endpoint_active_count(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	const struct kfastblock_leader_info *leader);
int kfastblock_monitor_conn_pool_check_backoff(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots,
	const struct kfastblock_monitor_endpoint *endpoint,
	unsigned long *remaining_jiffies);
struct kfastblock_cached_socket *kfastblock_osd_conn_pool_reserve(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots);
void kfastblock_osd_conn_pool_snapshot(
	struct kfastblock_cached_socket *slots,
	u32 nr_slots,
	struct kfastblock_conn_pool_snapshot *snapshot);
void kfastblock_monitor_conn_pool_snapshot(
	struct kfastblock_cached_monitor_socket *slots,
	u32 nr_slots,
	struct kfastblock_conn_pool_snapshot *snapshot);

void kfastblock_osd_conn_slot_init(struct kfastblock_cached_socket *cached);
void kfastblock_monitor_conn_slot_init(
	struct kfastblock_cached_monitor_socket *cached);
void kfastblock_osd_conn_slot_close_locked(
	struct kfastblock_cached_socket *cached);
void kfastblock_monitor_conn_slot_close_locked(
	struct kfastblock_cached_monitor_socket *cached);
void kfastblock_osd_conn_slot_close(struct kfastblock_cached_socket *cached);
void kfastblock_monitor_conn_slot_close(
	struct kfastblock_cached_monitor_socket *cached);
void kfastblock_osd_conn_slot_reset_backoff_locked(
	struct kfastblock_cached_socket *cached);
void kfastblock_monitor_conn_slot_reset_backoff_locked(
	struct kfastblock_cached_monitor_socket *cached);
void kfastblock_osd_conn_slot_reset_backoff(
	struct kfastblock_cached_socket *cached);
void kfastblock_monitor_conn_slot_reset_backoff(
	struct kfastblock_cached_monitor_socket *cached);
bool kfastblock_osd_conn_slot_matches_locked(
	const struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader);
bool kfastblock_monitor_conn_slot_matches_locked(
	const struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint);
void kfastblock_osd_conn_slot_begin_connect_locked(
	struct kfastblock_cached_socket *cached);
void kfastblock_monitor_conn_slot_begin_connect_locked(
	struct kfastblock_cached_monitor_socket *cached);
u64 kfastblock_osd_conn_slot_next_seq_locked(
	struct kfastblock_cached_socket *cached);
u64 kfastblock_monitor_conn_slot_next_seq_locked(
	struct kfastblock_cached_monitor_socket *cached);
int kfastblock_osd_conn_slot_backoff_active_locked(
	const struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	unsigned long *remaining_jiffies);
int kfastblock_monitor_conn_slot_backoff_active_locked(
	const struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint,
	unsigned long *remaining_jiffies);
unsigned long kfastblock_osd_conn_slot_mark_failure_locked(
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader,
	int ret);
unsigned long kfastblock_monitor_conn_slot_mark_failure_locked(
	struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint,
	int ret);
void kfastblock_osd_conn_slot_mark_success_locked(
	struct kfastblock_cached_socket *cached,
	const struct kfastblock_leader_info *leader);
void kfastblock_monitor_conn_slot_mark_success_locked(
	struct kfastblock_cached_monitor_socket *cached,
	const struct kfastblock_monitor_endpoint *endpoint);
void kfastblock_osd_conn_slot_note_reuse_locked(
	struct kfastblock_cached_socket *cached);
void kfastblock_monitor_conn_slot_note_reuse_locked(
	struct kfastblock_cached_monitor_socket *cached);

#endif
