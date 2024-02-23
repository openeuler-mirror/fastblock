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
#pragma once

#include <deque>
#include <vector>
#include<memory>
#include <assert.h>
#include <absl/container/node_hash_map.h>

#include "raft/raft_types.h"
#include "rpc/raft_msg.pb.h"
#include "raft/raft_node.h"
#include "utils/utils.h"

class raft_server_t;

class node_configuration{
public:
    node_configuration()
    : _index(0)
    , _term(0) {}

    node_configuration(raft_index_t index, raft_term_t term, 
            std::vector<raft_node_info> nodes, std::vector<raft_node_info> old_nodes)
    : _index(index)
    , _term(term)
    , _nodes(std::move(nodes))
    , _old_nodes(std::move(old_nodes)) {}
   
    void add_node(raft_node_id_t node_id, std::string& address, int port){
        raft_node_info node;
        node.set_node_id(node_id);
        node.set_addr(address);
        node.set_port(port);
        _nodes.emplace_back(std::move(node));
    }

    bool find_node(raft_node_id_t node_id){
        for(auto &node: _nodes){
            if(node.node_id() == node_id)
                return true;
        }
        return false;
    }

    bool find_old_node(raft_node_id_t node_id){
        for(auto &node: _old_nodes){
            if(node.node_id() == node_id)
                return true;
        }
        return false;        
    }

    int get_node_size(){
        return _nodes.size();
    }

    int get_old_node_size(){
        return _old_nodes.size();
    }

    std::vector<raft_node_id_t> get_nodes_id(){
        std::vector<raft_node_id_t> nodes_id;
        for(auto& node : _nodes){
            nodes_id.push_back(node.node_id());
        }
        return std::move(nodes_id);
    }

    const std::vector<raft_node_info>& get_nodes(){
        return _nodes;
    }

private:
    raft_index_t _index;
    raft_term_t  _term;
    std::vector<raft_node_info>   _nodes;
    std::vector<raft_node_info>   _old_nodes; 
};

enum class cfg_state {
    /*  此阶段表示没有成员变更 */
    CFG_NONE = 0,
    /* 成员变更开始阶段 */
    CFG_CATCHING_START,
    /* 成员变更追赶阶段，只有添加成员（包括替换成员里的添加成员）时，才会进入这个阶段，类似于单步成员变更 */
    CFG_CATCHING_UP,
    /* 联合一致阶段 */
    CFG_JOINT,
    /* 
     * 更新配置阶段，把新的成员配置表发送给新成员配置表里的所有成员，达到多数派后，变更结束。
     * */
    CFG_UPDATE_NEW_CFG
};

class node_configuration_manager{
public:
    node_configuration_manager(raft_server_t* raft)
    : _raft(raft) 
    , _state(cfg_state::CFG_NONE)
    , _cfg_entry(nullptr)
    , _cfg_complete(nullptr)
    , _configuration_index(0)
    , _old_node_match_size(0)
    , _old_node_fail_size(0)
    , _new_node_match_size(0)
    , _new_node_fail_size(0) {}

    void add_node_configuration(node_configuration&& node_config){
        _configurations.emplace_back(std::move(node_config));
    }

    node_configuration& get_last_node_configuration(){
        assert(!_configurations.empty());
        return _configurations.back();
    }

    void set_state(cfg_state state){
        _state = state;
    }

    cfg_state get_state(){
        return _state;
    }

    bool is_busy(){
        return _state != cfg_state::CFG_NONE;
    }

    bool cfg_change_is_in_progress(){
        return (_state == cfg_state::CFG_CATCHING_UP
                || _state == cfg_state::CFG_JOINT
                || _state == cfg_state::CFG_UPDATE_NEW_CFG);
    }

    void set_cfg_entry_complete(std::shared_ptr<raft_entry_t> cfg_entry, utils::context* cfg_complete){
        _cfg_entry = cfg_entry;
        _cfg_complete = cfg_complete;
    }

    void reset_cfg_entry_complete(){
        _cfg_entry = nullptr;
        _cfg_complete = nullptr;        
    }

    void reset_cfg_entry(){
        _cfg_entry = nullptr;        
    }

    void set_cfg_complete(utils::context* cfg_complete){
        _cfg_complete = cfg_complete;
    }

    auto get_cfg_entry_complete(){
        return std::make_pair(_cfg_entry, _cfg_complete);
    }

    int cfg_change_process(int result, raft_index_t rsp_current_idx, std::shared_ptr<raft_node> node);

    void add_catch_up_node(const raft_node_info& node_info);
        
    template<typename _Function>
    void for_catch_up_node(_Function fun){
        for(auto &[node_id, cnode] : _catch_up_nodes){
            fun(cnode.node);
        }
    }

    std::shared_ptr<raft_node> get_catch_up_node(raft_node_id_t node_id){
        auto iter = _catch_up_nodes.find(node_id);
        if(iter == _catch_up_nodes.end())
            return nullptr;
        return iter->second.node;
    }

    void clear_catch_up_nodes(){
        _catch_up_nodes.clear();
    }

    int get_catch_up_nodes_size(){
        return _catch_up_nodes.size();
    }

    class catch_up_node{
    public:
        bool catch_up;
        std::shared_ptr<raft_node> node;

        catch_up_node(std::shared_ptr<raft_node> nodep)
        : catch_up(false)
        , node(nodep) {}
    };

    void set_node_catch_up(raft_node_id_t node_id){
        auto iter = _catch_up_nodes.find(node_id);
        if(iter == _catch_up_nodes.end())
            return;
        iter->second.catch_up = true;
    }

    bool all_new_nodes_catch_up(){
        int ret = true;
        for(auto &[node_id, cnode] : _catch_up_nodes){
            if(!cnode.catch_up){
                ret = false;
                break;
            }
        }
        return ret;
    }

    bool find_node(raft_node_id_t node_id){
        return get_last_node_configuration().find_node(node_id);
    }

    bool find_old_node(raft_node_id_t node_id){
        return get_last_node_configuration().find_old_node(node_id);
    }

    int get_node_size(){
        return get_last_node_configuration().get_node_size();
    }
  
    int get_old_node_size(){
        return get_last_node_configuration().get_old_node_size();
    }

    void set_configuration_index(raft_index_t configuration_index){
        _configuration_index = configuration_index;
    }

    void updatet_raft_nodes_stat();

private:
    raft_server_t* _raft;
    std::deque<node_configuration> _configurations;
    cfg_state _state;
    std::shared_ptr<raft_entry_t> _cfg_entry;
    utils::context* _cfg_complete;

    //进入CFG_CATCHING_UP状态时设置，保存添加的所有节点
    absl::node_hash_map<raft_node_id_t, catch_up_node> _catch_up_nodes;

    //用于在CFG_UPDATE_NEW_CFG状态时标记RAFT_LOGTYPE_CONFIGURATION类型的entry的index
    raft_index_t _configuration_index;

    //用于统计CFG_JOINT或CFG_UPDATE_NEW_CFG状态时，RAFT_LOGTYPE_CONFIGURATION类型entry的commit信息
    int _old_node_match_size;
    int _old_node_fail_size; 
    int _new_node_match_size;
    int _new_node_fail_size;     
};