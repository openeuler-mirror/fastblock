#include <linux/kernel.h>

#include "kfastblock/request.h"
#include "kfastblock/scheduler.h"

static u32 kfastblock_scheduler_clamp_window(u32 window, u32 min_window,
					     u32 max_window)
{
	if (!min_window)
		min_window = 1;
	if (!max_window)
		max_window = min_window;
	if (max_window < min_window)
		max_window = min_window;
	if (window < min_window)
		return min_window;
	if (window > max_window)
		return max_window;
	return window;
}

void kfastblock_scheduler_init(struct kfastblock_scheduler_controller *controller,
			       u32 base_window,
			       u32 min_window,
			       u32 max_window)
{
	if (!controller)
		return;

	spin_lock_init(&controller->lock);
	controller->min_window = min_window ? min_window : 1;
	controller->max_window = max_window ? max_window : controller->min_window;
	controller->base_window = kfastblock_scheduler_clamp_window(
		base_window ? base_window : controller->min_window,
		controller->min_window, controller->max_window);
	controller->current_window = controller->base_window;
	controller->consecutive_successes = 0;
	controller->shrink_events = 0;
	controller->grow_events = 0;
	controller->retry_events = 0;
	controller->dispatch_failures = 0;
	controller->dynamic_enabled = true;
}

void kfastblock_scheduler_set_base_window(
	struct kfastblock_scheduler_controller *controller,
	u32 base_window)
{
	unsigned long flags;

	if (!controller)
		return;

	spin_lock_irqsave(&controller->lock, flags);
	controller->base_window = kfastblock_scheduler_clamp_window(
		base_window ? base_window : controller->min_window,
		controller->min_window, controller->max_window);
	controller->current_window = kfastblock_scheduler_clamp_window(
		controller->current_window, controller->min_window,
		controller->base_window);
	controller->consecutive_successes = 0;
	spin_unlock_irqrestore(&controller->lock, flags);
}

void kfastblock_scheduler_set_dynamic_enabled(
	struct kfastblock_scheduler_controller *controller,
	bool enabled)
{
	unsigned long flags;

	if (!controller)
		return;

	spin_lock_irqsave(&controller->lock, flags);
	controller->dynamic_enabled = enabled;
	if (!enabled) {
		controller->current_window = controller->base_window;
		controller->consecutive_successes = 0;
	}
	spin_unlock_irqrestore(&controller->lock, flags);
}

u32 kfastblock_scheduler_snapshot_window(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 window;

	if (!controller)
		return 1;

	spin_lock_irqsave(&controller->lock, flags);
	window = controller->dynamic_enabled ?
		controller->current_window : controller->base_window;
	spin_unlock_irqrestore(&controller->lock, flags);
	return window ? window : 1;
}

void kfastblock_scheduler_note_success(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 grow_threshold;

	if (!controller)
		return;

	spin_lock_irqsave(&controller->lock, flags);
	if (!controller->dynamic_enabled ||
	    controller->current_window >= controller->base_window) {
		controller->consecutive_successes = 0;
		spin_unlock_irqrestore(&controller->lock, flags);
		return;
	}

	controller->consecutive_successes++;
	grow_threshold = max_t(u32, controller->current_window, 1);
	if (controller->consecutive_successes >= grow_threshold) {
		controller->current_window = min(controller->current_window + 1,
						 controller->base_window);
		controller->grow_events++;
		controller->consecutive_successes = 0;
	}
	spin_unlock_irqrestore(&controller->lock, flags);
}

void kfastblock_scheduler_note_retry(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 next_window;

	if (!controller)
		return;

	spin_lock_irqsave(&controller->lock, flags);
	controller->retry_events++;
	controller->consecutive_successes = 0;
	if (!controller->dynamic_enabled ||
	    controller->current_window <= controller->min_window) {
		spin_unlock_irqrestore(&controller->lock, flags);
		return;
	}

	next_window = max_t(u32, controller->current_window / 2,
			    controller->min_window);
	if (next_window < controller->current_window) {
		controller->current_window = next_window;
		controller->shrink_events++;
	}
	spin_unlock_irqrestore(&controller->lock, flags);
}

void kfastblock_scheduler_note_dispatch_failure(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 next_window;

	if (!controller)
		return;

	spin_lock_irqsave(&controller->lock, flags);
	controller->dispatch_failures++;
	controller->consecutive_successes = 0;
	if (!controller->dynamic_enabled ||
	    controller->current_window <= controller->min_window) {
		spin_unlock_irqrestore(&controller->lock, flags);
		return;
	}

	next_window = max_t(u32, controller->current_window / 2,
			    controller->min_window);
	if (next_window < controller->current_window) {
		controller->current_window = next_window;
		controller->shrink_events++;
	}
	spin_unlock_irqrestore(&controller->lock, flags);
}

u32 kfastblock_scheduler_current_window(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 window;

	if (!controller)
		return 1;

	spin_lock_irqsave(&controller->lock, flags);
	window = controller->current_window;
	spin_unlock_irqrestore(&controller->lock, flags);
	return window ? window : 1;
}

u32 kfastblock_scheduler_base_window(
	const struct kfastblock_scheduler_controller *controller)
{
	if (!controller)
		return 1;
	return controller->base_window ? controller->base_window : 1;
}

u32 kfastblock_scheduler_min_window(
	const struct kfastblock_scheduler_controller *controller)
{
	if (!controller)
		return 1;
	return controller->min_window ? controller->min_window : 1;
}

u32 kfastblock_scheduler_max_window(
	const struct kfastblock_scheduler_controller *controller)
{
	if (!controller)
		return 1;
	return controller->max_window ? controller->max_window : 1;
}

bool kfastblock_scheduler_dynamic_enabled(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	bool enabled;

	if (!controller)
		return false;

	spin_lock_irqsave(&controller->lock, flags);
	enabled = controller->dynamic_enabled;
	spin_unlock_irqrestore(&controller->lock, flags);
	return enabled;
}

u32 kfastblock_scheduler_grow_events(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->grow_events;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_shrink_events(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->shrink_events;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_retry_events(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->retry_events;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_dispatch_failures(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->dispatch_failures;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}
