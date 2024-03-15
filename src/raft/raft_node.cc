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

#include "raft/raft_node.h"
#include "raft/configuration_manager.h"
#include "spdk/log.h"

void raft_nodes::update_with_node_configuration(node_configuration& cfg, 
        std::vector<std::shared_ptr<raft_node>> new_add_nodes){
    auto &node_infos = cfg.get_nodes();
    for(auto& node_info : node_infos){
        if(_nodes.contains(node_info.node_id()))
            continue;
        std::shared_ptr<raft_node> add_node = nullptr;
        for(auto new_add_node : new_add_nodes){
            if(new_add_node->raft_node_get_id() == node_info.node_id()){
                add_node = new_add_node;
                break;
            }
        }
        if(add_node){
            SPDK_INFOLOG(pg_group, "add node %d\n", node_info.node_id());
            _nodes.emplace(node_info.node_id(), add_node);
        }else{
            SPDK_INFOLOG(pg_group, "add node %d\n", node_info.node_id());
            auto node = std::make_shared<raft_node>(node_info);
            _nodes.emplace(node_info.node_id(), node);
        }
    }   

    absl::erase_if(_nodes, [&cfg](const nodes_type::value_type& p){
        bool no_found = true;
        for(auto node_id : cfg.get_nodes_id()){
            if(p.first == node_id){
                no_found = false;
                break;
            }
        }
        if(no_found)
            SPDK_INFOLOG(pg_group, "remove node %d\n", p.first);
        return no_found;
    });     
}

