#ifndef KFASTBLOCK_FAULT_H
#define KFASTBLOCK_FAULT_H

#include <linux/spinlock.h>
#include <linux/types.h>

enum kfastblock_fault_site {
	KFASTBLOCK_FAULT_NONE = 0,
	KFASTBLOCK_FAULT_MONITOR_CONNECT = 1U << 0,
	KFASTBLOCK_FAULT_OSD_CONNECT = 1U << 1,
	KFASTBLOCK_FAULT_LEADER_QUERY = 1U << 2,
	KFASTBLOCK_FAULT_MONITOR_FETCH_IMAGE = 1U << 3,
	KFASTBLOCK_FAULT_MONITOR_FETCH_CLUSTER = 1U << 4,
	KFASTBLOCK_FAULT_OBJECT_IO = 1U << 5,
	/* RDMA data-plane: fail CM connect before pool acquire. */
	KFASTBLOCK_FAULT_RDMA_CONNECT = 1U << 6,
	/* RDMA data-plane: fail send/recv exchange (timeout-like). */
	KFASTBLOCK_FAULT_RDMA_EXCHANGE = 1U << 7,
	/* Force AUTO/RDMA selection onto TCP for fallback testing. */
	KFASTBLOCK_FAULT_FORCE_TCP = 1U << 8,
};

/* Mask of all defined fault sites (kept in sync with enum above). */
#define KFASTBLOCK_FAULT_ALL_MASK \
	(KFASTBLOCK_FAULT_MONITOR_CONNECT | \
	 KFASTBLOCK_FAULT_OSD_CONNECT | \
	 KFASTBLOCK_FAULT_LEADER_QUERY | \
	 KFASTBLOCK_FAULT_MONITOR_FETCH_IMAGE | \
	 KFASTBLOCK_FAULT_MONITOR_FETCH_CLUSTER | \
	 KFASTBLOCK_FAULT_OBJECT_IO | \
	 KFASTBLOCK_FAULT_RDMA_CONNECT | \
	 KFASTBLOCK_FAULT_RDMA_EXCHANGE | \
	 KFASTBLOCK_FAULT_FORCE_TCP)

/* RDMA-related sites used when testing AUTO fallback / reconnect. */
#define KFASTBLOCK_FAULT_RDMA_MASK \
	(KFASTBLOCK_FAULT_RDMA_CONNECT | \
	 KFASTBLOCK_FAULT_RDMA_EXCHANGE | \
	 KFASTBLOCK_FAULT_FORCE_TCP)

struct kfastblock_fault_injection_state {
	spinlock_t lock;
	u32 mask;
	s32 errno_value;
	u32 budget;
	u32 arm_count;
	u32 reset_count;
	u32 hit_count;
	u32 skip_count;
	unsigned long last_arm_jiffies;
	unsigned long last_reset_jiffies;
	unsigned long last_trigger_jiffies;
	u32 last_site;
	s32 last_errno;
	bool enabled;
};

void kfastblock_fault_injection_init(
	struct kfastblock_fault_injection_state *state);
int kfastblock_fault_parse_mask(const char *buf, u32 *mask_out);
void kfastblock_fault_format_mask(u32 mask, char *buf, size_t buf_len);
const char *kfastblock_fault_site_name(u32 site);
void kfastblock_fault_injection_set_mask(
	struct kfastblock_fault_injection_state *state,
	u32 mask);
void kfastblock_fault_injection_set_errno(
	struct kfastblock_fault_injection_state *state,
	s32 errno_value);
void kfastblock_fault_injection_set_budget(
	struct kfastblock_fault_injection_state *state,
	u32 budget);
void kfastblock_fault_injection_set_enabled(
	struct kfastblock_fault_injection_state *state,
	bool enabled);
void kfastblock_fault_injection_reset(
	struct kfastblock_fault_injection_state *state);
bool kfastblock_fault_injection_should_fail(
	struct kfastblock_fault_injection_state *state,
	u32 site,
	int *err_out);
u32 kfastblock_fault_injection_mask(
	struct kfastblock_fault_injection_state *state);
s32 kfastblock_fault_injection_errno(
	struct kfastblock_fault_injection_state *state);
u32 kfastblock_fault_injection_budget(
	struct kfastblock_fault_injection_state *state);
bool kfastblock_fault_injection_enabled(
	struct kfastblock_fault_injection_state *state);
u32 kfastblock_fault_injection_arm_count(
	struct kfastblock_fault_injection_state *state);
u32 kfastblock_fault_injection_reset_count(
	struct kfastblock_fault_injection_state *state);
u32 kfastblock_fault_injection_hit_count(
	struct kfastblock_fault_injection_state *state);
u32 kfastblock_fault_injection_skip_count(
	struct kfastblock_fault_injection_state *state);
unsigned long kfastblock_fault_injection_last_arm_jiffies(
	struct kfastblock_fault_injection_state *state);
unsigned long kfastblock_fault_injection_last_reset_jiffies(
	struct kfastblock_fault_injection_state *state);
unsigned long kfastblock_fault_injection_last_trigger_jiffies(
	struct kfastblock_fault_injection_state *state);
u32 kfastblock_fault_injection_last_site(
	struct kfastblock_fault_injection_state *state);
s32 kfastblock_fault_injection_last_errno(
	struct kfastblock_fault_injection_state *state);

/* True when @site is one of the RDMA data-plane injection points. */
static inline bool kfastblock_fault_site_is_rdma(u32 site)
{
	return (site & KFASTBLOCK_FAULT_RDMA_MASK) != 0;
}

/* True when force_tcp is armed for the current mask (single-bit check). */
static inline bool kfastblock_fault_site_is_force_tcp(u32 site)
{
	return site == KFASTBLOCK_FAULT_FORCE_TCP;
}

/*
 * Peek whether @site would fire without consuming budget. Used by hot
 * paths that need a side-effect-free preference override (force_tcp).
 */
bool kfastblock_fault_injection_armed(
	struct kfastblock_fault_injection_state *state,
	u32 site);

/* True when force_tcp is armed (enabled+mask+budget) without consuming. */
static inline bool kfastblock_fault_injection_force_tcp_armed(
	struct kfastblock_fault_injection_state *state)
{
	return kfastblock_fault_injection_armed(state,
						KFASTBLOCK_FAULT_FORCE_TCP);
}

/* True when any RDMA site is armed. */
static inline bool kfastblock_fault_injection_rdma_armed(
	struct kfastblock_fault_injection_state *state)
{
	if (!state)
		return false;
	return kfastblock_fault_injection_armed(state,
						KFASTBLOCK_FAULT_RDMA_CONNECT) ||
	       kfastblock_fault_injection_armed(state,
						KFASTBLOCK_FAULT_RDMA_EXCHANGE) ||
	       kfastblock_fault_injection_force_tcp_armed(state);
}

#endif
