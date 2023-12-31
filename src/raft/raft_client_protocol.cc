/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include "raft/raft.h"
#include "raft/raft_client_protocol.h"
#include "raft/pg_group.h"

void appendentries_source::process_response(){
    _raft->raft_process_appendentries_reply(&response);
    delete this;
}

void vote_source::process_response(){
    _raft->raft_process_requestvote_reply(&response);
    delete this;
}

void install_snapshot_source::process_response(){
    _raft->raft_process_installsnapshot_reply(&response);
    delete this;
}

void heartbeat_source::process_response(){
    auto beat_num = _request->heartbeats_size();
    for(int i = 0; i < beat_num; i++){
        const heartbeat_metadata& meta = _request->heartbeats(i);
        auto raft = _group->get_pg(_shard_id, meta.pool_id(), meta.pg_id());
        SPDK_DEBUGLOG(pg_group, "heartbeat response from node: %d pg %lu.%lu\n", meta.target_node_id(), meta.pool_id(), meta.pg_id());

        msg_appendentries_response_t *rsp = response.mutable_meta(i);
        auto node = raft->raft_get_node(rsp->node_id());
        // node->raft_node_set_heartbeating(false);

        raft->raft_process_appendentries_reply(rsp, true);
    }
    delete this;
}