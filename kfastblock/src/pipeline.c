#include <linux/jiffies.h>
#include <linux/slab.h>

#include "kfastblock/pipeline.h"

static void kfastblock_pipeline_reset_entry(
	struct kfastblock_pipeline_entry *entry)
{
	if (!entry)
		return;

	entry->seq = 0;
	entry->object_index = 0;
	entry->last_error = 0;
	entry->response_body_len = 0;
	entry->transport_flags = 0;
	entry->response_status = 0;
	entry->service = 0;
	entry->opcode = 0;
	entry->queued_jiffies = 0;
	entry->completed_jiffies = 0;
	entry->active = false;
	INIT_LIST_HEAD(&entry->link);
}

int kfastblock_pipeline_init(struct kfastblock_pipeline_state *state,
			     unsigned int capacity,
			     gfp_t gfp)
{
	unsigned int i;

	if (!state)
		return -EINVAL;

	memset(state, 0, sizeof(*state));
	spin_lock_init(&state->lock);
	INIT_LIST_HEAD(&state->free_list);
	INIT_LIST_HEAD(&state->inflight_list);
	if (!capacity)
		return 0;

	state->entries = kvcalloc(capacity, sizeof(*state->entries), gfp);
	if (!state->entries)
		return -ENOMEM;

	state->capacity = capacity;
	for (i = 0; i < capacity; ++i) {
		kfastblock_pipeline_reset_entry(&state->entries[i]);
		list_add_tail(&state->entries[i].link, &state->free_list);
	}

	return 0;
}

void kfastblock_pipeline_reset(struct kfastblock_pipeline_state *state)
{
	unsigned long flags;
	unsigned int i;

	if (!state)
		return;

	spin_lock_irqsave(&state->lock, flags);
	INIT_LIST_HEAD(&state->free_list);
	INIT_LIST_HEAD(&state->inflight_list);
	state->inflight = 0;
	state->peak_inflight = 0;
	for (i = 0; i < state->capacity; ++i) {
		kfastblock_pipeline_reset_entry(&state->entries[i]);
		list_add_tail(&state->entries[i].link, &state->free_list);
	}
	spin_unlock_irqrestore(&state->lock, flags);
}

void kfastblock_pipeline_cleanup(struct kfastblock_pipeline_state *state)
{
	if (!state)
		return;

	kvfree(state->entries);
	state->entries = NULL;
	state->capacity = 0;
	state->inflight = 0;
	state->peak_inflight = 0;
	INIT_LIST_HEAD(&state->free_list);
	INIT_LIST_HEAD(&state->inflight_list);
}

struct kfastblock_pipeline_entry *kfastblock_pipeline_find_locked(
	struct kfastblock_pipeline_state *state,
	u64 seq)
{
	struct kfastblock_pipeline_entry *entry;

	if (!state || !seq)
		return NULL;

	list_for_each_entry(entry, &state->inflight_list, link) {
		if (entry->active && entry->seq == seq)
			return entry;
	}

	return NULL;
}

bool kfastblock_pipeline_lookup(struct kfastblock_pipeline_state *state,
				u64 seq,
				struct kfastblock_pipeline_entry *snapshot)
{
	struct kfastblock_pipeline_entry *entry;
	unsigned long flags;
	bool found = false;

	if (!state || !seq || !snapshot)
		return false;

	spin_lock_irqsave(&state->lock, flags);
	entry = kfastblock_pipeline_find_locked(state, seq);
	if (entry) {
		*snapshot = *entry;
		found = true;
	}
	spin_unlock_irqrestore(&state->lock, flags);

	return found;
}

bool kfastblock_pipeline_has_seq(struct kfastblock_pipeline_state *state,
				 u64 seq)
{
	struct kfastblock_pipeline_entry snapshot = {};

	return kfastblock_pipeline_lookup(state, seq, &snapshot);
}

bool kfastblock_pipeline_empty(struct kfastblock_pipeline_state *state)
{
	return kfastblock_pipeline_inflight_entries(state) == 0;
}

bool kfastblock_pipeline_full(struct kfastblock_pipeline_state *state)
{
	unsigned long flags;
	bool full = false;

	if (!state)
		return false;

	spin_lock_irqsave(&state->lock, flags);
	full = state->capacity != 0 && state->inflight >= state->capacity;
	spin_unlock_irqrestore(&state->lock, flags);

	return full;
}

bool kfastblock_pipeline_has_free_entries(
	struct kfastblock_pipeline_state *state)
{
	return kfastblock_pipeline_free_entries(state) > 0;
}

u64 kfastblock_pipeline_oldest_inflight_seq(
	struct kfastblock_pipeline_state *state)
{
	struct kfastblock_pipeline_snapshot snapshot = {};

	kfastblock_pipeline_snapshot(state, &snapshot);
	return snapshot.oldest_inflight_seq;
}

u64 kfastblock_pipeline_newest_inflight_seq(
	struct kfastblock_pipeline_state *state)
{
	struct kfastblock_pipeline_snapshot snapshot = {};

	kfastblock_pipeline_snapshot(state, &snapshot);
	return snapshot.newest_inflight_seq;
}

unsigned long kfastblock_pipeline_oldest_queued_jiffies(
	struct kfastblock_pipeline_state *state)
{
	struct kfastblock_pipeline_snapshot snapshot = {};

	kfastblock_pipeline_snapshot(state, &snapshot);
	return snapshot.oldest_queued_jiffies;
}

unsigned long kfastblock_pipeline_newest_queued_jiffies(
	struct kfastblock_pipeline_state *state)
{
	struct kfastblock_pipeline_snapshot snapshot = {};

	kfastblock_pipeline_snapshot(state, &snapshot);
	return snapshot.newest_queued_jiffies;
}

unsigned int kfastblock_pipeline_spare_capacity(
	struct kfastblock_pipeline_state *state)
{
	unsigned int spare = 0;
	unsigned long flags;

	if (!state)
		return 0;

	spin_lock_irqsave(&state->lock, flags);
	if (state->capacity > state->inflight)
		spare = state->capacity - state->inflight;
	spin_unlock_irqrestore(&state->lock, flags);

	return spare;
}

u32 kfastblock_pipeline_utilization_pct(
	struct kfastblock_pipeline_state *state)
{
	struct kfastblock_pipeline_snapshot snapshot = {};

	kfastblock_pipeline_snapshot(state, &snapshot);
	if (!snapshot.capacity)
		return 0;

	return min_t(u32, 100,
		     (snapshot.inflight * 100) / snapshot.capacity);
}

struct kfastblock_pipeline_entry *kfastblock_pipeline_enqueue(
	struct kfastblock_pipeline_state *state,
	u64 seq,
	unsigned int object_index)
{
	return kfastblock_pipeline_begin_exchange(state, seq, object_index, 0, 0);
}

struct kfastblock_pipeline_entry *kfastblock_pipeline_begin_exchange(
	struct kfastblock_pipeline_state *state,
	u64 seq,
	unsigned int object_index,
	u8 service,
	u8 opcode)
{
	struct kfastblock_pipeline_entry *entry = NULL;
	unsigned long flags;

	if (!state || !seq)
		return NULL;

	spin_lock_irqsave(&state->lock, flags);
	if (!list_empty(&state->free_list)) {
		entry = list_first_entry(&state->free_list,
					 struct kfastblock_pipeline_entry,
					 link);
		list_del_init(&entry->link);
		entry->seq = seq;
		entry->object_index = object_index;
		entry->last_error = 0;
		entry->response_body_len = 0;
		entry->transport_flags = 0;
		entry->response_status = 0;
		entry->service = service;
		entry->opcode = opcode;
		entry->queued_jiffies = jiffies;
		entry->completed_jiffies = 0;
		entry->active = true;
		list_add_tail(&entry->link, &state->inflight_list);
		state->inflight++;
		if (state->inflight > state->peak_inflight)
			state->peak_inflight = state->inflight;
	}
	spin_unlock_irqrestore(&state->lock, flags);

	return entry;
}

struct kfastblock_pipeline_entry *kfastblock_pipeline_complete(
	struct kfastblock_pipeline_state *state,
	u64 seq,
	int ret)
{
	return kfastblock_pipeline_finish_exchange(state, seq, ret, ret, 0, 0);
}

struct kfastblock_pipeline_entry *kfastblock_pipeline_finish_exchange(
	struct kfastblock_pipeline_state *state,
	u64 seq,
	int ret,
	s32 response_status,
	u32 response_body_len,
	u32 transport_flags)
{
	struct kfastblock_pipeline_entry *entry;
	unsigned long flags;

	if (!state || !seq)
		return NULL;

	spin_lock_irqsave(&state->lock, flags);
	entry = kfastblock_pipeline_find_locked(state, seq);
	if (!entry) {
		spin_unlock_irqrestore(&state->lock, flags);
		return NULL;
	}

	list_del_init(&entry->link);
	entry->last_error = ret;
	entry->response_status = response_status;
	entry->response_body_len = response_body_len;
	entry->transport_flags = transport_flags;
	entry->completed_jiffies = jiffies;
	entry->active = false;
	if (state->inflight)
		state->inflight--;
	list_add_tail(&entry->link, &state->free_list);
	spin_unlock_irqrestore(&state->lock, flags);

	return entry;
}

unsigned int kfastblock_pipeline_free_entries(
	struct kfastblock_pipeline_state *state)
{
	unsigned long flags;
	unsigned int free_entries = 0;
	struct kfastblock_pipeline_entry *entry;

	if (!state)
		return 0;

	spin_lock_irqsave(&state->lock, flags);
	list_for_each_entry(entry, &state->free_list, link)
		free_entries++;
	spin_unlock_irqrestore(&state->lock, flags);

	return free_entries;
}

unsigned int kfastblock_pipeline_inflight_entries(
	struct kfastblock_pipeline_state *state)
{
	unsigned int inflight = 0;
	unsigned long flags;

	if (!state)
		return 0;

	spin_lock_irqsave(&state->lock, flags);
	inflight = state->inflight;
	spin_unlock_irqrestore(&state->lock, flags);

	return inflight;
}

bool kfastblock_pipeline_peak_reached(
	struct kfastblock_pipeline_state *state)
{
	unsigned long flags;
	bool reached = false;

	if (!state)
		return false;

	spin_lock_irqsave(&state->lock, flags);
	reached = state->peak_inflight != 0 &&
		  state->inflight >= state->peak_inflight;
	spin_unlock_irqrestore(&state->lock, flags);

	return reached;
}

void kfastblock_pipeline_snapshot(struct kfastblock_pipeline_state *state,
				  struct kfastblock_pipeline_snapshot *snapshot)
{
	unsigned long flags;
	struct kfastblock_pipeline_entry *entry;

	if (!state || !snapshot)
		return;

	spin_lock_irqsave(&state->lock, flags);
	snapshot->capacity = state->capacity;
	snapshot->inflight = state->inflight;
	snapshot->peak_inflight = state->peak_inflight;
	snapshot->free_entries = 0;
	list_for_each_entry(entry, &state->free_list, link)
		snapshot->free_entries++;
	list_for_each_entry(entry, &state->inflight_list, link) {
		if (!snapshot->oldest_inflight_seq ||
		    entry->queued_jiffies < snapshot->oldest_queued_jiffies) {
			snapshot->oldest_inflight_seq = entry->seq;
			snapshot->oldest_queued_jiffies = entry->queued_jiffies;
		}
		if (entry->queued_jiffies >= snapshot->newest_queued_jiffies) {
			snapshot->newest_inflight_seq = entry->seq;
			snapshot->newest_queued_jiffies = entry->queued_jiffies;
		}
	}
	spin_unlock_irqrestore(&state->lock, flags);
}
