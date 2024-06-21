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
#include <assert.h>
#include <string>
#include <vector>
#include <memory>

#include "raft_types.h"
#include "raft_node.h"
#include "raft_log.h"
#include "state_machine.h"
#include "rpc/raft_msg.pb.h"
#include "utils/utils.h"
#include "raft/raft_client_protocol.h"
#include "raft/append_entry_buffer.h"
#include "localstore/kv_store.h"
#include "raft/configuration_manager.h"
#include "localstore/object_recovery.h"

constexpr int32_t TIMER_PERIOD_MSEC = 500;    //毫秒
constexpr int32_t HEARTBEAT_TIMER_INTERVAL_MSEC = 500;   //毫秒

constexpr int32_t SNAPSHOT_MAX_CHUNK = 1;
constexpr int32_t SNAPSHOT_MAX_CONCURRENT = 1;

constexpr int32_t CATCH_UP_NUM = 200;

typedef enum {
    RAFT_MEMBERSHIP_ADD,
    RAFT_MEMBERSHIP_REMOVE,
} raft_membership_e;

typedef enum {
    RAFT_STATE_NONE,
    RAFT_STATE_FOLLOWER,
    RAFT_STATE_CANDIDATE,
    RAFT_STATE_LEADER
} raft_identity;

enum class raft_op_state {
    RAFT_INIT, 
    RAFT_ACTIVE, 
    RAFT_DOWN, 
    RAFT_DELETE
};

int raft_state_to_errno(raft_op_state state);

std::string pg_id_to_name(uint64_t pool_id, uint64_t pg_id);

using raft_complete = std::function<void (void *, int)>;

namespace monitor {
    class client;
}

class raft_server_t;

class raft_server_t{
public:
    raft_server_t(raft_client_protocol& client, disk_log* log, std::shared_ptr<state_machine> sm_ptr,
                    uint64_t pool_id, uint64_t pg_id, kvstore *, std::shared_ptr<monitor::client>);

    ~raft_server_t();

    void raft_randomize_election_timeout(){
        /* [_election_timeout, 2 * _election_timeout) */
        _election_timeout_rand = _election_timeout + rand() % _election_timeout;
    }

    int raft_get_election_timeout_rand(){
        return _election_timeout_rand;
    }    

    /** Set election timeout.
     * The amount of time that needs to elapse before we assume the leader is down
     * @param[in] msec Election timeout in milliseconds */
    void raft_set_election_timeout(int millisec)
    {
        _election_timeout = millisec;
        raft_randomize_election_timeout();
    }

    /** Set request timeout in milliseconds.
     * The amount of time before we resend an appendentries message
     * @param[in] msec Request timeout in milliseconds */
    void raft_set_heartbeat_timeout(int millisec)
    {
        _heartbeat_timeout = millisec;
    }

    /** Set lease maintenance grace.
     * The amount of time granted to a leader to reestablish an expired lease
     * before the leader is concluded to be unable to maintain the lease (a leader
     * who is unable to maintain leases from a majority steps down voluntarily)
     * @param[in] msec Lease mainenace grace in milliseconds */
    void raft_set_lease_maintenance_grace(int millisec)
    {
        _lease_maintenance_grace = millisec;
    }

    /** Set "first start" flag.
     * This indicates that me represents the first start of the (persistent)
     * server. */
    void raft_set_first_start()
    {
        _first_start = true;
    }

    bool raft_get_first_start()
    {
        return _first_start;
    }


   /** Set this server's node ID. */
    void raft_set_nodeid(raft_node_id_t id)
    {
        assert(_node_id == -1);
        _node_id = id;
    }

    /**
     * @return server's node ID; -1 if it doesn't know what it is */
    raft_node_id_t raft_get_nodeid()
    {
        return _node_id;
    }

    /**
     * @return currently configured election timeout in milliseconds */
    int raft_get_election_timeout()
    {
        return _election_timeout;
    }

    /**
     * @return request timeout in milliseconds */
    int raft_get_heartbeat_timeout()
    {
        return _heartbeat_timeout;
    }

    /**
     * @return lease maintenance grace in milliseconds */
    int raft_get_lease_maintenance_grace()
    {
        return _lease_maintenance_grace;
    }

    /**
     * @return currently elapsed timeout in milliseconds */
    int raft_get_timeout_elapsed()
    {
        return utils::get_time() - _election_timer;
    }

    void raft_set_voted_for(raft_node_id_t node_id)
    {
        _voted_for = node_id;
    }

    /**
     * @return node ID of who I voted for */
    raft_node_id_t raft_get_voted_for()
    {
        return _voted_for;
    }

    /**
     * @return number of voting nodes that this server has */
    std::pair<uint64_t, uint64_t> raft_get_num_voting_nodes()
    {
        return std::make_pair(_nodes_stat.size(), _nodes_stat.new_node_size());
    }

    /** Set the current term.
     * This should be used to reload persistent state, ie. the current_term field.
     * @param[in] term The new current term
     * @return
     *  0 on success */
    int raft_set_current_term(const raft_term_t term);

    raft_term_t raft_get_current_term()
    {
        return _current_term;
    }

    /**
     * @return current log index */
    raft_index_t raft_get_current_idx()
    {
        return _current_idx;
    }

    void raft_set_current_idx(raft_index_t current_idx){
        _current_idx = current_idx;
    }

    /** Set the commit idx.
     * This should be used to reload persistent state, ie. the commit_idx field.
     * @param[in] commit_idx The new commit index. */
    void raft_set_commit_idx(raft_index_t idx)
    {
        assert(_commit_idx <= idx);
        assert(idx <= raft_get_current_idx());
        _commit_idx = idx;
    }

    void raft_set_last_applied_idx(raft_index_t idx)
    {
        _machine->set_last_applied_idx(idx);
    }

    /**
     * @return index of last applied entry */
    raft_index_t raft_get_last_applied_idx()
    {
        return _machine->get_last_applied_idx();
    }

    raft_index_t raft_get_commit_idx()
    {
        return _commit_idx;
    }

    void raft_set_identity(raft_identity identity)
    {
        /* if became the leader, then update the current leader entry */
        if (identity == RAFT_STATE_LEADER)
            _leader_id = _node_id;
        _identity = identity;
    }

    /** Tell if we are a leader, candidate or follower.
     * @return get state of type raft_identity. */
    raft_identity raft_get_identity()
    {
        return _identity;
    }

    /**
     * @param[in] node_id The node's ID
     * @return node pointed to by node ID */
    std::shared_ptr<raft_node> raft_get_node(raft_node_id_t node_id)
    { 
        auto nodep = _nodes_stat.get_node(node_id);
        if(!nodep){
            nodep = _nodes_stat.get_new_node(node_id);
        }
        return nodep;
    }

    /**
     * @return the server's node */
    std::shared_ptr<raft_node> raft_get_my_node()
    {
        return raft_get_node(_node_id);
    }

    /** Get what this node thinks the node ID of the leader is.
     * @return node of what this node thinks is the valid leader;
     *   -1 if the leader is unknown */
    raft_node_id_t raft_get_current_leader()
    {
        return _leader_id;
    }

    void raft_set_current_leader(raft_node_id_t node_id)
    {
        _leader_id = node_id;
    }

    bool raft_is_follower()
    {
        return raft_get_identity() == RAFT_STATE_FOLLOWER;
    }

    bool raft_is_leader()
    {
        return raft_get_identity() == RAFT_STATE_LEADER;
    }

    bool raft_is_candidate()
    {
        return raft_get_identity() == RAFT_STATE_CANDIDATE;
    }

    bool raft_is_prevoted_candidate()
    {
        return raft_get_identity() == RAFT_STATE_CANDIDATE && !_prevote;
    }

    /**
     * @return 1 if node ID matches the server; 0 otherwise */
    bool raft_is_self(std::shared_ptr<raft_node> node)
    {
        return (node && node->raft_node_get_id() == _node_id);
    }

    void raft_set_snapshot_in_progress(bool in_progress) {
        _snapshot_in_progress = in_progress;
    }

    /** Check is a snapshot is in progress **/
    bool raft_get_snapshot_in_progress()
    {
        return _snapshot_in_progress;
    }

    /** Get last applied entry **/
    std::shared_ptr<raft_entry_t> raft_get_last_applied_entry();

    std::shared_ptr<raft_log> raft_get_log(){
        return _log;
    }

    void raft_set_election_timer(raft_time_t election_timer){
        _election_timer = election_timer;
    }

    raft_time_t raft_get_election_timer(){
        return _election_timer;
    }

    void raft_set_start_time(raft_time_t start_time){
        _start_time = start_time;
    }

    raft_time_t raft_get_start_time(){
        return _start_time;
    }

    int raft_get_prevote(){
        return _prevote;
    }

    void raft_set_prevote(int prevote){
        _prevote = prevote;
    }

    using get_entry_complete = std::function<void (std::shared_ptr<raft_entry_t>)>;
    /**
     * @param[in] idx The entry's index
     **/
    void raft_get_entry_by_idx(raft_index_t idx, get_entry_complete cb_fn)
    {
        auto ety = raft_get_log()->log_get_at_idx(idx);
        if(ety){
            cb_fn(ety);
            return;
        }

        raft_get_log()->disk_read(
          idx,
          idx,
          [cb_fn = std::move(cb_fn)](std::vector<raft_entry_t>&& entries, int rberrno){
             if(rberrno != 0 || entries.size() == 0){
               cb_fn(nullptr);
               return; 
             }
             cb_fn(std::make_shared<raft_entry_t>(entries[0]));
          }  
        );
    }

    void stop();
    void raft_destroy(raft_complete cb_fn, void* arg);

    /** Become leader
     * WARNING: this is a dangerous function call. It could lead to your cluster
     * losing it's consensus guarantees. */
    void raft_become_leader();

    int raft_become_candidate();

    int raft_become_prevoted_candidate();

    /** Vote for a server.
     * This should be used to reload persistent state, ie. the voted-for field.
     * @param[in] nodeid The server to vote for by nodeid
     * @return
     *  0 on success */
    int raft_vote_for_nodeid(const raft_node_id_t nodeid);

    int raft_count_votes();

    /** @return
     *  nonzero if server is leader and has leases from majority of voting nodes;
     *  0 if server is not leader or lacks leases from majority of voting nodes;
    */
    bool raft_has_majority_leases();

    /** Process events that are dependent on time passing.
     * @return
     *  0 on success;
     *  -1 on failure; */
    int raft_periodic();

    void follow_raft_disk_append_finish(raft_index_t start_idx, raft_index_t end_idx, raft_index_t commit_idx, int result);

    /** Receive an appendentries message.
     *
     *
     * @param[in] node_id The node who sent us this message
     * @param[in] ae The appendentries message
     * @param[out] r The resulting response
     * @return
     *  0 on success
     *  */
    int raft_recv_appendentries(raft_node_id_t node_id,
                                const msg_appendentries_t* ae,
                                msg_appendentries_response_t *r,
                                utils::context* complete);

    /** Receive a response from an appendentries message we sent.
     * @param[in] node The node who sent us this message
     * @param[in] r The appendentries response message
     * @return
     *  0 on success;
     *  -1 on error;
     *  RAFT_ERR_NOT_LEADER server is not the leader */
    int raft_process_appendentries_reply(msg_appendentries_response_t* r, bool is_heartbeat = false);

    /** Become follower. This may be used to give up leadership. It does not change
     * currentTerm. */
    void raft_become_follower();

    void raft_write_entry_finish(raft_index_t start_idx, raft_index_t end_idx, int result);
    void follow_raft_write_entry_finish(raft_index_t start_idx, raft_index_t end_idx, int result);

    void raft_disk_append_finish(raft_index_t start_idx, raft_index_t end_idx, int result);

    /**
     *
     * Append the entry to the log and send appendentries to followers.
     *
     *
     * @param[in] ety The entry message
     * @return
     *  0 on success;
     *  RAFT_ERR_NOT_LEADER server is not the leader;
     *  RAFT_ERR_NOMEM memory allocation failure
     */
    int raft_write_entry(std::shared_ptr<raft_entry_t> ety, utils::context *complete);

    //停止正在处理的entrys，给客户端发送响应。 stop raft时调用
    void stop_processing_entrys(int state);
    void stop_flush(int state);
    void raft_flush();
    void process_conf_change_entry(std::shared_ptr<raft_entry_t> entry);

    int raft_send_heartbeat(std::shared_ptr<raft_node> node);

    //send append entry request which containing raft_entry at [start_index, end_index] to node
    int raft_send_appendentries(std::shared_ptr<raft_node> node, raft_index_t start_index, raft_index_t end_index);

    bool raft_get_entry_term(raft_index_t idx, raft_term_t& term);

    int raft_send_heartbeat_all();

    /**
     * @return number of votes this server has received this election 
     *  <node_vote_num, new_node_vote_num>
     * */
    std::pair<uint64_t, uint64_t> raft_get_nvotes_for_me();

    //截断idx（包含）之后的log entry
    int raft_log_truncate(raft_index_t idx, log_op_complete cb_fn, void* arg);

    /** Add entries to the server's log cache. used by follower
     * @param[in] entries List of entries to be appended
     * @return
     *  0 on success;
     * */
    int raft_append_entries(std::vector<std::pair<std::shared_ptr<raft_entry_t>, utils::context*>>& entries){
        return raft_get_log()->log_append(entries);
    }

    /** Add entry to the server's log cache.  used by leader
     */
    int raft_append_entry(std::shared_ptr<raft_entry_t> entry, utils::context* complete){
        return raft_get_log()->log_append(entry, complete);
    }

    void raft_disk_append_entries(raft_index_t start_idx, raft_index_t end_idx, utils::context* complete){
        raft_get_log()->disk_append(start_idx, end_idx, complete);
    }

    int entry_queue_flush(){
        return raft_get_log()->entry_queue_flush();
    }

    raft_index_t raft_get_last_cache_entry(){
        return raft_get_log()->get_last_cache_entry();
    }

    /**
     * @return 0 on error */
    int raft_send_requestvote(std::shared_ptr<raft_node> node);

    /** Receive a requestvote message.
     * @param[in] node The node who sent us this message
     * @param[in] vr The requestvote message
     * @param[out] r The resulting response
     * @return 0 on success */
    int raft_recv_requestvote(raft_node_id_t node_id,
                              const msg_requestvote_t* vr,
                              msg_requestvote_response_t *r);

    /** Receive a response from a requestvote message we sent.
     * @param[in] r The requestvote response message
     * @return
     *  0 on success;
     *  RAFT_ERR_SHUTDOWN server MUST shutdown; */
    int raft_process_requestvote_reply(
                                       msg_requestvote_response_t* r);

    void raft_set_timer();

    int raft_election_start();

    void start_timed_task(){
        _append_entries_buffer.start();
        _machine->start();
    }

    void stop_timed_task(){
        _machine->stop();
        _append_entries_buffer.stop();
    }

    void append_entries_to_buffer(const msg_appendentries_t* request,
                msg_appendentries_response_t* response,
                utils::context* complete){
        _append_entries_buffer.enqueue(request, response, complete);
    }

    int  save_vote_for(const raft_node_id_t nodeid){
        std::string key = std::to_string(_pool_id) + "." + std::to_string(_pg_id) + ".vote_for";
        std::string val = std::to_string(nodeid);
        _kv->put(key, val);
        return 0;
    }

    std::optional<raft_node_id_t> load_vote_for(){
        std::string key = std::to_string(_pool_id) + "." + std::to_string(_pg_id) + ".vote_for";
        auto val = _kv->get(key);
        if(!val.has_value())
            return std::nullopt;
        _voted_for = atoi(val.value().c_str());
        return _voted_for;
    }

    int save_current_term(const raft_term_t term){
        std::string key = std::to_string(_pool_id) + "." + std::to_string(_pg_id) + ".term";
        std::string val = std::to_string(term);
        _kv->put(key, val);
        return 0;
    }

    std::optional<raft_term_t> load_current_term(){
        std::string key = std::to_string(_pool_id) + "." + std::to_string(_pg_id) + ".term";
        auto val = _kv->get(key);
        if(!val.has_value())
            return std::nullopt;
        _current_term = atol(val.value().c_str());
        return _current_term;
    }

    int save_node_configuration(std::string& value){
        std::string key = std::to_string(_pool_id) + "." + std::to_string(_pg_id) + ".node_cfg";
        _kv->put(key, value);
        return 0;
    }

    int save_last_apply_index(raft_index_t last_applied_idx){
        std::string key = std::to_string(_pool_id) + "." + std::to_string(_pg_id) + ".lapply_idx";
        _kv->put(key, std::to_string(last_applied_idx));
        return 0;        
    }

    std::optional<raft_index_t> load_last_apply_index(){
        std::string key = std::to_string(_pool_id) + "." + std::to_string(_pg_id) + ".lapply_idx";
        auto val = _kv->get(key);
        if(!val.has_value())
            return std::nullopt;
        auto last_applied_idx = atol(val.value().c_str());  
        return last_applied_idx;      
    }

    std::optional<std::string> load_node_configuration(){
        std::string key = std::to_string(_pool_id) + "." + std::to_string(_pg_id) + ".node_cfg";
        auto val = _kv->get(key);
        return val;       
    }


    void start_raft_timer(int raft_heartbeat_period_time_msec, int  raft_lease_time_msec, int  raft_election_timeout_msec);

    void for_each_node(each_node_func&& f)  {
        _nodes_stat.for_all_node(std::move(f));
    }

    uint64_t raft_get_pool_id(){
        return _pool_id;
    }

    uint64_t raft_get_pg_id(){
        return _pg_id;
    }

    std::string raft_get_pg_name(){
        return pg_id_to_name(_pool_id, _pg_id);
    }

    bool is_lease_valid(){
        return raft_has_majority_leases();
    }

    msg_appendentries_t* create_appendentries(std::shared_ptr<raft_node> node, raft_index_t next_idx);
    void dispatch_recovery(std::shared_ptr<raft_node> node);
    void do_recovery(std::shared_ptr<raft_node> node);

    void raft_set_op_state(raft_op_state op_state){
        _op_state = op_state;
    }

    raft_op_state raft_get_op_state(){
        return _op_state;
    }

    void init(std::vector<utils::osd_info_t>&& node_list, raft_node_id_t current_node, 
      int raft_heartbeat_period_time_msec, int  raft_lease_time_msec, int  raft_election_timeout_msec);

    //添加单个成员
    void add_raft_membership(const raft_node_info& node, utils::context* complete);
    //删除单个成员
    void remove_raft_membership(const raft_node_info& node, utils::context* complete);
    //变更多个成员
    void change_raft_membership(std::vector<raft_node_info>&& new_nodes, utils::context* complete);

    int raft_configuration_entry(std::shared_ptr<raft_entry_t> ety, utils::context *complete);
    
    std::vector<raft_node_id_t> raft_get_nodes_id(){
        return _configuration_manager.get_last_node_configuration().get_nodes_id();
    }

    void process_conf_change_add_nonvoting(std::shared_ptr<raft_entry_t> entry);
    void process_conf_change_configuration(std::shared_ptr<raft_entry_t> entry);

    void set_configuration_state(cfg_state state){
        _configuration_manager.set_state(state);
    }

    cfg_state get_configuration_state(){
        return _configuration_manager.get_state();
    }

    bool configuration_is_changing(){
        return _configuration_manager.is_busy();
    }

    bool conf_change_catch_up_leader(std::shared_ptr<raft_node> node){
        return (raft_get_commit_idx() < (CATCH_UP_NUM + node->raft_node_get_match_idx())); 
    }

    /**
     * @param[in] node_id The node's ID
     * @return node pointed to by node ID */
    std::shared_ptr<raft_node> raft_get_cfg_node(raft_node_id_t node_id)
    { 
        auto node = _nodes_stat.get_node(node_id);
        if(node)
            return node;
        auto cfg_state = get_configuration_state();
        if(cfg_state == cfg_state::CFG_CATCHING_UP 
                || cfg_state == cfg_state::CFG_JOINT
                || cfg_state == cfg_state::CFG_UPDATE_NEW_CFG){
            node = _configuration_manager.get_catch_up_node(node_id);
            return  node;
        }else{
            node = _nodes_stat.get_new_node(node_id);
            if(node)
                return node;
        }
        return nullptr;
    }

    void set_cfg_entry_complete(std::shared_ptr<raft_entry_t> cfg_entry, utils::context* cfg_complete){
        _configuration_manager.set_cfg_entry_complete(cfg_entry, cfg_complete);
    }

    void set_cfg_complete(utils::context* cfg_complete){
        _configuration_manager.set_cfg_complete(cfg_complete);
    }

    void reset_cfg_entry(){
        _configuration_manager.reset_cfg_entry();
    }

    auto get_cfg_entry_complete(){
        return _configuration_manager.get_cfg_entry_complete();
    }

    //Only the leader will call the function
    int cfg_change_process(int result, raft_index_t rsp_current_idx, std::shared_ptr<raft_node> node){
        return _configuration_manager.cfg_change_process(result, rsp_current_idx, node);
    }

    bool node_is_cfg_change_process(std::shared_ptr<raft_node> node){
        auto nodep = _configuration_manager.get_catch_up_node(node->raft_node_get_id());
        return (nodep != nullptr);
    }

    bool cfg_change_is_in_progress(){
        return _configuration_manager.cfg_change_is_in_progress();
    }

    void set_configuration_index(raft_index_t configuration_index){
        _configuration_manager.set_configuration_index(configuration_index);
    }

    void check_and_set_configuration(std::shared_ptr<raft_entry_t> entry);

    void update_nodes_stat(node_configuration& cfg, 
            std::vector<std::shared_ptr<raft_node>>& new_add_nodes){
        _nodes_stat.update_with_node_configuration(cfg, new_add_nodes);   
    }

    void update_nodes_stat(node_configuration& cfg){
        _nodes_stat.update_with_node_configuration(cfg, {}); 
    }

    raft_nodes&  get_nodes_stat(){
        return _nodes_stat;
    }

    /* The leader node is remove from the new member list, then passes leadership to other node
     */
    void raft_step_down(raft_index_t commit_index);

    int raft_send_timeout_now(raft_node_id_t node_id);
    int raft_process_timeout_now_reply(timeout_now_response* rsp);

    /** Receive an installsnapshot message. */
    int raft_recv_installsnapshot(raft_node_id_t node_id,
                                  const installsnapshot_request* request,
                                  installsnapshot_response *response,
                                  utils::context* complete);
    int raft_process_installsnapshot_reply(installsnapshot_response *rsp);

    /** Receive an snapshot_check message. */
    int raft_recv_snapshot_check(raft_node_id_t node_id,
                                  const snapshot_check_request* request,
                                  snapshot_check_response *response,
                                  utils::context* complete);
    int raft_process_snapshot_check_reply(snapshot_check_response *rsp);
    int raft_send_snapshot_check(std::shared_ptr<raft_node> node);
    int raft_send_installsnapshot(installsnapshot_request *req, raft_node_id_t node_id);

    std::shared_ptr<state_machine>  get_machine(){
        return _machine;
    }

    raft_index_t get_snapshot_index(){
        return _snapshot_index;
    }

    raft_term_t get_snapshot_term(){
        return _snapshot_term;
    }

    object_recovery* get_object_recovery(){
        return _obr;
    }

    void free_object_recovery(){
        delete _obr;
        _obr = nullptr;
    }

    /*
     *  follower节点在接收完leader的快照后，才能调用
     */
    void set_index_after_snapshot(raft_index_t index){
        raft_set_last_applied_idx(index);
        raft_set_current_idx(index);
        raft_set_commit_idx(index);
        raft_get_log()->set_next_idx(index + 1);
    }

    node_configuration_manager &get_node_configuration_manager(){
        return _configuration_manager;
    }

    void load(raft_node_id_t current_node, raft_complete cb_fn, void *arg, 
      int raft_heartbeat_period_time_msec, int  raft_lease_time_msec, int  raft_election_timeout_msec);
    void set_last_applied_idx_after_load(raft_index_t idx){
        _machine->set_last_applied_idx(idx);
        _log->set_trim_index(idx);
    }
    void active_raft();
public:
    enum class task_type : uint8_t{
        SNAP_CHECK_TASK = 1
    };
    
    struct task_info{
        task_info(task_type type, raft_node_id_t node_id)
        : type(type)
        , node_id(node_id){

        }

        task_type type;
        raft_node_id_t node_id; 
    };

    void push_task(task_type type, raft_node_id_t node_id){
        task_info task(type, node_id);
        _tasks.push(std::move(task));
    }

    void task_loop();
    void handle_snap_check_task(task_info& task);

    void raft_node_process_commit(int result, raft_index_t index, raft_node_id_t node_id);
    void send_pg_member_change_finished_notify(int result);
private:
    int _recovery_by_snapshot(std::shared_ptr<raft_node> node);
    bool _has_majority_leases(raft_time_t now, int with_grace);
    int _should_grant_vote(const msg_requestvote_t* vr);
    int _has_lease(std::shared_ptr<raft_node> node, raft_time_t now, int with_grace);
    void _raft_get_entries_from_idx(raft_index_t start_index, raft_index_t end_index, msg_appendentries_t* ae);
    void _send_leader_be_elected_notify();

    /* the server's best guess of what the current term is
     * starts at zero */
    raft_term_t _current_term;

    /* The candidate the server voted for in its current term,
     * or Nil if it hasn't voted for any.  */
    raft_node_id_t _voted_for;

    /* the log which is replicated */
    std::shared_ptr<raft_log> _log; 

    /* Volatile state: */

    /* idx of highest log entry known to be committed */
    raft_index_t _commit_idx;

    /* follower/leader/candidate indicator */
    raft_identity _identity;

    /* true if this server is in the candidate prevote state (§4.2.3, §9.6) */
    int _prevote;

    /* start time of this server */
    raft_time_t _start_time;

    /* start time of election timer */
    raft_time_t _election_timer;

    int _election_timeout;
    int _election_timeout_rand;
    int _heartbeat_timeout;

    /* what this node thinks is the node ID of the current leader,
     * or -1 if there isn't a known current leader. */
    raft_node_id_t _leader_id;

    /* my node ID */
    raft_node_id_t _node_id;
    
    //是否正在为数据恢复而创建snapshot
    bool _snapshot_in_progress;

    /* grace period after each lease expiration time honored when we determine
     * if a leader is maintaining leases from a majority (see raft_periodic) */
    int _lease_maintenance_grace;

    /* represents the first start of this (persistent) server; not a restart */
    bool _first_start;

    std::shared_ptr<state_machine>  _machine;

    uint64_t _pool_id;
    uint64_t _pg_id;

    raft_index_t _first_idx;     //当前正在处理的一批log中第一个的idx(只用在leader中)
    raft_index_t _current_idx;   //leader中是当前正在处理的一批log中最后一个的idx，follower中是已经处理的最新的idx
    raft_client_protocol& _client;

    append_entries_buffer _append_entries_buffer;

    kvstore *_kv;
   
    struct spdk_poller * _raft_timer;
    raft_op_state _op_state;
    
    raft_index_t _last_index_before_become_leader;

    raft_nodes  _nodes_stat;  //配置更新后，这个需要更新为最新的
    node_configuration_manager _configuration_manager;

    raft_index_t _snapshot_index;

    /* term of the snapshot base */
    raft_term_t _snapshot_term;

    object_recovery *_obr; 
    std::queue<task_info> _tasks;

    std::shared_ptr<monitor::client> _mon_client;
}; 

bool raft_votes_is_majority(std::pair<uint64_t, uint64_t> &node_num_pair, std::pair<uint64_t, uint64_t> &nvotes_pair);

#define RAFT_REQUESTVOTE_ERR_GRANTED          1
#define RAFT_REQUESTVOTE_ERR_NOT_GRANTED      0
#define RAFT_REQUESTVOTE_ERR_UNKNOWN_NODE    -1

typedef enum {
    /**
     * Regular log type.
     * This is solely for application data intended for the FSM.
     */
    RAFT_LOGTYPE_WRITE,
    RAFT_LOGTYPE_DELETE,
    
    /**
     * Membership change.
     * Non-voting nodes can't cast votes or start elections.
     * Used to start the first phase of a member change: catch up with the leader,
     */
    RAFT_LOGTYPE_ADD_NONVOTING_NODE,

    RAFT_LOGTYPE_CONFIGURATION,
} raft_logtype_e;

/** Initialise a new Raft server.
 *
 * @return newly initialised Raft server */
extern std::shared_ptr<raft_server_t> raft_new(raft_client_protocol& client,
        disk_log* log, std::shared_ptr<state_machine> sm_ptr, uint64_t pool_id, 
        uint64_t pg_id, kvstore*, std::shared_ptr<monitor::client>);

/** Determine if entry is voting configuration change.
 * @param[in] ety The entry to query.
 * @return 1 if this is a voting configuration change. */
bool raft_entry_is_voting_cfg_change(raft_entry_t* ety);

/** Determine if entry is configuration change.
 * @param[in] ety The entry to query.
 */
bool raft_entry_is_cfg_change(std::shared_ptr<raft_entry_t> ety);