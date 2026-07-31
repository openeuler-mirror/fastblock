#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "kfastblock/diag.h"
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
