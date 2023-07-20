/**
 * Copyright (c) 2013, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * @file
 * @brief Representation of a peer
 * @author Willem Thiart himself@willemthiart.com
 * @version 0.1
 */
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