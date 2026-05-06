#ifndef KFASTBLOCK_PIPELINE_H
#define KFASTBLOCK_PIPELINE_H

#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct kfastblock_pipeline_entry {
	struct list_head link;
	u64 seq;
	unsigned int object_index;
	int last_error;
	u32 response_body_len;
	u32 transport_flags;
	s32 response_status;
	u8 service;
	u8 opcode;
	unsigned long queued_jiffies;
	unsigned long completed_jiffies;
	bool active;
};

struct kfastblock_pipeline_snapshot {
	unsigned int capacity;
	unsigned int inflight;
	unsigned int peak_inflight;
	unsigned int free_entries;
};

struct kfastblock_pipeline_state {
	spinlock_t lock;
	struct list_head free_list;
	struct list_head inflight_list;
	struct kfastblock_pipeline_entry *entries;
	unsigned int capacity;
	unsigned int inflight;
	unsigned int peak_inflight;
};

int kfastblock_pipeline_init(struct kfastblock_pipeline_state *state,
			     unsigned int capacity,
			     gfp_t gfp);
void kfastblock_pipeline_reset(struct kfastblock_pipeline_state *state);
void kfastblock_pipeline_cleanup(struct kfastblock_pipeline_state *state);
struct kfastblock_pipeline_entry *kfastblock_pipeline_enqueue(
	struct kfastblock_pipeline_state *state,
	u64 seq,
	unsigned int object_index);
struct kfastblock_pipeline_entry *kfastblock_pipeline_begin_exchange(
	struct kfastblock_pipeline_state *state,
	u64 seq,
	unsigned int object_index,
	u8 service,
	u8 opcode);
struct kfastblock_pipeline_entry *kfastblock_pipeline_find_locked(
	struct kfastblock_pipeline_state *state,
	u64 seq);
bool kfastblock_pipeline_lookup(struct kfastblock_pipeline_state *state,
				u64 seq,
				struct kfastblock_pipeline_entry *snapshot);
struct kfastblock_pipeline_entry *kfastblock_pipeline_complete(
	struct kfastblock_pipeline_state *state,
	u64 seq,
	int ret);
struct kfastblock_pipeline_entry *kfastblock_pipeline_finish_exchange(
	struct kfastblock_pipeline_state *state,
	u64 seq,
	int ret,
	s32 response_status,
	u32 response_body_len,
	u32 transport_flags);
void kfastblock_pipeline_snapshot(struct kfastblock_pipeline_state *state,
				  struct kfastblock_pipeline_snapshot *snapshot);

#endif
