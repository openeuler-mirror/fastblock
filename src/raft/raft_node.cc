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

#include <assert.h>

#include "raft/configuration_manager.h"
#include "raft/raft_node.h"
#include "raft/pg_group.h"


void raft_nodes::update_with_node_configuration(node_configuration& cfg, 
        std::vector<std::shared_ptr<raft_node>> new_add_nodes){
    auto &node_infos = cfg.get_nodes();
    auto &new_node_infos = cfg.get_new_nodes();

    nodes_type old_nodes = std::move(_nodes);
    nodes_type old_new_nodes = std::move(_new_nodes);
    _nodes.clear();
    _new_nodes.clear();

    auto get_raft_node = [&old_nodes, &new_add_nodes](const raft_node_info& node_info) {
        std::shared_ptr<raft_node> node;
        auto it = old_nodes.find(node_info.node_id());
        if (it != old_nodes.end()) {
            node = it->second;
        } else {
            bool is_find = false;
            for (auto& new_add_node : new_add_nodes) {
                if (new_add_node->raft_node_get_id() == node_info.node_id()) {
                    node = new_add_node;
                    is_find = true;
                    break;
                }
            }
            if (!is_find) {
                SPDK_DEBUGLOG(pg_group,
                              "no find node %d in nodes and new_add_nodes\n",
                              node_info.node_id());
                node = std::make_shared<raft_node>(node_info);
            }
        }
        return node;
    };

    for (auto& node_info : node_infos) {
        auto add_node = get_raft_node(node_info);
        _nodes.emplace(node_info.node_id(), std::move(add_node));
        SPDK_DEBUGLOG(pg_group, "_nodes: osd id %d\n", node_info.node_id());
    }

    for (auto& new_node_info : new_node_infos) {
        auto add_new_node = get_raft_node(new_node_info);
        _new_nodes.emplace(new_node_info.node_id(), std::move(add_new_node));
        SPDK_DEBUGLOG(pg_group, "_new_nodes: osd id %d\n",
                      new_node_info.node_id());
    }
    (void)old_new_nodes;
}

