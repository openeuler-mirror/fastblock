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
#include "localstore/types.h"


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
        //随后在finish_func中调用了_cfg_complete->complete
        // if(ret != 0){
            // complete->complete(ret);
        // }
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
        //随后在finish_func中调用了_cfg_complete->complete
        // if(ret != 0){
            // complete->complete(ret);
        // }
        return ret;
    };

    switch (get_state()){
    case  cfg_state::CFG_CATCHING_UP:
    {
        if(!get_catch_up_node(node->raft_node_get_id()))
            return 0;
        SPDK_INFOLOG_EX(pg_group, "state: %d node_id: %d result: %d\n", (int)get_state(), node->raft_node_get_id(), result);
        if(result != 0){
            finish_func(result);
            return 1;
        }

        set_node_catch_up(node->raft_node_get_id());
        if(all_new_nodes_catch_up()){
            SPDK_INFOLOG_EX(pg_group, "all_new_nodes_catch_up\n");
            int ret = 0;
            if(get_change_num(_cfg_entry) > 1){
                //添加和删除节点之和大于1
                ret = process_cfg_change(cfg_state::CFG_JOINT, _cfg_entry, _cfg_complete);
            }else{
                //一次只添加1个节点
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
        SPDK_INFOLOG_EX(pg_group, "state: %d node_id: %d rsp_current_idx:%ld result: %d\n", (int)get_state(), node->raft_node_get_id(), rsp_current_idx, result);
        if((_new_node_match_size  > get_node_size() / 2)
                && (_old_node_match_size  > get_old_node_size() / 2)){
            SPDK_INFOLOG_EX(pg_group, "all nodes match\n");
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
        SPDK_INFOLOG_EX(pg_group, "state: %d node_id: %d rsp_current_idx: %ld result: %d\n", (int)get_state(), node->raft_node_get_id(), _configuration_index, result);
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
            SPDK_INFOLOG_EX(pg_group, "all nodes match\n");
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

void node_configuration_manager::truncate_by_idx(raft_index_t index){
    bool update = false;
    while(!_configurations.empty()){
        auto &cfg = _configurations.back();
        if(cfg.get_index() >= index){
            _configurations.pop_back();
            update = true;
        }else{
            break;
        }
    }
    if(update){
        auto &cfg = _configurations.back();
        save_node_configuration();
        _raft->update_nodes_stat(cfg);
    }
}

bool node_configuration_manager::save_node_configuration(){
    auto [res, value] = serialize();
    if(!res){
        SPDK_INFOLOG_EX(pg_group, "serialize node_configuration failed.\n");
        return false;
    }

    if(_raft->save_node_configuration(value) != 0){
        SPDK_INFOLOG_EX(pg_group, "save_node_configuration failed.\n");
        return false;
    }
    return true;        
}

bool node_configuration_manager::load_node_configuration(){
    auto val = _raft->load_node_configuration();
    if(!val.has_value()){
        SPDK_INFOLOG_EX(pg_group, "no find node configuration\n");
        return false;    
    }
    return  deserialize(val.value());     
}

std::pair<bool, std::string> node_configuration_manager::serialize(){
    char* buf = (char*)spdk_malloc(1_MB,
            0x1000, NULL, SPDK_ENV_LCORE_ID_ANY,
            SPDK_MALLOC_DMA);
    spdk_buffer sbuf = spdk_buffer(buf, 1_MB);
    auto size = _configurations.size();
    bool rc;
    auto hand_end = [this, buf, &sbuf](bool res){
        if(res){
            std::string date(sbuf.get_buf(), sbuf.used());
            return std::make_pair(res, std::move(date));
        }
        spdk_free(buf);
        return std::make_pair(res, std::string());
    };

    rc = PutFixed32(sbuf, size);
    if(!rc) return hand_end(false);
    auto iter = _configurations.begin();
    while(iter != _configurations.end()){
        auto index = iter->get_index();
        rc = PutFixed64(sbuf, index);
        if(!rc) return hand_end(false);
        auto term = iter->get_term();
        rc = PutFixed64(sbuf, term);
        if(!rc) return hand_end(false);

        auto &nodes = iter->get_nodes();
        rc = PutFixed32(sbuf, nodes.size());
        if(!rc) return hand_end(false);   
        for(auto& node : nodes){
            rc = PutFixed32(sbuf, node.node_id());
            if(!rc) return hand_end(false);   
            rc = PutString(sbuf, node.addr());
            if(!rc) return hand_end(false);    
            rc = PutFixed32(sbuf, node.port());
            if(!rc) return hand_end(false);                                  
        }         

        auto &old_nodes = iter->get_old_nodes();
        rc = PutFixed32(sbuf, old_nodes.size());
        if(!rc) return hand_end(false);     
        for(auto& old_node : old_nodes){
            rc = PutFixed32(sbuf, old_node.node_id());
            if(!rc) return hand_end(false);   
            rc = PutString(sbuf, old_node.addr());
            if(!rc) return hand_end(false);    
            rc = PutFixed32(sbuf, old_node.port());
            if(!rc) return hand_end(false);                     
        }                        
        iter++;
    }
    return hand_end(true);
}

bool node_configuration_manager::deserialize(std::string& data){
    char *date = const_cast<char*>(data.data());
    spdk_buffer sbuf(date, data.size());
    bool rc;
    auto hand_error = [this](bool res){
        if(!res)
            _configurations.clear();
        return res;
    };
    
    uint32_t cfg_size;
    rc = GetFixed32(sbuf, cfg_size);
    if(!rc) return hand_error(false);
    for(uint32_t i = 0; i < cfg_size; i++){
        node_configuration node_cfg;
        uint64_t index = 0;
        uint64_t  term = 0;

        rc = GetFixed64(sbuf, index);
        if(!rc) return hand_error(false);  
        rc = GetFixed64(sbuf, term);
        if(!rc) return hand_error(false); 
        node_cfg._index = index;
        node_cfg._term = term;
       
        uint32_t node_size = 0;
        uint32_t old_node_size = 0;
        uint32_t node_id;
        std::string addr;
        uint32_t port;
        rc = GetFixed32(sbuf, node_size);
        if(!rc) return hand_error(false);

        for(uint32_t j = 0; j < node_size; j++){
            raft_node_info node;
            rc = GetFixed32(sbuf, node_id);
            if(!rc) return hand_error(false);
            rc = GetString(sbuf, addr);
            if(!rc) return hand_error(false);
            rc = GetFixed32(sbuf, port);
            if(!rc) return hand_error(false);   
            node.set_node_id(node_id);
            node.set_addr(addr);
            node.set_port(port);
            node_cfg._nodes.emplace_back(std::move(node));              
        }

        rc = GetFixed32(sbuf, old_node_size);
        if(!rc) return hand_error(false); 
        for(uint32_t j = 0; j < old_node_size; j++){
            raft_node_info node;
            rc = GetFixed32(sbuf, node_id);
            if(!rc) return hand_error(false);
            rc = GetString(sbuf, addr);
            if(!rc) return hand_error(false);
            rc = GetFixed32(sbuf, port);
            if(!rc) return hand_error(false);   
            node.set_node_id(node_id);
            node.set_addr(addr);
            node.set_port(port);
            node_cfg._old_nodes.emplace_back(std::move(node));       
        } 

        _configurations.emplace_back(std::move(node_cfg));        
    }
    return true;
}