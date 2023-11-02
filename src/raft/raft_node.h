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
/**
 * Copyright (c) 2013, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the raft/LICENSE file.
 */

#pragma once
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
    , _effective_time(0)
    , _suppress_heartbeats(false)
    , _append_time(0)
    , _is_heartbeating(false)
    , _is_recovering(false)
    , _end_idx(0) {}

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

    void raft_set_suppress_heartbeats(bool suppressed){
        _suppress_heartbeats = suppressed;
    }

    bool raft_get_suppress_heartbeats(){
        return _suppress_heartbeats;
    }

    void raft_set_append_time(raft_time_t append_time){
        _append_time = append_time;
    }

    raft_time_t raft_get_append_time(){
        return _append_time;
    }

    bool raft_node_is_heartbeating(){
        return _is_heartbeating;
    }

    void raft_node_set_heartbeating(bool is_heartbeating){
        _is_heartbeating = is_heartbeating;
    }

    bool raft_node_is_recovering(){
        return _is_recovering;
    }

    void raft_node_set_recovering(bool recovering){
        _is_recovering = recovering;
    }

    raft_index_t raft_get_end_idx(){
        return _end_idx;
    }

    void raft_set_end_idx(raft_index_t idx){
        _end_idx = idx;
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

    bool _suppress_heartbeats;
    raft_time_t _append_time;
    bool _is_heartbeating;
    bool _is_recovering;

    raft_index_t _end_idx;  //leader发给此node的append entry request中最后一个log
};