#include "raft/raft_private.h"
#include "raft/raft_client_protocol.h"
#include "raft/pg_group.h"

SPDK_LOG_REGISTER_COMPONENT(client_proto)
void appendentries_source::process_response(){
    _raft->raft_process_appendentries_reply(&response);
}

void vote_source::process_response(){
    _raft->raft_process_requestvote_reply(&response);
}

void install_snapshot_source::process_response(){
    _raft->raft_process_installsnapshot_reply(&response);
}

void heartbeat_source::process_response(){
    auto beat_num = _request->heartbeats_size();
    for(int i = 0; i < beat_num; i++){
        const heartbeat_metadata& meta = _request->heartbeats(i);
        auto raft = _group->get_pg(_shard_id, meta.pool_id(), meta.pg_id());
        SPDK_DEBUGLOG(pg_group, "heartbeat response from node: %d pg %lu.%lu\n", meta.target_node_id(), meta.pool_id(), meta.pg_id());

        msg_appendentries_response_t *rsp = response.mutable_meta(i);
        auto node = raft->raft_get_node(rsp->node_id());
        node->raft_node_set_heartbeating(false);

        raft->raft_process_appendentries_reply(rsp);
    }
}