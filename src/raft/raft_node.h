#ifndef RAFT_NODE_H_
#define RAFT_NODE_H_

#include "raft_types.h"

#define RAFT_NODE_VOTED_FOR_ME        (1 << 0)
#define RAFT_NODE_VOTING              (1 << 1)
#define RAFT_NODE_HAS_SUFFICIENT_LOG  (1 << 2)

class raft_node
{
public:
    raft_node(void* udata, raft_node_id_t id)
    : _udata(udata)
    , _next_idx(1)
    , _match_idx(0)
    , _flags(RAFT_NODE_VOTING)
    , _id(id)
    , _lease(0)
    , _effective_time(0){}

    void raft_node_set_effective_time(raft_time_t effective_time)
    {
        _effective_time = effective_time;
    }

    /** Turn a node into a voting node.
     * Voting nodes can take part in elections and in-regards to commiting entries,
     * are counted in majorities. */
    void raft_node_set_voting(int voting);

    /**
     * @return the node's next index */
    raft_index_t raft_node_get_next_idx()
    {
        return _next_idx;
    }

    void raft_node_set_next_idx(raft_index_t next_idx)
    {
        /* log index begins at 1 */
        _next_idx = next_idx < 1 ? 1 : next_idx;
    }

    /**
     * @return this node's user data */
    raft_index_t raft_node_get_match_idx()
    {
        return _match_idx;
    }

    void raft_node_set_match_idx(raft_index_t match_idx)
    {
        _match_idx = match_idx;
    }

    /**
     * @return this node's user data */
    void* raft_node_get_udata()
    {
        return _udata;
    }

    /**
     * Set this node's user data */
    void raft_node_set_udata(void* udata)
    {
        _udata = udata;
    }

    void raft_node_vote_for_me(const int vote)
    {
        if (vote)
            _flags |= RAFT_NODE_VOTED_FOR_ME;
        else
            _flags &= ~RAFT_NODE_VOTED_FOR_ME;
    }

    bool raft_node_has_vote_for_me()
    {
        return (_flags & RAFT_NODE_VOTED_FOR_ME) != 0;
    }

    /** Tell if a node is a voting node or not. */
    bool raft_node_is_voting()
    {
        return (_flags & RAFT_NODE_VOTING) != 0;
    }

    /** Check if a node has sufficient logs to be able to join the cluster. **/
    bool raft_node_has_sufficient_logs()
    {
        return (_flags & RAFT_NODE_HAS_SUFFICIENT_LOG) != 0;
    }

    void raft_node_set_has_sufficient_logs()
    {
        _flags |= RAFT_NODE_HAS_SUFFICIENT_LOG;
    }

    /** Get node's ID.
     * @return ID of node */
    raft_node_id_t raft_node_get_id()
    {
        return _id;
    }

    void raft_node_set_lease(raft_time_t lease)
    {
        if (_lease < lease)
            _lease = lease;
    }

    /**
     * @return this node's lease expiration time */
    raft_time_t raft_node_get_lease()
    {
        return _lease;
    }

    raft_time_t raft_node_get_effective_time()
    {
        return _effective_time;
    }
private:

    void* _udata;

    raft_index_t _next_idx;
    raft_index_t _match_idx;

    int _flags;

    raft_node_id_t _id;

    /* lease expiration time */
    raft_time_t _lease;
    /* time when this node becomes part of leader's configuration */
    raft_time_t _effective_time;
};

#endif