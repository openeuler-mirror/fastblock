#ifndef KFASTBLOCK_SCHEDULER_H
#define KFASTBLOCK_SCHEDULER_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct kfastblock_scheduler_controller {
	spinlock_t lock;
	u32 base_window;
	u32 current_window;
	u32 min_window;
	u32 max_window;
	u32 consecutive_successes;
	u32 shrink_events;
	u32 grow_events;
	u32 retry_events;
	u32 dispatch_failures;
	bool dynamic_enabled;
};

void kfastblock_scheduler_init(struct kfastblock_scheduler_controller *controller,
			       u32 base_window,
			       u32 min_window,
			       u32 max_window);
void kfastblock_scheduler_set_base_window(
	struct kfastblock_scheduler_controller *controller,
	u32 base_window);
void kfastblock_scheduler_set_dynamic_enabled(
	struct kfastblock_scheduler_controller *controller,
	bool enabled);
u32 kfastblock_scheduler_snapshot_window(
	struct kfastblock_scheduler_controller *controller);
void kfastblock_scheduler_note_success(
	struct kfastblock_scheduler_controller *controller);
void kfastblock_scheduler_note_retry(
	struct kfastblock_scheduler_controller *controller);
void kfastblock_scheduler_note_dispatch_failure(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_current_window(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_base_window(
	const struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_min_window(
	const struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_max_window(
	const struct kfastblock_scheduler_controller *controller);
bool kfastblock_scheduler_dynamic_enabled(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_grow_events(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_shrink_events(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_retry_events(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_dispatch_failures(
	struct kfastblock_scheduler_controller *controller);

#endif
