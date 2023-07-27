#include "raft/raft_private.h"

void appendentries_source::process_response(){
    _raft->raft_process_appendentries_reply(&response);
}

void vote_source::process_response(){
    _raft->raft_process_requestvote_reply(&response);
}

void install_snapshot_source::process_response(){
    _raft->raft_process_installsnapshot_reply(&response);
}