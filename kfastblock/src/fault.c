#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "kfastblock/fault.h"

struct kfastblock_fault_site_desc {
	u32 mask;
	const char *name;
};

static const struct kfastblock_fault_site_desc kfastblock_fault_sites[] = {
	{ KFASTBLOCK_FAULT_MONITOR_CONNECT, "monitor_connect" },
	{ KFASTBLOCK_FAULT_OSD_CONNECT, "osd_connect" },
	{ KFASTBLOCK_FAULT_LEADER_QUERY, "leader_query" },
	{ KFASTBLOCK_FAULT_MONITOR_FETCH_IMAGE, "monitor_fetch_image" },
	{ KFASTBLOCK_FAULT_MONITOR_FETCH_CLUSTER, "monitor_fetch_cluster" },
	{ KFASTBLOCK_FAULT_OBJECT_IO, "object_io" },
};

static bool kfastblock_fault_token_matches(const char *token, const char *name)
{
	return token && name && !strcmp(token, name);
}

static u32 kfastblock_fault_mask_for_token(const char *token)
{
	u32 i;

	if (!token || !*token)
		return 0;
	if (!strcmp(token, "none"))
		return 0;
	if (!strcmp(token, "all"))
		return KFASTBLOCK_FAULT_MONITOR_CONNECT |
			KFASTBLOCK_FAULT_OSD_CONNECT |
			KFASTBLOCK_FAULT_LEADER_QUERY |
			KFASTBLOCK_FAULT_MONITOR_FETCH_IMAGE |
			KFASTBLOCK_FAULT_MONITOR_FETCH_CLUSTER |
			KFASTBLOCK_FAULT_OBJECT_IO;

	for (i = 0; i < ARRAY_SIZE(kfastblock_fault_sites); ++i) {
		if (kfastblock_fault_token_matches(token,
						   kfastblock_fault_sites[i].name))
			return kfastblock_fault_sites[i].mask;
	}

	return U32_MAX;
}

void kfastblock_fault_injection_init(
	struct kfastblock_fault_injection_state *state)
{
	if (!state)
		return;

	memset(state, 0, sizeof(*state));
	spin_lock_init(&state->lock);
	state->errno_value = -EIO;
}

int kfastblock_fault_parse_mask(const char *buf, u32 *mask_out)
{
	char tmp[128];
	char *cursor;
	char *token;
	u32 mask = 0;
	int ret;

	if (!buf || !mask_out)
		return -EINVAL;

	ret = strscpy(tmp, buf, sizeof(tmp));
	if (ret < 0)
		return ret;

	cursor = tmp;
	while ((token = strsep(&cursor, ",| \t\n")) != NULL) {
		u32 token_mask;

		if (!*token)
			continue;
		token_mask = kfastblock_fault_mask_for_token(token);
		if (token_mask == U32_MAX) {
			u32 numeric;

			ret = kstrtou32(token, 0, &numeric);
			if (ret)
				return -EINVAL;
			mask |= numeric;
			continue;
		}
		if (!strcmp(token, "none")) {
			mask = 0;
			continue;
		}
		if (!strcmp(token, "all")) {
			mask = token_mask;
			continue;
		}
		mask |= token_mask;
	}

	*mask_out = mask;
	return 0;
}

void kfastblock_fault_format_mask(u32 mask, char *buf, size_t buf_len)
{
	u32 i;
	size_t used = 0;

	if (!buf || !buf_len)
		return;

	buf[0] = '\0';
	if (!mask) {
		scnprintf(buf, buf_len, "none");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(kfastblock_fault_sites); ++i) {
		const struct kfastblock_fault_site_desc *desc =
			&kfastblock_fault_sites[i];

		if (!(mask & desc->mask))
			continue;
		used += scnprintf(buf + used, buf_len > used ? buf_len - used : 0,
				  "%s%s", used ? "," : "", desc->name);
		mask &= ~desc->mask;
	}

	if (mask && used < buf_len)
		scnprintf(buf + used, buf_len - used, "%s0x%x",
			  used ? "," : "", mask);
}

const char *kfastblock_fault_site_name(u32 site)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(kfastblock_fault_sites); ++i) {
		if (kfastblock_fault_sites[i].mask == site)
			return kfastblock_fault_sites[i].name;
	}

	return "unknown";
}

static void kfastblock_fault_note_arm_locked(
	struct kfastblock_fault_injection_state *state,
	bool enabled)
{
	if (!state)
		return;
	if (!enabled)
		return;

	state->arm_count++;
	state->last_arm_jiffies = jiffies;
}

void kfastblock_fault_injection_set_mask(
	struct kfastblock_fault_injection_state *state,
	u32 mask)
{
	unsigned long flags;

	if (!state)
		return;

	spin_lock_irqsave(&state->lock, flags);
	state->mask = mask;
	spin_unlock_irqrestore(&state->lock, flags);
}

void kfastblock_fault_injection_set_errno(
	struct kfastblock_fault_injection_state *state,
	s32 errno_value)
{
	unsigned long flags;

	if (!state)
		return;

	spin_lock_irqsave(&state->lock, flags);
	state->errno_value = errno_value ? errno_value : -EIO;
	spin_unlock_irqrestore(&state->lock, flags);
}

void kfastblock_fault_injection_set_budget(
	struct kfastblock_fault_injection_state *state,
	u32 budget)
{
	unsigned long flags;

	if (!state)
		return;

	spin_lock_irqsave(&state->lock, flags);
	state->budget = budget;
	spin_unlock_irqrestore(&state->lock, flags);
}

void kfastblock_fault_injection_set_enabled(
	struct kfastblock_fault_injection_state *state,
	bool enabled)
{
	unsigned long flags;

	if (!state)
		return;

	spin_lock_irqsave(&state->lock, flags);
	state->enabled = enabled;
	kfastblock_fault_note_arm_locked(state, enabled);
	spin_unlock_irqrestore(&state->lock, flags);
}

void kfastblock_fault_injection_reset(
	struct kfastblock_fault_injection_state *state)
{
	unsigned long flags;

	if (!state)
		return;

	spin_lock_irqsave(&state->lock, flags);
	state->enabled = false;
	state->mask = 0;
	state->budget = 0;
	state->last_site = 0;
	state->last_errno = 0;
	state->last_reset_jiffies = jiffies;
	state->reset_count++;
	spin_unlock_irqrestore(&state->lock, flags);
}

bool kfastblock_fault_injection_should_fail(
	struct kfastblock_fault_injection_state *state,
	u32 site,
	int *err_out)
{
	unsigned long flags;
	bool fail = false;
	int err = -EIO;

	if (err_out)
		*err_out = 0;
	if (!state || !site)
		return false;

	spin_lock_irqsave(&state->lock, flags);
	if (!state->enabled || !(state->mask & site)) {
		spin_unlock_irqrestore(&state->lock, flags);
		return false;
	}
	if (!state->budget) {
		state->skip_count++;
		spin_unlock_irqrestore(&state->lock, flags);
		return false;
	}

	state->budget--;
	state->hit_count++;
	state->last_site = site;
	state->last_errno = state->errno_value ? state->errno_value : -EIO;
	state->last_trigger_jiffies = jiffies;
	err = state->last_errno;
	fail = true;
	spin_unlock_irqrestore(&state->lock, flags);

	if (err_out)
		*err_out = err;
	return fail;
}

#define KFASTBLOCK_FAULT_GETTER(name, field, type) \
type name(struct kfastblock_fault_injection_state *state) \
{ \
	unsigned long flags; \
	type value = 0; \
	if (!state) \
		return 0; \
	spin_lock_irqsave(&state->lock, flags); \
	value = state->field; \
	spin_unlock_irqrestore(&state->lock, flags); \
	return value; \
}

KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_mask, mask, u32)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_errno, errno_value, s32)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_budget, budget, u32)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_enabled, enabled, bool)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_arm_count, arm_count, u32)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_reset_count, reset_count, u32)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_hit_count, hit_count, u32)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_skip_count, skip_count, u32)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_last_arm_jiffies,
			last_arm_jiffies, unsigned long)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_last_reset_jiffies,
			last_reset_jiffies, unsigned long)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_last_trigger_jiffies,
			last_trigger_jiffies, unsigned long)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_last_site, last_site, u32)
KFASTBLOCK_FAULT_GETTER(kfastblock_fault_injection_last_errno, last_errno, s32)
