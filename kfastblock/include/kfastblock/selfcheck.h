#ifndef KFASTBLOCK_SELFCHECK_H
#define KFASTBLOCK_SELFCHECK_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct kfastblock_volume;
struct seq_file;

enum kfastblock_selfcheck_flag {
	KFASTBLOCK_SELFCHECK_VOLUME_CORE = 1U << 0,
	KFASTBLOCK_SELFCHECK_META_VIEW = 1U << 1,
	KFASTBLOCK_SELFCHECK_SCHEDULER = 1U << 2,
	KFASTBLOCK_SELFCHECK_BUFFER_POOL = 1U << 3,
	KFASTBLOCK_SELFCHECK_OSD_CONN_POOL = 1U << 4,
	KFASTBLOCK_SELFCHECK_MON_CONN_POOL = 1U << 5,
	KFASTBLOCK_SELFCHECK_QUEUE_GATE = 1U << 6,
	KFASTBLOCK_SELFCHECK_FAULT_INJECTION = 1U << 7,
};

struct kfastblock_selfcheck_report {
	u32 total_checks;
	u32 failed_checks;
	u32 warning_checks;
	u32 flags;
	int result_errno;
};

struct kfastblock_selfcheck_state {
	spinlock_t lock;
	unsigned long last_run_jiffies;
	u32 run_count;
	u32 failure_runs;
	u32 warning_runs;
	s32 last_errno;
	u32 last_failed_checks;
	u32 last_warning_checks;
	u32 last_flags;
};

void kfastblock_selfcheck_state_init(
	struct kfastblock_selfcheck_state *state);
int kfastblock_selfcheck_run(struct kfastblock_volume *vol,
			     struct kfastblock_selfcheck_report *report,
			     struct seq_file *m);
u32 kfastblock_selfcheck_run_count(
	struct kfastblock_selfcheck_state *state);
u32 kfastblock_selfcheck_failure_runs(
	struct kfastblock_selfcheck_state *state);
u32 kfastblock_selfcheck_warning_runs(
	struct kfastblock_selfcheck_state *state);
s32 kfastblock_selfcheck_last_errno(
	struct kfastblock_selfcheck_state *state);
u32 kfastblock_selfcheck_last_failed_checks(
	struct kfastblock_selfcheck_state *state);
u32 kfastblock_selfcheck_last_warning_checks(
	struct kfastblock_selfcheck_state *state);
u32 kfastblock_selfcheck_last_flags(
	struct kfastblock_selfcheck_state *state);
unsigned long kfastblock_selfcheck_last_run_jiffies(
	struct kfastblock_selfcheck_state *state);

#endif
