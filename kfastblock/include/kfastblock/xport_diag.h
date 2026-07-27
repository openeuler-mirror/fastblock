#ifndef KFASTBLOCK_XPORT_DIAG_H
#define KFASTBLOCK_XPORT_DIAG_H

#include <linux/types.h>

struct kfastblock_diag_xport_snapshot;
struct seq_file;

/*
 * Human-readable helpers for OSD data-plane transport diagnostics.
 * Used by diag dump / selfcheck so operators can see RDMA selection state.
 */

/* Format preference + readiness counters into @buf (NUL-terminated). */
void kfastblock_xport_diag_format_summary(
	const struct kfastblock_diag_xport_snapshot *xport,
	char *buf, size_t buf_len);

/* 0..100: share of valid leaders advertising rdma_port; 0 if none. */
u32 kfastblock_xport_diag_leader_rdma_pct(
	const struct kfastblock_diag_xport_snapshot *xport);

/* 0..100: share of shards with rdma_port; 0 if none. */
u32 kfastblock_xport_diag_shard_rdma_pct(
	const struct kfastblock_diag_xport_snapshot *xport);

/* True when preference wants RDMA but no leader has rdma_port. */
bool kfastblock_xport_diag_rdma_unavailable(
	const struct kfastblock_diag_xport_snapshot *xport);

/* Dump xport.* lines to seq_file (optional prefix, may be ""). */
void kfastblock_xport_diag_dump_seq(
	struct seq_file *m, const char *prefix,
	const struct kfastblock_diag_xport_snapshot *xport);

/* Append probe-cache hit/miss/valid lines (uses xport probe cache). */
void kfastblock_xport_diag_dump_probe_cache(struct seq_file *m,
					    const char *prefix);

/* Severity: 0=ok, 1=warn (partial RDMA), 2=error (prefer RDMA none ready). */
u32 kfastblock_xport_diag_severity(
	const struct kfastblock_diag_xport_snapshot *xport);

const char *kfastblock_xport_diag_severity_name(u32 severity);

#endif
