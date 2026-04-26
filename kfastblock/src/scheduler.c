#include <linux/jiffies.h>
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

const char *kfastblock_scheduler_policy_name(
	enum kfastblock_scheduler_policy policy)
{
	switch (policy) {
	case KFASTBLOCK_SCHED_POLICY_STATIC:
		return "static";
	case KFASTBLOCK_SCHED_POLICY_ADAPTIVE:
		return "adaptive";
	case KFASTBLOCK_SCHED_POLICY_PRESSURE:
		return "pressure";
	default:
		return "unknown";
	}
}

static enum kfastblock_scheduler_policy kfastblock_scheduler_clamp_policy(
	enum kfastblock_scheduler_policy policy)
{
	switch (policy) {
	case KFASTBLOCK_SCHED_POLICY_STATIC:
	case KFASTBLOCK_SCHED_POLICY_ADAPTIVE:
	case KFASTBLOCK_SCHED_POLICY_PRESSURE:
		return policy;
	default:
		return KFASTBLOCK_SCHED_POLICY_ADAPTIVE;
	}
}

static bool kfastblock_scheduler_is_cooldown_active_locked(
	const struct kfastblock_scheduler_controller *controller)
{
	return controller && controller->cooldown_ms &&
	       time_before(jiffies, controller->cooldown_until_jiffies);
}

static void kfastblock_scheduler_activate_cooldown_locked(
	struct kfastblock_scheduler_controller *controller)
{
	if (!controller || !controller->cooldown_ms ||
	    controller->policy != KFASTBLOCK_SCHED_POLICY_PRESSURE ||
	    !controller->dynamic_enabled)
		return;

	controller->cooldown_until_jiffies =
		jiffies + msecs_to_jiffies(controller->cooldown_ms);
	controller->cooldown_events++;
}

static u32 kfastblock_scheduler_limit_for_pressure_locked(
	const struct kfastblock_scheduler_controller *controller,
	u32 window,
	u32 inflight_ios,
	bool cooldown_active,
	bool *pressure_limited,
	bool *cooldown_limited)
{
	u32 limited;

	if (!controller)
		return 1;

	limited = kfastblock_scheduler_clamp_window(window ? window : 1,
						    controller->min_window,
						    controller->max_window);
	if (controller->pressure_inflight_limit &&
	    inflight_ios > controller->pressure_inflight_limit) {
		u32 over = inflight_ios - controller->pressure_inflight_limit;
		u32 reduced = over >= limited ?
			controller->min_window :
			max_t(u32, limited - over, controller->min_window);

		if (reduced < limited) {
			limited = reduced;
			if (pressure_limited)
				*pressure_limited = true;
		}
	}

	if (cooldown_active && limited > controller->min_window) {
		u32 cooled = max_t(u32, limited / 2, controller->min_window);

		if (cooled < limited) {
			limited = cooled;
			if (cooldown_limited)
				*cooldown_limited = true;
		}
	}

	return kfastblock_scheduler_clamp_window(limited,
						 controller->min_window,
						 controller->max_window);
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
	controller->pressure_inflight_limit = max_t(u32,
		controller->base_window * 2, controller->min_window);
	controller->cooldown_ms = 250;
	controller->consecutive_successes = 0;
	controller->shrink_events = 0;
	controller->grow_events = 0;
	controller->retry_events = 0;
	controller->dispatch_failures = 0;
	controller->sample_events = 0;
	controller->pressure_limited_samples = 0;
	controller->cooldown_limited_samples = 0;
	controller->cooldown_events = 0;
	controller->last_sample_inflight_ios = 0;
	controller->last_sample_request_objects = 0;
	controller->last_sample_controller_window = controller->base_window;
	controller->last_sample_pressure_window = controller->base_window;
	controller->last_sample_effective_window = controller->base_window;
	controller->cooldown_until_jiffies = 0;
	controller->policy = KFASTBLOCK_SCHED_POLICY_ADAPTIVE;
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

void kfastblock_scheduler_set_policy(
	struct kfastblock_scheduler_controller *controller,
	enum kfastblock_scheduler_policy policy)
{
	unsigned long flags;

	if (!controller)
		return;

	spin_lock_irqsave(&controller->lock, flags);
	controller->policy = kfastblock_scheduler_clamp_policy(policy);
	if (controller->policy == KFASTBLOCK_SCHED_POLICY_STATIC) {
		controller->current_window = controller->base_window;
		controller->consecutive_successes = 0;
		controller->cooldown_until_jiffies = 0;
	}
	spin_unlock_irqrestore(&controller->lock, flags);
}

void kfastblock_scheduler_set_pressure_inflight_limit(
	struct kfastblock_scheduler_controller *controller,
	u32 inflight_limit)
{
	unsigned long flags;

	if (!controller)
		return;

	spin_lock_irqsave(&controller->lock, flags);
	controller->pressure_inflight_limit = inflight_limit;
	spin_unlock_irqrestore(&controller->lock, flags);
}

void kfastblock_scheduler_set_cooldown_ms(
	struct kfastblock_scheduler_controller *controller,
	u32 cooldown_ms)
{
	unsigned long flags;

	if (!controller)
		return;

	spin_lock_irqsave(&controller->lock, flags);
	controller->cooldown_ms = cooldown_ms;
	if (!cooldown_ms)
		controller->cooldown_until_jiffies = 0;
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
		controller->cooldown_until_jiffies = 0;
	}
	spin_unlock_irqrestore(&controller->lock, flags);
}

u32 kfastblock_scheduler_sample_window(
	struct kfastblock_scheduler_controller *controller,
	u32 inflight_ios,
	u32 request_objects,
	struct kfastblock_scheduler_sample *sample)
{
	unsigned long flags;
	enum kfastblock_scheduler_policy policy;
	u32 window;
	u32 pressure_window;
	u32 effective_window;
	bool cooldown_active = false;
	bool pressure_limited = false;
	bool cooldown_limited = false;

	if (!controller)
		return 1;

	spin_lock_irqsave(&controller->lock, flags);
	window = controller->dynamic_enabled ?
		controller->current_window : controller->base_window;
	policy = controller->dynamic_enabled ?
		kfastblock_scheduler_clamp_policy(controller->policy) :
		KFASTBLOCK_SCHED_POLICY_STATIC;
	pressure_window = window;
	if (policy == KFASTBLOCK_SCHED_POLICY_PRESSURE) {
		cooldown_active =
			kfastblock_scheduler_is_cooldown_active_locked(controller);
		pressure_window = kfastblock_scheduler_limit_for_pressure_locked(
			controller, window, inflight_ios, cooldown_active,
			&pressure_limited, &cooldown_limited);
	}
	effective_window = pressure_window ? pressure_window : 1;
	if (request_objects)
		effective_window = min(effective_window, request_objects);
	effective_window = kfastblock_scheduler_clamp_window(
		effective_window, controller->min_window, controller->max_window);
	controller->sample_events++;
	if (pressure_limited)
		controller->pressure_limited_samples++;
	if (cooldown_limited)
		controller->cooldown_limited_samples++;
	controller->last_sample_inflight_ios = inflight_ios;
	controller->last_sample_request_objects = request_objects;
	controller->last_sample_controller_window = window;
	controller->last_sample_pressure_window = pressure_window;
	controller->last_sample_effective_window = effective_window;
	if (sample) {
		sample->inflight_ios = inflight_ios;
		sample->request_objects = request_objects;
		sample->base_window = controller->base_window;
		sample->controller_window = window;
		sample->pressure_window = pressure_window;
		sample->effective_window = effective_window;
		sample->cooldown_active = cooldown_active;
		sample->policy = policy;
	}
	spin_unlock_irqrestore(&controller->lock, flags);
	return effective_window ? effective_window : 1;
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
	    controller->policy == KFASTBLOCK_SCHED_POLICY_STATIC ||
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
	kfastblock_scheduler_activate_cooldown_locked(controller);
	if (!controller->dynamic_enabled ||
	    controller->policy == KFASTBLOCK_SCHED_POLICY_STATIC ||
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
	kfastblock_scheduler_activate_cooldown_locked(controller);
	if (!controller->dynamic_enabled ||
	    controller->policy == KFASTBLOCK_SCHED_POLICY_STATIC ||
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

enum kfastblock_scheduler_policy kfastblock_scheduler_policy(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	enum kfastblock_scheduler_policy policy;

	if (!controller)
		return KFASTBLOCK_SCHED_POLICY_ADAPTIVE;

	spin_lock_irqsave(&controller->lock, flags);
	policy = kfastblock_scheduler_clamp_policy(controller->policy);
	spin_unlock_irqrestore(&controller->lock, flags);
	return policy;
}

u32 kfastblock_scheduler_pressure_inflight_limit(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 limit;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	limit = controller->pressure_inflight_limit;
	spin_unlock_irqrestore(&controller->lock, flags);
	return limit;
}

u32 kfastblock_scheduler_cooldown_ms(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 cooldown_ms;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	cooldown_ms = controller->cooldown_ms;
	spin_unlock_irqrestore(&controller->lock, flags);
	return cooldown_ms;
}

bool kfastblock_scheduler_cooldown_active(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	bool active;

	if (!controller)
		return false;

	spin_lock_irqsave(&controller->lock, flags);
	active = kfastblock_scheduler_is_cooldown_active_locked(controller);
	spin_unlock_irqrestore(&controller->lock, flags);
	return active;
}

u32 kfastblock_scheduler_cooldown_remaining_ms(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 remaining_ms = 0;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	if (kfastblock_scheduler_is_cooldown_active_locked(controller))
		remaining_ms = jiffies_to_msecs(
			controller->cooldown_until_jiffies - jiffies);
	spin_unlock_irqrestore(&controller->lock, flags);
	return remaining_ms;
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

u32 kfastblock_scheduler_sample_events(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->sample_events;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_pressure_limited_samples(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->pressure_limited_samples;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_cooldown_limited_samples(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->cooldown_limited_samples;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_cooldown_events(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->cooldown_events;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_last_sample_inflight_ios(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->last_sample_inflight_ios;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_last_sample_request_objects(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->last_sample_request_objects;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_last_sample_controller_window(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->last_sample_controller_window;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_last_sample_pressure_window(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->last_sample_pressure_window;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}

u32 kfastblock_scheduler_last_sample_effective_window(
	struct kfastblock_scheduler_controller *controller)
{
	unsigned long flags;
	u32 value;

	if (!controller)
		return 0;

	spin_lock_irqsave(&controller->lock, flags);
	value = controller->last_sample_effective_window;
	spin_unlock_irqrestore(&controller->lock, flags);
	return value;
}
