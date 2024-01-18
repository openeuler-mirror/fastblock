/* Copyright (c) 2023-2024 ChinaUnicom
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
#include "raft/raft_types.h"
// #include "raft/configuration_manager.h"
#include "rpc/raft_msg.pb.h"

#include <absl/container/node_hash_map.h>

#define RAFT_NODE_VOTED_FOR_ME        (1 << 0)

class raft_node
{
public:
    raft_node(raft_node_info node_info)
    : _next_idx(1)
    , _match_idx(0)
    , _flags(0)
    , _info(std::move(node_info))
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

    /** Get node's ID.
     * @return ID of node */
    raft_node_id_t raft_node_get_id()
    {
        return _info.node_id();
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

    const raft_node_info& raft_get_node_info(){
        return _info;
    }

private:

    raft_index_t _next_idx;
    raft_index_t _match_idx;

    int _flags;

    raft_node_info _info;

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

class node_configuration;

class raft_nodes{
    using nodes_type = absl::node_hash_map<raft_node_id_t, std::shared_ptr<raft_node>>;
public:
    raft_nodes(){}

    void update_with_node_configuration(node_configuration& cfg, 
            std::vector<std::shared_ptr<raft_node>> new_add_nodes = {});

    bool contains(raft_node_id_t node_id) const {
        return _nodes.find(node_id) != _nodes.end();
    }

    nodes_type::iterator find(raft_node_id_t node_id) { return _nodes.find(node_id); }
    nodes_type::const_iterator find(raft_node_id_t node_id) const { return _nodes.find(node_id); }

    nodes_type::iterator begin() { return _nodes.begin(); }
    nodes_type::iterator end() { return _nodes.end(); }
    nodes_type::const_iterator begin() const { return _nodes.begin(); }
    nodes_type::const_iterator end() const { return _nodes.end(); }

    size_t size() const { return _nodes.size(); }

    std::shared_ptr<raft_node> get_node(raft_node_id_t node_id){
        auto it = _nodes.find(node_id);
        if(it == _nodes.end())
            return nullptr;
        return it->second;
    }
private:
    nodes_type _nodes;
};