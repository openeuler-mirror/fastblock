#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "kfastblock/diag.h"
#include "kfastblock/xport.h"
#include "kfastblock/xport_diag.h"

void kfastblock_xport_diag_format_summary(
	const struct kfastblock_diag_xport_snapshot *xport,
	char *buf, size_t buf_len)
{
	if (!buf || !buf_len)
		return;
	if (!xport) {
		buf[0] = '\0';
		return;
	}

	scnprintf(buf, buf_len,
		  "pref=%s prefers_rdma=%u leaders=%u rdma_ready=%u tcp_only=%u shards=%u shard_rdma=%u osd_rdma=%u",
		  xport->preference_name[0] ? xport->preference_name : "?",
		  xport->prefers_rdma,
		  xport->leader_valid_count,
		  xport->leader_rdma_ready_count,
		  xport->leader_tcp_only_count,
		  xport->shard_count,
		  xport->shard_rdma_port_count,
		  xport->osd_with_rdma_count);
}

u32 kfastblock_xport_diag_leader_rdma_pct(
	const struct kfastblock_diag_xport_snapshot *xport)
{
	if (!xport || !xport->leader_valid_count)
		return 0;
	return (xport->leader_rdma_ready_count * 100U) /
	       xport->leader_valid_count;
}

u32 kfastblock_xport_diag_shard_rdma_pct(
	const struct kfastblock_diag_xport_snapshot *xport)
{
	if (!xport || !xport->shard_count)
		return 0;
	return (xport->shard_rdma_port_count * 100U) / xport->shard_count;
}

bool kfastblock_xport_diag_rdma_unavailable(
	const struct kfastblock_diag_xport_snapshot *xport)
{
	if (!xport)
		return false;
	return xport->prefers_rdma &&
	       xport->leader_valid_count > 0 &&
	       xport->leader_rdma_ready_count == 0;
}

void kfastblock_xport_diag_dump_seq(
	struct seq_file *m, const char *prefix,
	const struct kfastblock_diag_xport_snapshot *xport)
{
	char summary[256];

	if (!m || !xport)
		return;
	if (!prefix)
		prefix = "";

	seq_printf(m, "%sxport.preference=%u\n", prefix, xport->preference);
	seq_printf(m, "%sxport.preference_name=%s\n", prefix,
		   xport->preference_name);
	seq_printf(m, "%sxport.prefers_rdma=%u\n", prefix, xport->prefers_rdma);
	seq_printf(m, "%sxport.leader_valid_count=%u\n", prefix,
		   xport->leader_valid_count);
	seq_printf(m, "%sxport.leader_rdma_ready_count=%u\n", prefix,
		   xport->leader_rdma_ready_count);
	seq_printf(m, "%sxport.leader_tcp_only_count=%u\n", prefix,
		   xport->leader_tcp_only_count);
	seq_printf(m, "%sxport.leader_rdma_pct=%u\n", prefix,
		   kfastblock_xport_diag_leader_rdma_pct(xport));
	seq_printf(m, "%sxport.shard_count=%u\n", prefix, xport->shard_count);
	seq_printf(m, "%sxport.shard_rdma_port_count=%u\n", prefix,
		   xport->shard_rdma_port_count);
	seq_printf(m, "%sxport.shard_rdma_pct=%u\n", prefix,
		   kfastblock_xport_diag_shard_rdma_pct(xport));
	seq_printf(m, "%sxport.osd_with_rdma_count=%u\n", prefix,
		   xport->osd_with_rdma_count);
	seq_printf(m, "%sxport.rdma_unavailable=%u\n", prefix,
		   kfastblock_xport_diag_rdma_unavailable(xport) ? 1 : 0);
	kfastblock_xport_diag_format_summary(xport, summary, sizeof(summary));
	seq_printf(m, "%sxport.summary=%s\n", prefix, summary);
}

void kfastblock_xport_diag_dump_probe_cache(struct seq_file *m,
					    const char *prefix)
{
	if (!m)
		return;
	if (!prefix)
		prefix = "";
	seq_printf(m, "%sxport.probe_cache_hits=%llu\n", prefix,
		   (unsigned long long)kfastblock_xport_probe_cache_hits());
	seq_printf(m, "%sxport.probe_cache_misses=%llu\n", prefix,
		   (unsigned long long)kfastblock_xport_probe_cache_misses());
	seq_printf(m, "%sxport.probe_cache_valid=%u\n", prefix,
		   kfastblock_xport_probe_cache_valid_count());
}
