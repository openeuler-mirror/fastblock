#include <assert.h>

#include "raft.h"

void raft_node::raft_node_set_voting(int voting)
{
    if (voting)
    {
        assert(!raft_node_is_voting());
        _flags |= RAFT_NODE_VOTING;
    }
    else
    {
        assert(raft_node_is_voting());
        _flags &= ~RAFT_NODE_VOTING;
    }
}