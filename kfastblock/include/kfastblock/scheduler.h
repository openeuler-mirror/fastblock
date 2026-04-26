#ifndef KFASTBLOCK_SCHEDULER_H
#define KFASTBLOCK_SCHEDULER_H

#include <linux/spinlock.h>
#include <linux/types.h>

enum kfastblock_scheduler_policy {
	KFASTBLOCK_SCHED_POLICY_STATIC = 0,
	KFASTBLOCK_SCHED_POLICY_ADAPTIVE = 1,
	KFASTBLOCK_SCHED_POLICY_PRESSURE = 2,
};

struct kfastblock_scheduler_sample {
	u32 inflight_ios;
	u32 request_objects;
	u32 base_window;
	u32 controller_window;
	u32 pressure_window;
	u32 effective_window;
	bool cooldown_active;
	u8 policy;
};

struct kfastblock_scheduler_controller {
	spinlock_t lock;
	u32 base_window;
	u32 current_window;
	u32 min_window;
	u32 max_window;
	u32 pressure_inflight_limit;
	u32 cooldown_ms;
	u32 consecutive_successes;
	u32 shrink_events;
	u32 grow_events;
	u32 retry_events;
	u32 dispatch_failures;
	u32 sample_events;
	u32 pressure_limited_samples;
	u32 cooldown_limited_samples;
	u32 cooldown_events;
	u32 last_sample_inflight_ios;
	u32 last_sample_request_objects;
	u32 last_sample_controller_window;
	u32 last_sample_pressure_window;
	u32 last_sample_effective_window;
	unsigned long cooldown_until_jiffies;
	u8 policy;
	bool dynamic_enabled;
};

const char *kfastblock_scheduler_policy_name(
	enum kfastblock_scheduler_policy policy);
void kfastblock_scheduler_init(struct kfastblock_scheduler_controller *controller,
			       u32 base_window,
			       u32 min_window,
			       u32 max_window);
void kfastblock_scheduler_set_base_window(
	struct kfastblock_scheduler_controller *controller,
	u32 base_window);
void kfastblock_scheduler_set_policy(
	struct kfastblock_scheduler_controller *controller,
	enum kfastblock_scheduler_policy policy);
void kfastblock_scheduler_set_pressure_inflight_limit(
	struct kfastblock_scheduler_controller *controller,
	u32 inflight_limit);
void kfastblock_scheduler_set_cooldown_ms(
	struct kfastblock_scheduler_controller *controller,
	u32 cooldown_ms);
void kfastblock_scheduler_set_dynamic_enabled(
	struct kfastblock_scheduler_controller *controller,
	bool enabled);
/*
 * Sample the current dispatch window for a newly initialized request,
 * optionally factoring in inflight pressure and temporary cooldown state.
 */
u32 kfastblock_scheduler_sample_window(
	struct kfastblock_scheduler_controller *controller,
	u32 inflight_ios,
	u32 request_objects,
	struct kfastblock_scheduler_sample *sample);
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
enum kfastblock_scheduler_policy kfastblock_scheduler_policy(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_pressure_inflight_limit(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_cooldown_ms(
	struct kfastblock_scheduler_controller *controller);
bool kfastblock_scheduler_cooldown_active(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_cooldown_remaining_ms(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_grow_events(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_shrink_events(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_retry_events(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_dispatch_failures(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_sample_events(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_pressure_limited_samples(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_cooldown_limited_samples(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_cooldown_events(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_last_sample_inflight_ios(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_last_sample_request_objects(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_last_sample_controller_window(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_last_sample_pressure_window(
	struct kfastblock_scheduler_controller *controller);
u32 kfastblock_scheduler_last_sample_effective_window(
	struct kfastblock_scheduler_controller *controller);

#endif
