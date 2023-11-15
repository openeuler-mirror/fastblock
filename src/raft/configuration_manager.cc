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

#include "raft/configuration_manager.h"
#include "raft/raft_node.h"
#include "raft/raft.h"


void node_configuration_manager::add_catch_up_node(const raft_node_info& node_info){
    auto node = std::make_shared<raft_node>(node_info);
    catch_up_node cnode(node);
    _catch_up_nodes.emplace(node_info.node_id(), std::move(cnode));
}

int node_configuration_manager::cfg_change_process(int result, raft_index_t rsp_current_idx, std::shared_ptr<raft_node> node){
    auto finish_func = [this](int result){
        if(_cfg_complete){
            _cfg_complete->complete(result);
        }
        reset_cfg_entry_complete();
        clear_catch_up_nodes();
        set_state(cfg_state::CFG_NONE);
        _configuration_index = 0;
        _old_node_match_size = 0;
        _old_node_fail_size = 0;
        _new_node_match_size = 0;
        _new_node_fail_size = 0;
    };

    auto get_change_num = [this](std::shared_ptr<raft_entry_t> entry){
        raft_configuration config;
        config.ParseFromString(entry->meta()); 

        std::map<int32_t, int32_t> new_nodes;
        auto new_node_size = config.new_nodes_size();
        for(int i = 0; i < new_node_size; i++){
            auto &node_info = config.new_nodes(i);
            new_nodes[node_info.node_id()] = 1;
        }

        auto &cfg = get_last_node_configuration();
        int add_size = get_catch_up_nodes_size();
        int remove_size = 0;

        auto old_node_ids = cfg.get_nodes_id();
        for(auto node_id : old_node_ids){
            if(new_nodes.find(node_id) == new_nodes.end()){
                remove_size++;
            }
        }

        return add_size + remove_size;
    };

    auto process_cfg_change = [this](
                            cfg_state next_state, 
                            std::shared_ptr<raft_entry_t> entry, 
                            utils::context* complete){
        raft_configuration config;
        config.ParseFromString(entry->meta()); 
        auto node_size = config.new_nodes_size();

        raft_configuration cfg;
        for(int i = 0; i < node_size; i++){
            auto info = cfg.add_new_nodes();
            *info = config.new_nodes(i);
        }

        if(next_state == cfg_state::CFG_JOINT){
            auto &old_nodes = get_last_node_configuration().get_nodes();
            for(uint64_t i = 0; i < old_nodes.size(); i++){
                auto info = cfg.add_old_nodes();
                *info = old_nodes[i];
            }
        }

        std::string buf;
        cfg.SerializeToString(&buf);

        auto entry_ptr = std::make_shared<raft_entry_t>();
        entry_ptr->set_type(RAFT_LOGTYPE_CONFIGURATION);
        entry_ptr->set_meta(std::move(buf));

        int ret = _raft->raft_write_entry(entry_ptr, nullptr);
        if(ret != 0){
            complete->complete(ret);
        }
        return ret;
    };

    auto create_cfg_entry = [this](utils::context* complete){
        raft_configuration cfg;

        auto &nodes = get_last_node_configuration().get_nodes();
        for(uint64_t i = 0; i < nodes.size(); i++){
            auto info = cfg.add_new_nodes();
            *info = nodes[i];
        }

        std::string buf;
        cfg.SerializeToString(&buf);

        auto entry_ptr = std::make_shared<raft_entry_t>();
        entry_ptr->set_type(RAFT_LOGTYPE_CONFIGURATION);
        entry_ptr->set_meta(std::move(buf));

        int ret = _raft->raft_write_entry(entry_ptr, nullptr);
        if(ret != 0){
            complete->complete(ret);
        }
        return ret;
    };

    switch (get_state()){
    case  cfg_state::CFG_CATCHING_UP:
    {
        if(!get_catch_up_node(node->raft_node_get_id()))
            return 0;
        SPDK_INFOLOG(pg_group, "state: %d node_id: %d result: %d\n", (int)get_state(), node->raft_node_get_id(), result);
        if(result != 0){
            finish_func(result);
            return 1;
        }

        set_node_catch_up(node->raft_node_get_id());
        if(all_new_nodes_catch_up()){
            SPDK_INFOLOG(pg_group, "all_new_nodes_catch_up\n");
            int ret = 0;
            if(get_change_num(_cfg_entry) > 1){
                ret = process_cfg_change(cfg_state::CFG_JOINT, _cfg_entry, _cfg_complete);
            }else{
                ret = process_cfg_change(cfg_state::CFG_UPDATE_NEW_CFG, _cfg_entry, _cfg_complete);
            }
            if(ret != 0){
                finish_func(result);
            }
            reset_cfg_entry();
        }
        break;
    }
    case cfg_state::CFG_JOINT:
    {
        if(result != 0){
            if(result == err::RAFT_ERR_NOT_LEADER || result == err::RAFT_ERR_PG_DELETED){
                finish_func(result);
                _raft->raft_flush();
                return 1;
            }
            if(find_node(node->raft_node_get_id()))
                _new_node_fail_size++;
            if(find_old_node(node->raft_node_get_id()))
                _old_node_fail_size++;
            if((_new_node_fail_size > (get_node_size() / 2))
                    || (_old_node_fail_size > (get_old_node_size() / 2))){
                finish_func(result);
                _raft->raft_flush();
                return 1;
            }
        }else{
            if(find_node(node->raft_node_get_id()))
                _new_node_match_size++;
            if(find_old_node(node->raft_node_get_id()))
                _old_node_match_size++;
        }
        SPDK_INFOLOG(pg_group, "state: %d node_id: %d rsp_current_idx:%ld result: %d\n", (int)get_state(), node->raft_node_get_id(), rsp_current_idx, result);
        if((_new_node_match_size  > get_node_size() / 2)
                && (_old_node_match_size  > get_old_node_size() / 2)){
            SPDK_INFOLOG(pg_group, "all nodes match\n");
            int ret = create_cfg_entry(_cfg_complete);
            if(ret != 0){
                finish_func(result);
            }else{
                set_state(cfg_state::CFG_UPDATE_NEW_CFG);
                _configuration_index = 0;
                _old_node_match_size = 0;
                _old_node_fail_size = 0;
                _new_node_match_size = 0;
                _new_node_fail_size = 0;
                _raft->raft_set_commit_idx(rsp_current_idx);
            }
            _raft->raft_flush();
        }
        break;
    }   
    case cfg_state::CFG_UPDATE_NEW_CFG:
    {
        if(_configuration_index == 0 || _configuration_index != rsp_current_idx)
            return 0;
        SPDK_INFOLOG(pg_group, "state: %d node_id: %d rsp_current_idx: %ld result: %d\n", (int)get_state(), node->raft_node_get_id(), _configuration_index, result);
        if(result != 0){
            if(result == err::RAFT_ERR_NOT_LEADER || result == err::RAFT_ERR_PG_DELETED){
                finish_func(result);
                _raft->raft_flush();
                return 1;
            }
            if(find_node(node->raft_node_get_id()))
                _new_node_fail_size++;
            if(_new_node_fail_size > (get_node_size() / 2)){
                finish_func(result);
                _raft->raft_flush();
                return 1;
            }
        }else{
            if(find_node(node->raft_node_get_id()))
                _new_node_match_size++;
        }       
        if(_new_node_match_size > get_node_size() / 2){
            SPDK_INFOLOG(pg_group, "all nodes match\n");
            //更新raft的_nodes_stat
            updatet_raft_nodes_stat();
            finish_func(0);
            _raft->raft_set_commit_idx(rsp_current_idx);

            if(!find_node(_raft->raft_get_nodeid())){
                //The leader node is not in the new membership list
                _raft->raft_step_down(rsp_current_idx);
            }
            _raft->raft_flush();            
        } 
        break;
    }
    default:
        break;
    }
    return 1;
}

void node_configuration_manager::updatet_raft_nodes_stat(){
    std::vector<std::shared_ptr<raft_node>> new_add_nodes;
    for(auto &[node_id, catchup_node] : _catch_up_nodes){
        new_add_nodes.emplace_back(catchup_node.node);
    }
    auto &cfg = get_last_node_configuration();
    _raft->update_nodes_stat(cfg, new_add_nodes);
}