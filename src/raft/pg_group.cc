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
#include "pg_group.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "localstore/storage_manager.h"

#include <absl/container/flat_hash_map.h>

SPDK_LOG_REGISTER_COMPONENT(pg_group)

std::string pg_id_to_name(uint64_t pool_id, uint64_t pg_id){
    char name[128];
    
    snprintf(name, sizeof(name), "%lu.%lu", pool_id, pg_id);
    return name;
}

static int recv_installsnapshot(
       raft_server_t* raft,
       void *user_data,
       raft_node* node,
       const msg_installsnapshot_t* msg,
       msg_installsnapshot_response_t* r){
    (void)raft;
    (void)user_data;
    (void)node;
    (void)msg;
    (void)r;
    return 0;    
}

static int recv_installsnapshot_response(
        raft_server_t* raft,
        void *user_data,
        raft_node* node,
        msg_installsnapshot_response_t* r){
    (void)raft;
    (void)user_data;
    (void)node;
    (void)r;
    return 0;        
}

raft_cbs_t raft_funcs = {
    .recv_installsnapshot = recv_installsnapshot,
    .recv_installsnapshot_response = recv_installsnapshot_response
};

int pg_group_t::create_pg(std::shared_ptr<state_machine> sm_ptr,  uint32_t shard_id, uint64_t pool_id, 
            uint64_t pg_id, std::vector<utils::osd_info_t>&& osds, disk_log* log){
    int ret = 0;
    auto raft = raft_new(_client, log, sm_ptr, pool_id, pg_id       
                                        , global_storage().kvs());

    raft->raft_set_callbacks(&raft_funcs, NULL);
    _pg_add(shard_id, raft, pool_id, pg_id);

    raft->init(std::move(osds), get_current_node_id());
    return 0;
}

void pg_group_t::delete_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id){
    SPDK_INFOLOG(pg_group, "remove pg %lu.%lu\n", pool_id, pg_id);
    _pg_remove(shard_id, pool_id, pg_id);
}

void pg_group_t::start_shard_manager(utils::complete_fun fun, void *arg)
{
    uint32_t i = 0;
    auto shard_num = _shard_mg.size();
    utils::multi_complete *complete = new utils::multi_complete(shard_num, fun, arg);

    for (i = 0; i < shard_num; i++)
    {
        _shard.invoke_on(
            i,
            [this, shard_id = i, complete](){
                _shard_mg[shard_id].start();
                complete->complete(0);
            });
    }
}

void pg_group_t::start(utils::complete_fun fun, void *arg){
    start_shard_manager(fun, arg);
}

void pg_group_t::change_pg_membership(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id, std::vector<raft_node_info>&& new_osds, utils::context* complete){
    auto name = pg_id_to_name(pool_id, pg_id);
    auto raft = _shard_mg[shard_id].get_pg(name);
    if(!raft){
        SPDK_WARNLOG("not found pg %lu.%lu\n", pool_id, pg_id);
        if(complete)
            complete->complete(err::RAFT_ERR_NOT_FOUND_PG);
        return;
    }
    if(!raft->raft_is_leader()){
        SPDK_INFOLOG(pg_group, "not leader of pg %lu.%lu\n", pool_id, pg_id);
        if(complete)
            complete->complete(0);
        return;        
    }

    std::map<raft_node_id_t, int> smp;
    int add_count = 0;
    int remove_count = 0;
    raft_node_info add_node;
    raft_node_info remove_node;

    for(auto& osd : new_osds){
        smp[osd.node_id()] = 1;
        auto it = raft->get_nodes_stat().get_node(osd.node_id());
        if(!it){
            add_count++;
            add_node = osd;
            SPDK_INFOLOG(pg_group, "add node %d to pg %lu.%lu\n", add_node.node_id(), raft->raft_get_pool_id(), raft->raft_get_pg_id());
        }
    }

    for(auto &node_stat : raft->get_nodes_stat()){
        if(smp.find(node_stat.first) == smp.end()){
            remove_node = node_stat.second->raft_get_node_info();
            remove_count++;
            SPDK_INFOLOG(pg_group, "remove node %d from pg %lu.%lu\n", remove_node.node_id(), raft->raft_get_pool_id(), raft->raft_get_pg_id());
        }
    }

    if(add_count + remove_count >= 2){
        raft->change_raft_membership(std::move(new_osds), complete);
    }else if(add_count == 1){
        raft->add_raft_membership(add_node, complete);
    }else if(remove_count == 1){
        raft->remove_raft_membership(remove_node, complete);
    }else{
        if(complete)
            complete->complete(0);        
    }
}


static int heartbeat_task(void *arg){
    shard_manager* manager = (shard_manager *)arg;
    manager->dispatch_heartbeats();
    return 0;
}

void shard_manager::start(){
    _heartbeat_timer = SPDK_POLLER_REGISTER(&heartbeat_task, this, HEARTBEAT_TIMER_INTERVAL_MSEC * 1000);    
}


std::vector<shard_manager::node_heartbeat> shard_manager::get_heartbeat_requests(){
    absl::flat_hash_map<
      raft_node_id_t,
      heartbeat_request*> pending_beats;

    raft_time_t now = utils::get_time();
    for(auto& p : _pgs){
        auto raft = p.second;
        if(raft->raft_get_identity() != RAFT_STATE_LEADER){
            continue;
        }

        auto create_heartbeat_request = [this, raft, now, &pending_beats](const std::shared_ptr<raft_node> node) mutable{
            if (raft->raft_is_self(node.get()))
                return;
            SPDK_DEBUGLOG(pg_group, "node: %d pg: %lu.%lu suppress_heartbeats: %d heartbeating: %d  append_time: %lu heartbeat_timeout: %d now: %lu\n", 
                    node->raft_node_get_id(), raft->raft_get_pool_id(), raft->raft_get_pg_id(),
                    node->raft_get_suppress_heartbeats(),
                    node->raft_node_is_heartbeating(), node->raft_get_append_time(),
                    raft->raft_get_heartbeat_timeout(), now);
            if(node->raft_get_suppress_heartbeats())
                return;
            
            // if(node->raft_node_is_heartbeating())
                // return;
            if(node->raft_get_append_time() + raft->raft_get_heartbeat_timeout() > now)
                return;
            
            SPDK_DEBUGLOG(pg_group, "------ heartbeat to node: %d pg: %lu.%lu\n", 
                    node->raft_node_get_id(), raft->raft_get_pool_id(), raft->raft_get_pg_id());
            // node->raft_node_set_heartbeating(true);
            node->raft_set_append_time(now); 

            raft->raft_set_election_timer(now);
            heartbeat_request* req = nullptr;
            if(pending_beats.contains(node->raft_node_get_id())){
                req = pending_beats[node->raft_node_get_id()];
            }else{
                req = new heartbeat_request();
                pending_beats[node->raft_node_get_id()] = req;
            }
            auto meta_ptr = req->add_heartbeats();
            meta_ptr->set_node_id(raft->raft_get_nodeid());
            meta_ptr->set_target_node_id(node->raft_node_get_id());
            meta_ptr->set_pool_id(raft->raft_get_pool_id());
            meta_ptr->set_pg_id(raft->raft_get_pg_id());
            meta_ptr->set_term(raft->raft_get_current_term());
            raft_index_t next_idx = node->raft_node_get_next_idx();
            meta_ptr->set_prev_log_idx(next_idx - 1);
            
            raft_term_t term = 0;
            auto got = raft->raft_get_entry_term(meta_ptr->prev_log_idx(), term);
            assert(got);
            (void)got;
            meta_ptr->set_prev_log_term(term);
            meta_ptr->set_leader_commit(raft->raft_get_commit_idx());
        };
        raft->for_each_node(create_heartbeat_request);
    }

    std::vector<shard_manager::node_heartbeat> reqs;
    for (auto& p : pending_beats) {        
        shard_manager::node_heartbeat req(p.first, p.second);
        reqs.push_back(std::move(req));
    }
    return reqs; 
}

void shard_manager::dispatch_heartbeats(){
    auto reqs = get_heartbeat_requests();
    for(auto &req : reqs){
        _group->get_raft_client_proto().send_heartbeat(req.target, req.request, _group);
    }
}