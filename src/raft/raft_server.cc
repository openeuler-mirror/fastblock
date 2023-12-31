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

#include <string.h>
#include <assert.h>

/* for varags */
#include <stdarg.h>

#include "raft.h"
#include "raft_log.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "utils/err_num.h"
#include "localstore/kv_store.h"

constexpr long recovery_max_entry_num = 100;

std::shared_ptr<raft_server_t> raft_new(raft_client_protocol& client,
        disk_log* log, std::shared_ptr<state_machine> sm_ptr, uint64_t pool_id, uint64_t pg_id
        , kvstore *kv)
{
    auto raft = std::make_shared<raft_server_t>(client, std::move(log), sm_ptr, 
                                               pool_id, pg_id, kv);
    return raft;
}

void raft_server_t::raft_set_callbacks(raft_cbs_t* funcs, void* _udata)
{
    memcpy(&cb, funcs, sizeof(raft_cbs_t));
    raft_set_udata(_udata);
    raft_get_log()->log_set_raft((void *)this);

    /* We couldn't initialize the time fields without the callback. */
    raft_time_t now = get_time();
    raft_set_election_timer(now);
    raft_set_start_time(now);
}

int raft_server_t::raft_truncate_from_idx(raft_index_t idx)
{
    assert(raft_get_commit_idx() < idx);

    if (idx <= raft_get_voting_cfg_change_log_idx())
        raft_set_voting_cfg_change_log_idx(-1);

    return raft_get_log()->log_truncate(idx);
}

int raft_server_t::raft_election_start()
{
    SPDK_WARNLOG("election starting: pool.pg %lu.%lu %d %ld, current term: %ld current index: %ld\n", pool_id, pg_id,
          raft_get_election_timeout_rand(), raft_get_election_timer(), raft_get_current_term(),
          raft_get_current_idx());

    return raft_become_candidate();
}

void raft_server_t::raft_become_leader()
{
    SPDK_WARNLOG("becoming leader of pg %lu.%lu at term:%ld\n", pool_id, pg_id, raft_get_current_term());

    raft_set_state(RAFT_STATE_LEADER);
    raft_time_t now = get_time();
    raft_set_election_timer(now);
    _last_index_before_become_leader = raft_get_current_idx();
    for(auto _node : nodes){
        raft_node* node = _node.get(); 

        node->raft_node_set_match_idx(0);
        if (raft_is_self(node))
            continue;    

        node->raft_node_set_next_idx(raft_get_current_idx() + 1);
        node->raft_node_set_effective_time(now);
        raft_send_appendentries(node);   
    }
}

int raft_server_t::raft_count_votes()
{
    int votes = raft_get_nvotes_for_me();
    if (raft_votes_is_majority(raft_get_num_voting_nodes(), votes))
    {
        if (raft_get_prevote())
        {
            int e = raft_become_prevoted_candidate();
            if (0 != e)
                return e;
        }
        else
            raft_become_leader();
    }
    return 0;
}

int raft_server_t::raft_become_candidate()
{
    raft_set_current_term(raft_get_current_term() + 1);
    SPDK_INFOLOG(pg_group, "becoming candidate pool.pg %lu.%lu term: %ld\n", pool_id, pg_id, raft_get_current_term());

    raft_set_state(RAFT_STATE_CANDIDATE);
    raft_set_prevote(1);

    for(auto node : nodes){
        node->raft_node_vote_for_me(0);
    }
    raft_get_my_node()->raft_node_vote_for_me(1);

    raft_set_current_leader(-1);
    raft_randomize_election_timeout();
    auto election_timer = get_time();
    raft_set_election_timer(election_timer);

    for(auto _node : nodes){
        raft_node* node = _node.get();

        if (!raft_is_self(node) &&
            node->raft_node_is_voting())
        {
            raft_send_requestvote(node);
        }
    }

    /* We've already got at least one prevote from ourself, which is enough if
     * we are the only voting node. */
    return raft_count_votes();
}

int raft_server_t::raft_become_prevoted_candidate()
{
    SPDK_INFOLOG(pg_group, "becoming prevoted candidate\n");

    int e = raft_set_current_term(raft_get_current_term() + 1);
    if (0 != e)
        return e;
    for(auto node : nodes){
        node->raft_node_vote_for_me(0);
    }
    e = raft_vote_for_nodeid(raft_get_nodeid());
    if (0 != e)
        return e;
    raft_get_my_node()->raft_node_vote_for_me(1);
    raft_set_prevote(0);

    for(auto _node : nodes){
        raft_node* node = _node.get();

        if (!raft_is_self(node) &&
            node->raft_node_is_voting())
        {
            raft_send_requestvote(node);
        }
    }

    /* We've already got at least one vote from ourself, which is enough if we
     * are the only voting node. */
    return raft_count_votes();
}

void raft_server_t::raft_become_follower()
{
    SPDK_INFOLOG(pg_group, "becoming follower, term: %ld\n", raft_get_current_term());
    raft_set_state(RAFT_STATE_FOLLOWER);
    raft_randomize_election_timeout();
    auto election_timer = get_time();
    raft_set_election_timer(election_timer);
}

int raft_server_t::_has_lease(raft_node* node, raft_time_t now, int with_grace)
{
    if (raft_is_self(node))
        return 1;

    if (with_grace)
    {
        // SPDK_INFOLOG(pg_group, "now:%ld lease:%ld lease_maintenance_grace:%d effective_time:%ld election_timeout:%d\n",
                // now, node->raft_node_get_lease(), raft_get_lease_maintenance_grace(),
                // node->raft_node_get_effective_time(), raft_get_election_timeout());
        if (now < node->raft_node_get_lease() + raft_get_lease_maintenance_grace())
            return 1;
        /* Since a leader has no lease from any other node at the beginning of
         * its term, or from any new node the leader adds thereafter, we give
         * it some time to acquire the initial lease. */
        if (now - node->raft_node_get_effective_time() < raft_get_election_timeout() + raft_get_lease_maintenance_grace())
            return 1;
    }
    else
    {
        if (now < node->raft_node_get_lease())
            return 1;
    }

    return 0;
}

bool raft_server_t::_has_majority_leases(raft_time_t now, int with_grace)
{
    assert(raft_get_state() == RAFT_STATE_LEADER);

    int n = 0;
    int n_voting = 0;

    for(auto _node : nodes){
        raft_node* node = _node.get();
        if(node->raft_node_is_voting())
        {
            n_voting++;
            if (_has_lease(node, now, with_grace))
                n++;
        }
    }

    return n_voting / 2 + 1 <= n;
}

bool raft_server_t::raft_has_majority_leases()
{
    if (raft_get_state() != RAFT_STATE_LEADER)
        return false;

    /* Check without grace, because the caller may be checking leadership for
     * linearizability (§6.4). */
    return _has_majority_leases(get_time(), 0 /* with_grace */);
}

int raft_server_t::raft_periodic()
{
    raft_node *my_node = raft_get_my_node();
    raft_time_t now = get_time();

    if (raft_get_state() == RAFT_STATE_LEADER)
    {
        if (!_has_majority_leases(now, 1 /* with_grace */))
        {
            /* A leader who can't maintain majority leases shall step down. */
            SPDK_WARNLOG("pg: %lu.%lu unable to maintain majority leases\n", pool_id, pg_id);
            raft_become_follower();
            raft_set_current_leader(-1);
        }
    }
    else if (raft_get_election_timeout_rand() <= now - raft_get_election_timer() &&
        /* Don't become the leader when building snapshots or bad things will
         * happen when we get a client request */
        !raft_get_snapshot_in_progress())
    {
        if (my_node && my_node->raft_node_is_voting())
        {
            int e = raft_election_start();
            if (0 != e)
                return e;
        }
    }

    return 0;
}

bool raft_server_t::raft_get_entry_term(raft_index_t idx, raft_term_t& term)
{
    raft_term_t _term;
    auto got = raft_get_log()->disk_get_term(idx, _term);
    if (got){
        term = _term;
    }else if (idx == raft_get_log()->log_get_base()){
        term = raft_get_log()->log_get_base_term();
    }else{
        return false;
    }
    return true;
}

//接收到失败的response，如何处理raft_entry   todo ?
int raft_server_t::raft_process_appendentries_reply(
                                     msg_appendentries_response_t* r, bool is_heartbeat)
{
    SPDK_INFOLOG(pg_group, 
          "received appendentries response %s res: %d from %d ci:%ld rci:%ld 1stidx:%ld\
           ls=%ld  ct:%ld rt:%ld\n",
          r->success() == 1 ? "SUCCESS" : "fail", 
          r->success(),
          r->node_id(),
          raft_get_current_idx(),
          r->current_idx(),
          r->first_idx(),
          r->lease(),
          raft_get_current_term(),
          r->term());

    raft_node* node = raft_get_node(r->node_id());
    if (!node)
        return 1;

    auto  process_response = [this, r, node](int result, raft_index_t end_idx){
        if(node->raft_node_is_recovering() && result != 0){
            node->raft_node_set_recovering(false);
            return;
        }
        
        raft_index_t point = end_idx;
        SPDK_INFOLOG(pg_group, "current_idx: %lu commit_idx: %lu from node: %d\n", 
                end_idx, raft_get_commit_idx(), node->raft_node_get_id());

        if (point && raft_get_commit_idx() < point)
        {
            raft_term_t term;
            bool got = false;
            auto _entry = raft_get_log()->log_get_at_idx(point);
            if(_entry){
                got = true;
                term = _entry->term();
            }else
                got = raft_get_entry_term(point, term);
            if (got && term == raft_get_current_term())
            {
                int votes = 0;
                bool leader_match = false;
                for(auto _node : nodes)
                {
                    raft_node* tmpnode = _node.get();
                    if (
                        tmpnode->raft_node_is_voting() &&
                        (point <= tmpnode->raft_node_get_match_idx() || 
                        (result != 0 && _node->raft_node_get_id() == node->raft_node_get_id())))
                    {
                        votes++;
                    }
                    if(raft_is_self(tmpnode) && tmpnode->raft_node_is_voting() && point <= tmpnode->raft_node_get_match_idx()){
                        //确保leader已经提交了这条log
                        leader_match = true;
                    }
                }
    
                if (raft_get_num_voting_nodes() / 2 < votes && leader_match){
                    raft_set_commit_idx(point);
                    raft_flush();
                }
            }
        }
    };

    node->raft_set_suppress_heartbeats(false);
    if (!raft_is_leader()){
        SPDK_ERRLOG("node %d is not leader\n", raft_get_nodeid());
        process_response(err::RAFT_ERR_NOT_LEADER, node->raft_get_end_idx());
        return err::RAFT_ERR_NOT_LEADER;
    }

    if(r->success() == err::RAFT_ERR_NOT_FOUND_PG){
        SPDK_ERRLOG("node %d does not find pg\n", r->node_id());
        process_response(err::RAFT_ERR_NOT_FOUND_PG, node->raft_get_end_idx());
        return err::RAFT_ERR_NOT_FOUND_PG;
    }

    if(r->success() == err::E_ENOSPC){
        SPDK_ERRLOG("node %d does not have space\n", r->node_id());
        process_response(err::E_ENOSPC, node->raft_get_end_idx());
        return err::E_ENOSPC;
    }

    if(r->success() == err::RAFT_ERR_SHUTDOWN){
        SPDK_ERRLOG("node %d has a seriously wrong\n", r->node_id());
        process_response(err::RAFT_ERR_SHUTDOWN, node->raft_get_end_idx());
        return err::RAFT_ERR_SHUTDOWN;
    }

    if(r->success() == err::RAFT_ERR_PG_SHUTDOWN){
        SPDK_WARNLOG("pg %lu.%lu is shutdown at node %d\n", pool_id, pg_id, r->node_id());
        process_response(err::RAFT_ERR_PG_SHUTDOWN, node->raft_get_end_idx());
        return err::RAFT_ERR_PG_SHUTDOWN;
    }

    /* If response contains term T > currentTerm: set currentTerm = T
       and convert to follower (§5.3) */
    if (raft_get_current_term() < r->term())
    {
        raft_set_current_term(r->term());
        raft_become_follower();
        raft_set_current_leader(-1);
        SPDK_ERRLOG("node %d change from leader to follow\n", raft_get_nodeid());
        process_response(err::RAFT_ERR_NOT_LEADER, node->raft_get_end_idx());
        return err::RAFT_ERR_NOT_LEADER;
    }
    else if (raft_get_current_term() != r->term()){
        SPDK_WARNLOG("current term %ld:  res term: %ld\n", raft_get_current_term(), r->term());
        return 1;
    }

    if(r->success() == err::RAFT_ERR_LOG_NOT_MATCH){
        node->raft_node_set_next_idx(r->current_idx() + 1);
        dispatch_recovery(node);
        return 1;
    }
    

    node->raft_node_set_lease(r->lease());

    raft_index_t match_idx = node->raft_node_get_match_idx();

    if (0 == r->success())
    {
        /* If AppendEntries fails because of log inconsistency:
           decrement nextIndex and retry (§5.3) */
        
        raft_index_t next_idx = node->raft_node_get_next_idx();
        if(node->raft_node_is_recovering()){
            // SPDK_WARNLOG("node %d is recovering, match_idx: %ld next_idx: %ld r->current_idx: %ld\n", 
            //   node->raft_node_get_id(), match_idx, next_idx, r->current_idx());

            if(next_idx > r->current_idx()){
                node->raft_node_set_next_idx(r->current_idx() + 1);
            }
            dispatch_recovery(node);
            return 1;
        }else{
            node->raft_node_set_next_idx(r->current_idx() + 1);
        }

        if(r->first_idx() - 1 > r->current_idx()){
            //should recovery
            dispatch_recovery(node);
        }else{
            process_response(err::RAFT_ERR_UNKNOWN, node->raft_get_end_idx());
        }
        return 1;
    }


    if (!node->raft_node_is_voting() &&
        !raft_voting_change_is_in_progress() &&
        raft_get_current_idx() <= r->current_idx() + 1 &&
        raft_get_cbs().node_has_sufficient_logs &&
        0 == node->raft_node_has_sufficient_logs()
        )
    {
        int e = raft_get_cbs().node_has_sufficient_logs(this, raft_get_udata(), node);
        if (0 == e)
            node->raft_node_set_has_sufficient_logs();
    }

    if (r->current_idx() <= match_idx && !node->raft_node_is_recovering())
        return 0;

    assert(r->current_idx() <= raft_get_current_idx());

    node->raft_node_set_next_idx(r->current_idx() + 1);
    node->raft_node_set_match_idx(r->current_idx());
    if(node->raft_node_is_recovering()){
        SPDK_INFOLOG(pg_group, "recovery idx [%ld - %ld] to node %d success. current idx %ld\n",
                r->first_idx(), r->current_idx(), node->raft_node_get_id(), raft_get_current_idx());
        dispatch_recovery(node);
        return 0;
    }

    if(is_heartbeat){
        return 0;
    }

    auto cur_idx = raft_get_current_idx();
    process_response(0, r->current_idx());

    /* Aggressively send remaining entries */
    if (node->raft_node_get_next_idx() <= cur_idx){
        SPDK_INFOLOG(pg_group, "node: %d next_idx: %ld cur_idx: %ld\n", node->raft_node_get_id(), node->raft_node_get_next_idx(), cur_idx);
        dispatch_recovery(node);
    }

    return 0;
}

void raft_server_t::follow_raft_disk_append_finish(raft_index_t start_idx, raft_index_t end_idx, raft_index_t _commit_idx, int result){
    if (raft_get_commit_idx() < _commit_idx)
        raft_set_commit_idx(_commit_idx);  
    follow_raft_write_entry_finish(start_idx, end_idx, result);  
}

struct follow_disk_append_complete : public context{
    follow_disk_append_complete(raft_index_t _start_idx, raft_index_t _end_idx,
    raft_index_t _commit_idx, raft_server_t* _raft, msg_appendentries_response_t *_rsp)
    : start_idx(_start_idx)
    , end_idx(_end_idx)
    , commit_idx(_commit_idx)
    , raft(_raft)
    , rsp(_rsp) {}

    void finish(int r) override {
        SPDK_INFOLOG(pg_group, "follow_disk_append_complete finish, start_idx: %ld end_idx: %ld commit_idx %ld result: %d.\n", 
                start_idx, end_idx, commit_idx, r);
        if(r != 0){
            SPDK_ERRLOG("follow_disk_append_complete, result: %d\n", r);
            rsp->set_success(r);
        }
        raft->follow_raft_disk_append_finish(start_idx, end_idx, commit_idx, r);
    }
    raft_index_t start_idx;
    raft_index_t end_idx;
    raft_index_t commit_idx;
    raft_server_t* raft;
    msg_appendentries_response_t *rsp;
};

int raft_server_t::raft_recv_appendentries(
    raft_node_id_t node_id,
    const msg_appendentries_t* ae,
    msg_appendentries_response_t *r,
    context* complete
    )
{
    int e = 0;
    int k = 0;
    raft_time_t election_timer1;
    int i;
    std::vector<std::pair<std::shared_ptr<raft_entry_t>, context*>> entrys;
    int entries_num = ae->entries_size();
    raft_index_t start_idx;
    raft_index_t end_idx;
    follow_disk_append_complete *append_complete;
    raft_index_t new_commit_idx = 0; 

    if (0 < entries_num)
        SPDK_INFOLOG(pg_group, "recvd appendentries from node %d pg: %lu.%lu current_term: %ld request_term:%ld current_idx:%ld \
              request leader_commit:%ld request prev_log_idx:%ld request prev_log_term:%ld entry_num:%d\n",
              ae->node_id(),
              ae->pool_id(), ae->pg_id(),
              raft_get_current_term(),
              ae->term(),
              raft_get_current_idx(),
              ae->leader_commit(),
              ae->prev_log_idx(),
              ae->prev_log_term(),
              entries_num);

    r->set_node_id(raft_get_nodeid());
    r->set_success(0);

    if (raft_is_candidate() && raft_get_current_term() == ae->term())
    {
        raft_become_follower();
    }
    else if (raft_get_current_term() < ae->term())
    {
        e = raft_set_current_term(ae->term());
        if (0 != e)
            goto out;
        raft_become_follower();
    }
    else if (ae->term() < raft_get_current_term())
    {
        /* 1. Reply false if term < currentTerm (§5.1) */
        SPDK_INFOLOG(pg_group, "AE from %d term %ld is less than current term %ld\n",
              ae->node_id(), ae->term(), raft_get_current_term());
        goto out;
    }

    /* update current leader because ae->term is up to date */
    raft_set_current_leader(node_id);

    election_timer1 = get_time();
    raft_set_election_timer(election_timer1);
    r->set_lease(election_timer1 + election_timeout);

    /* Not the first appendentries we've received */
    /* NOTE: the log starts at 1 */
    if (0 < ae->prev_log_idx())
    {
        /* 2. Reply false if log doesn't contain an entry at prevLogIndex
           whose term matches prevLogTerm (§5.3) */
        raft_term_t term;

        bool got = raft_get_entry_term(ae->prev_log_idx(), term);
        if (!got && raft_get_current_idx() < ae->prev_log_idx())
        {
            //follower滞后leader，需要recovery
            SPDK_WARNLOG("AE from %d no log at prev_idx %ld , current idx %ld\n", ae->node_id(), ae->prev_log_idx(), raft_get_current_idx());
            goto out;
        }
        else if (got && term != ae->prev_log_term())
        { 
            SPDK_WARNLOG("AE term doesn't match prev_term (ie. %ld vs %ld) ci:%ld comi:%ld lcomi:%ld pli:%ld \n",
                  term, ae->prev_log_term(), raft_get_current_idx(),
                  raft_get_commit_idx(), ae->leader_commit(), ae->prev_log_idx());
            if (ae->prev_log_idx() <= raft_get_commit_idx())
            {
                /* Should never happen; something is seriously wrong! */
                SPDK_WARNLOG("AE prev conflicts with committed entry\n");
                e = err::RAFT_ERR_SHUTDOWN;
                r->set_success(e);
                goto out;
            }
            /* Delete all the following log entries because they don't match */
            e = raft_truncate_from_idx(ae->prev_log_idx());
            r->set_success(err::RAFT_ERR_LOG_NOT_MATCH);
            goto out;
        }
    }

    r->set_success(1);
    r->set_current_idx(ae->prev_log_idx());

    /* 3. If an existing entry conflicts with a new one (same index
       but different terms), delete the existing entry and all that
       follow it (§5.3) */
    for (i = 0; i < entries_num; i++)
    {
        const raft_entry_t& ety = ae->entries(i);
        raft_index_t ety_index = ae->prev_log_idx() + 1 + i;
        raft_term_t term;
        auto got = raft_get_entry_term(ety_index, term);
        if (got && term != ety.term())
        {
            if (ety_index <= raft_get_commit_idx())
            {
                /* Should never happen; something is seriously wrong! */
                SPDK_WARNLOG("AE entry conflicts with committed entry ci:%ld comi:%ld lcomi:%ld pli:%ld \n",
                      raft_get_current_idx(), raft_get_commit_idx(),
                      ae->leader_commit(), ae->prev_log_idx());
                e = err::RAFT_ERR_SHUTDOWN;
                r->set_success(e);
                goto out;
            }
            e = raft_truncate_from_idx(ety_index);
            if (0 != e)
                goto out;
            current_idx = ety_index - 1;
            break;
        }
        else if (!got && raft_get_current_idx() < ety_index)
            break;
        r->set_current_idx(ety_index);
    }

    /* 4. Append any new entries not already in the log */
    k = entries_num - i;
    for(auto j = i; j < entries_num; j++){
        const raft_entry_t& ety = ae->entries(j);

        std::shared_ptr<raft_entry_t> ety_ptr = std::make_shared<raft_entry_t>(std::move(ety));
        if(j == entries_num - 1){
            entrys.emplace_back(std::make_pair(std::move(ety_ptr), complete));
        }else{
            entrys.emplace_back(std::make_pair(std::move(ety_ptr), nullptr));
        }
    }
    start_idx = ae->prev_log_idx() + 1 + i;
    end_idx =  start_idx + k - 1;
    SPDK_INFOLOG(pg_group, "start_idx: %ld  end_idx: %ld \n", start_idx, end_idx);
    e = raft_append_entries(entrys);
    i += k;
    r->set_current_idx(ae->prev_log_idx() + i);
    if (0 != e)
        goto out;

    /* 5. If leaderCommit > commitIndex, set commitIndex =
        min(leaderCommit, index of last new entry) */
    if (raft_get_commit_idx() < ae->leader_commit())
    {
        new_commit_idx = std::min(ae->leader_commit(), r->current_idx());
    }
    
    r->set_term(raft_get_current_term());
    r->set_first_idx(ae->prev_log_idx() + 1);
    if(start_idx > end_idx){
        //空的append entry request，既心跳包
        complete->complete(0);
        return 0;
    }
    current_idx = end_idx;
    append_complete = new follow_disk_append_complete(start_idx, end_idx, new_commit_idx, this, r);
    raft_disk_append_entries(start_idx, end_idx, append_complete);
    return 0;

out:
    r->set_term(raft_get_current_term());
    if (1 != r->success())
        r->set_current_idx(raft_get_current_idx());
    r->set_first_idx(ae->prev_log_idx() + 1);
    return e;
}

int raft_server_t::_should_grant_vote(const msg_requestvote_t* vr)
{
    /* For a prevote, we could theoretically proceed to the votedFor check
     * below, if vr->term == currentTerm - 1. That, however, would only matter
     * if we had rejected a previous RequestVote from a third server, who must
     * have already won a prevote phase. Hence, we choose not to look into
     * votedFor for simplicity. */
    if (vr->term() < raft_get_current_term())
        return 0;

    if (!vr->prevote() && raft_get_voted_for() != -1 && raft_get_voted_for() != vr->candidate_id())
        return 0;

    /* Below we check if log is more up-to-date... */

    raft_index_t current_idx_tmp = raft_get_current_idx();

    raft_term_t term;
    auto got = raft_get_entry_term(current_idx_tmp, term);
    assert(got);
    (void)got;
    if (term < vr->last_log_term())
        return 1;

    if (vr->last_log_term() == term && current_idx_tmp <= vr->last_log_idx())
        return 1;

    return 0;
}

int raft_server_t::raft_recv_requestvote(raft_node_id_t node_id,
                          const msg_requestvote_t* vr,
                          msg_requestvote_response_t *r)
{
    raft_time_t now = get_time();
    int e = 0;

    SPDK_INFOLOG(pg_group, "raft_recv_requestvote from node %d pg %lu.%lu term %ld current_term %ld candidate_id %d \
            last_log_idx %ld last_log_term %ld prevote %d\n", 
            node_id, vr->pool_id(), vr->pg_id(),
            vr->term(), raft_get_current_term(), vr->candidate_id(), 
            vr->last_log_idx(), vr->last_log_term(), vr->prevote());
    raft_node* node = raft_get_node(node_id);
    if (!node)
        node = raft_get_node(vr->candidate_id());

    r->set_node_id(raft_get_nodeid());
    /* Reject request if we have a leader or if we have just started (for we might
     * have granted a lease before a restart) */
    if (raft_get_state() == RAFT_STATE_LEADER ||
        (raft_get_current_leader() != -1 && raft_get_current_leader() != vr->candidate_id() &&
         now - raft_get_election_timer() < raft_get_election_timeout()) ||
        (!raft_get_first_start() && now - raft_get_start_time() < raft_get_election_timeout()))
    {
        r->set_vote_granted(0);
        goto done;
    }

    if (raft_get_current_term() < vr->term())
    {
        SPDK_INFOLOG(pg_group, "current_term %ld  request term %ld\n", raft_get_current_term(), vr->term());
        e = raft_set_current_term(vr->term());
        if (0 != e) {
            r->set_vote_granted(0);
            goto done;
        }
        raft_become_follower();
        raft_set_current_leader(-1);
    }

    if (_should_grant_vote(vr))
    {
        SPDK_INFOLOG(pg_group, "should_grant_vote\n");
        /* It shouldn't be possible for a leader or prevoted candidate to grant a vote
         * Both states would have voted for themselves
         * A candidate may grant a prevote though */
        assert(!raft_is_leader() && (!raft_is_candidate() || raft_get_prevote() || vr->prevote()));

        r->set_vote_granted(1);
        if (!vr->prevote())
        {
            e = raft_vote_for_nodeid(vr->candidate_id());
            if (0 != e)
                r->set_vote_granted(0);

            /* there must be in an election. */
            raft_set_current_leader(-1);
            raft_set_election_timer(now);
        }
    }
    else
    {
        /* It's possible the candidate node has been removed from the cluster but
         * hasn't received the appendentries that confirms the removal. Therefore
         * the node is partitioned and still thinks its part of the cluster. It
         * will eventually send a requestvote. This is error response tells the
         * node that it might be removed. */
        if (!node)
        {
            r->set_vote_granted(RAFT_REQUESTVOTE_ERR_UNKNOWN_NODE);
            goto done;
        }
        else
            r->set_vote_granted(0);
    }

done:
    SPDK_INFOLOG(pg_group, "node requested vote%s: %d replying: %s \n",
          vr->prevote() ? " (prevote)" : "",
          node == nullptr ? -1 : node->raft_node_get_id(),
          r->vote_granted() == 1 ? "granted" :
          r->vote_granted() == 0 ? "not granted" : "unknown");

    r->set_term(raft_get_current_term());
    r->set_prevote(vr->prevote());
    return e;
}

int raft_votes_is_majority(const int num_nodes, const int nvotes)
{
    if (num_nodes < nvotes)
        return 0;
    int half = num_nodes / 2;
    return half + 1 <= nvotes;
}

int raft_server_t::raft_process_requestvote_reply(
                                   msg_requestvote_response_t* r)
{
    SPDK_INFOLOG(pg_group, "node responded to requestvote%s for pg %lu.%lu from node %d status:%s current_term:%ld rsp term:%ld \n",
          r->prevote() ? " (prevote)" : "", 
          raft_get_pool_id(), raft_get_pg_id(),
          r->node_id(),
          r->vote_granted() == 1 ? "granted" :
          r->vote_granted() == 0 ? "not granted" : "unknown",
          raft_get_current_term(),
          r->term());
    raft_node* node = raft_get_node(r->node_id());

    if (!raft_is_candidate() || raft_get_prevote() != r->prevote())
    {
        return 0;
    }
    else if (raft_get_current_term() < r->term())
    {
        int e = raft_set_current_term(r->term());
        if (0 != e)
            return e;
        raft_become_follower();
        raft_set_current_leader(-1);
        return 0;
    }
    else if (raft_get_current_term() != r->term())
    {
        /* The node who voted for us would have obtained our term.
         * Therefore this is an old message we should ignore.
         * This happens if the network is pretty choppy. */
        return 0;
    }

    switch (r->vote_granted())
    {
        case RAFT_REQUESTVOTE_ERR_GRANTED:
            if (node)
                node->raft_node_vote_for_me(1);
            return raft_count_votes();

        case RAFT_REQUESTVOTE_ERR_NOT_GRANTED:
            break;

        case RAFT_REQUESTVOTE_ERR_UNKNOWN_NODE:
            if (raft_get_my_node()->raft_node_is_voting() &&
                raft_is_connected() == RAFT_NODE_STATUS_DISCONNECTING)
                return err::RAFT_ERR_SHUTDOWN;
            break;

        default:
            assert(0);
    }

    return 0;
}

int raft_server_t::raft_recv_installsnapshot(raft_node_id_t node_id,
                              const msg_installsnapshot_t* is,
                              msg_installsnapshot_response_t* r,
                              context* complete)
{
    int e;
    raft_node* node = raft_get_node(node_id);

    r->set_node_id(raft_get_nodeid());
    r->set_term(raft_get_current_term());
    
    r->set_last_idx(is->last_idx());
    r->set_complete(0);

    if (is->term() < raft_get_current_term())
        return 0;

    if (raft_get_current_term() < is->term())
    {
        e = raft_set_current_term(is->term());
        if (0 != e)
            return e;
        r->set_term(raft_get_current_term());
    }

    if (!raft_is_follower())
        raft_become_follower();

    raft_set_current_leader(node_id);
    auto election_timer = get_time();
    raft_set_election_timer(election_timer);
    r->set_lease(election_timer + election_timeout);

    if (is->last_idx() <= raft_get_commit_idx())
    {
        /* Committed entries must match the snapshot. */
        r->set_complete(1);
        return 0;
    }

    raft_term_t term;
    auto got = raft_get_entry_term(is->last_idx(), term);
    if (got && term == is->last_term())
    {
        raft_set_commit_idx(is->last_idx());
        r->set_complete(1);
        return 0;
    }

    assert(raft_get_cbs().recv_installsnapshot);
    e = raft_get_cbs().recv_installsnapshot(this, raft_get_udata(), node, is, r);
    if (e < 0)
        return e;

    if (e == 1)
        r->set_complete(1);
    
    //这里应该是固化installsnapshot成功后调用  todo
    complete->complete(0);
    return 0;
}

int raft_server_t::raft_process_installsnapshot_reply(
                                       msg_installsnapshot_response_t *r)
{
    raft_node* node = raft_get_node(r->node_id());
    if (!node)
        return -1;

    if (!raft_is_leader())
        return err::RAFT_ERR_NOT_LEADER;

    if (raft_get_current_term() < r->term())
    {
        int e = raft_set_current_term(r->term());
        if (0 != e)
            return e;
        raft_become_follower();
        raft_set_current_leader(-1);
        return 0;
    }
    else if (raft_get_current_term() != r->term())
        return 0;

    node->raft_node_set_lease(r->lease());

    assert(raft_get_cbs().recv_installsnapshot_response);
    int e = raft_get_cbs().recv_installsnapshot_response(this, raft_get_udata(), node, r);
    if (0 != e)
        return e;

    /* The snapshot installation is complete. Update the node state. */
    if (r->complete() && node->raft_node_get_match_idx() < r->last_idx())
    {
        node->raft_node_set_match_idx(r->last_idx());
        node->raft_node_set_next_idx(r->last_idx() + 1);
    }

    if (node->raft_node_get_next_idx() <= raft_get_current_idx())
        raft_send_appendentries(node);

    return 0;
}

int raft_server_t::_cfg_change_is_valid(raft_entry_t* ety)
{
    /* A membership change to a leader is either nonsense or dangerous
       (e.g., we append the entry locally and count voting nodes below
       without checking if ourself remains a voting node). */
    raft_node_id_t node_id = raft_get_cbs().log_get_node_id(this, udata, ety, 0);
    if (node_id == raft_get_nodeid())
        return 0;

    raft_node* node = raft_get_node(node_id);
    switch (ety->type())
    {
        case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
        case RAFT_LOGTYPE_ADD_NODE:
            if (node)
                return 0;
            break;

        case RAFT_LOGTYPE_DEMOTE_NODE:
        case RAFT_LOGTYPE_REMOVE_NODE:
            if (!node || !node->raft_node_is_voting())
                return 0;
            break;

        case RAFT_LOGTYPE_PROMOTE_NODE:
        case RAFT_LOGTYPE_REMOVE_NONVOTING_NODE:
            if (!node || node->raft_node_is_voting())
                return 0;
            break;

        default:
            assert(0);
    }

    return 1;
}

void raft_server_t::raft_write_entry_finish(raft_index_t start_idx, raft_index_t end_idx, int result){
    SPDK_INFOLOG(pg_group, "raft_write_entry_finish, [%ld-%ld] commit: %ld result: %d\n", 
            start_idx, end_idx, raft_get_commit_idx(), result);
    raft_get_log()->raft_write_entry_finish(start_idx, end_idx, result);
    raft_flush();
}

void raft_server_t::follow_raft_write_entry_finish(raft_index_t start_idx, raft_index_t end_idx, int result){
    raft_get_log()->raft_write_entry_finish(start_idx, end_idx, result);
}

void raft_server_t::raft_disk_append_finish(raft_index_t start_idx, raft_index_t end_idx, int result){
    int votes = 0;
    for(auto _node : nodes){
        raft_node* tmpnode = _node.get();
        //update match idx of leader
        if(raft_is_self(tmpnode)){
            tmpnode->raft_node_set_match_idx(end_idx);
        }

        if(raft_get_commit_idx() < end_idx){
            if(tmpnode->raft_node_is_voting() && end_idx <= tmpnode->raft_node_get_match_idx()){
                votes++;
            }
        }
    }
    if((raft_get_commit_idx() < end_idx) && (raft_get_num_voting_nodes() / 2 < votes)){
        raft_set_commit_idx(end_idx);
        raft_flush();
    }
}

struct disk_append_complete : public context{
    disk_append_complete(raft_index_t _start_idx, raft_index_t _end_idx, raft_server_t* _raft)
    : start_idx(_start_idx)
    , end_idx(_end_idx)
    , raft(_raft) {}

    void finish(int r) override {
        SPDK_INFOLOG(pg_group, "disk_append_complete, start_idx: %ld end_idx: %ld result: %d\n", 
                start_idx, end_idx, r);
        if(r != 0)
            SPDK_ERRLOG("disk_append_complete, result: %d\n", r);
        raft->raft_disk_append_finish(start_idx, end_idx, r);
    }
    raft_index_t start_idx;
    raft_index_t end_idx;
    raft_server_t* raft;
};

int raft_server_t::raft_write_entry(std::shared_ptr<raft_entry_t> ety,
                    context *complete)
{
    auto ety_ptr = ety.get();
    if (!raft_is_leader())
        return err::RAFT_ERR_NOT_LEADER;

    if (raft_entry_is_cfg_change(ety_ptr))
    {
        /* Multi-threading: need to fail here because user might be
         * snapshotting membership settings. */
        if (raft_get_snapshot_in_progress())
            return err::RAFT_ERR_SNAPSHOT_IN_PROGRESS;

        /* Only one voting cfg change at a time */
        if (raft_entry_is_voting_cfg_change(ety_ptr) &&
            raft_voting_change_is_in_progress())
                return err::RAFT_ERR_ONE_VOTING_CHANGE_ONLY;

        if (!_cfg_change_is_valid(ety_ptr))
            return err::RAFT_ERR_INVALID_CFG_CHANGE;
    }

    ety->set_term(raft_get_current_term());
    std::vector<std::pair<std::shared_ptr<raft_entry_t>, context*>> entrys;
    entrys.push_back(std::make_pair(ety, complete));
    int e = raft_append_entries(entrys);
    if (0 != e)
        return e;

    if (raft_entry_is_voting_cfg_change(ety_ptr))
        raft_set_voting_cfg_change_log_idx(ety->idx());

    if(current_idx > commit_idx){
        return 0;
    }
    raft_flush();
    return 0;
}

void raft_server_t::stop_flush(int state){
    auto last_cache_idx = raft_get_last_cache_entry();
    SPDK_INFOLOG(pg_group, "delete entrys [%lu, %lu]\n", current_idx + 1, last_cache_idx);
    if(last_cache_idx <= current_idx){
        return;
    }
    
    raft_get_log()->raft_write_entry_finish(current_idx + 1, last_cache_idx, state);
    raft_get_log()->remove_entry_between(current_idx + 1, last_cache_idx);
}

void raft_server_t::raft_flush(){
    //上一次的log已经commit了
    auto last_cache_idx = raft_get_last_cache_entry();
    if(!raft_is_leader()){
        SPDK_ERRLOG("not leader\n");
        stop_flush(err::RAFT_ERR_NOT_LEADER);
        return;
    }
    if(last_cache_idx == current_idx){
        return;
    }
    first_idx = current_idx + 1;
    current_idx = last_cache_idx;
    SPDK_INFOLOG(pg_group, "------ first_idx: %lu current_idx: %lu ------\n", first_idx, current_idx);

    for(auto _node : nodes)
    {
        raft_node* node = _node.get();

        if (!node || raft_is_self(node) ||
            !node->raft_node_is_voting())
            continue;

        
        /* Only send new entries.
         * Don't send the entry to peers who are behind, to prevent them from
         * becoming congested. */
        raft_index_t next_idx = node->raft_node_get_next_idx();
        if(!node->raft_node_is_recovering() && next_idx == first_idx){
            SPDK_INFOLOG(pg_group, "send to node %d next_idx: %ld current_idx: %ld\n", 
                    node->raft_node_get_id(), next_idx, current_idx);
            node->raft_set_end_idx(current_idx);
            raft_send_appendentries(node);
        }
        else{
            if(node->raft_node_is_recovering())
                SPDK_INFOLOG(pg_group, "node %d is recovering\n", node->raft_node_get_id());
            else
                SPDK_INFOLOG(pg_group, "node %d is fall behind,  next_idx: %ld first_idx: %ld \n", 
                    node->raft_node_get_id(), next_idx, first_idx);
        }             
    }

    disk_append_complete *append_complete = new disk_append_complete(first_idx, current_idx, this);
    raft_disk_append_entries(first_idx, current_idx, append_complete);    
}

int raft_server_t::raft_send_requestvote(raft_node* node)
{
    msg_requestvote_t* rv = new msg_requestvote_t();

    assert(node);
    assert(!raft_is_self(node));

    SPDK_INFOLOG(pg_group, "sending requestvote%s  term: %ld to: %d , pool.pg %lu.%lu\n",
          raft_get_prevote() ? " (prevote)" : "", raft_get_current_term(), node->raft_node_get_id(), pool_id, pg_id);

    rv->set_node_id(raft_get_nodeid());
    rv->set_pool_id(pool_id);
    rv->set_pg_id(pg_id);    
    rv->set_term(raft_get_current_term());
    rv->set_last_log_idx(raft_get_current_idx());

    raft_term_t _term = 0;
    auto got = raft_get_entry_term(raft_get_current_idx(),  _term);  
    assert(got);
    (void)got;   
    rv->set_last_log_term(_term);
    rv->set_candidate_id(raft_get_nodeid());
    rv->set_prevote(raft_get_prevote());
    client.send_vote(this, node->raft_node_get_id(), rv);

    return 0;
}

int raft_server_t::_raft_send_installsnapshot(raft_node* node)
{
    msg_installsnapshot_t* is = new msg_installsnapshot_t();
    is->set_node_id(raft_get_nodeid());
    is->set_pool_id(pool_id);
    is->set_pg_id(pg_id);
    is->set_term(raft_get_current_term());
    is->set_last_idx(raft_get_log()->log_get_base());
    is->set_last_term(raft_get_log()->log_get_base_term());

    SPDK_INFOLOG(pg_group, "sending installsnapshot: ci:%ld comi:%ld t:%ld lli:%ld llt:%ld \n",
          raft_get_current_idx(),
          raft_get_commit_idx(),
          is->term(),
          is->last_idx(),
          is->last_term());

    client.send_install_snapshot(this, node->raft_node_get_id(), is);
    return 0;
}

void raft_server_t::_raft_get_entries_from_idx(raft_index_t idx, msg_appendentries_t* ae)
{
    std::vector<std::shared_ptr<raft_entry_t>> entrys;
    raft_get_log()->log_get_from_idx(idx, entrys);
    for(auto entry : entrys){
        auto entry_ptr = ae->add_entries();
        *entry_ptr = *entry;
    }
}

int raft_server_t::raft_send_heartbeat(raft_node* node)
{
    assert(node);
    assert(!raft_is_self(node));

    msg_appendentries_t* ae = new msg_appendentries_t();
    ae->set_node_id(raft_get_nodeid());
    ae->set_pool_id(pool_id);
    ae->set_pg_id(pg_id);
    ae->set_term(raft_get_current_term());
    raft_index_t next_idx = node->raft_node_get_next_idx();  
    ae->set_prev_log_idx(next_idx - 1);  
    raft_term_t term = 0;
    auto got = raft_get_entry_term(ae->prev_log_idx(), term);
    assert(got);
    (void)got;
    ae->set_prev_log_term(term);
    ae->set_leader_commit(raft_get_commit_idx());

    SPDK_INFOLOG(pg_group, "sending heartbeat appendentries to node %d: ci:%ld comi:%ld t:%ld lc:%ld pli:%ld plt:%ld \n",
          node->raft_node_get_id(),
          raft_get_current_idx(),
          raft_get_commit_idx(),
          ae->term(),
          ae->leader_commit(),
          ae->prev_log_idx(),
          ae->prev_log_term());


    client.send_appendentries(this, node->raft_node_get_id(), ae);
    return 0;
}

msg_appendentries_t* raft_server_t::create_appendentries(raft_node* node)
{
    assert(node);
    assert(!raft_is_self(node));
    node->raft_set_suppress_heartbeats(true);

    msg_appendentries_t* ae = new msg_appendentries_t();
    ae->set_node_id(raft_get_nodeid());
    ae->set_pool_id(pool_id);
    ae->set_pg_id(pg_id);
    ae->set_term(raft_get_current_term());
    ae->set_leader_commit(raft_get_commit_idx());

    raft_index_t next_idx = node->raft_node_get_next_idx();

    ae->set_prev_log_idx(next_idx - 1);
    raft_term_t term = 0;
    auto got = raft_get_entry_term(ae->prev_log_idx(), term);
    assert(got);
    (void)got;

    ae->set_prev_log_term(term);

    SPDK_INFOLOG(pg_group, "sending appendentries node %d: next_idx: %ld ci:%ld comi:%ld t:%ld lc:%ld pli:%ld plt:%ld \n",
          node->raft_node_get_id(),  next_idx, raft_get_current_idx(),
          raft_get_commit_idx(),
          ae->term(),
          ae->leader_commit(),
          ae->prev_log_idx(),
          ae->prev_log_term());
    auto cur_timer = get_time();
    node->raft_set_append_time(cur_timer); 
    raft_set_election_timer(cur_timer);  
    return ae;
}

int raft_server_t::raft_send_appendentries(raft_node* node)
{
    msg_appendentries_t*ae = create_appendentries(node);
    raft_index_t next_idx = node->raft_node_get_next_idx();

    _raft_get_entries_from_idx(next_idx, ae);

    client.send_appendentries(this, node->raft_node_get_id(), ae);
    return 0;
}

int raft_server_t::raft_send_heartbeat_all()
{
    int e;

    auto election_timer = get_time();
    raft_set_election_timer(election_timer);
    for(auto _node : nodes)
    {
        raft_node* node = _node.get();
        if (raft_is_self(node))
            continue;

        if(_node->raft_get_suppress_heartbeats())
            continue;
        e = raft_send_heartbeat(node);
        if (0 != e)
            return e;
    }

    return 0;
}

raft_node* raft_server_t::raft_add_node_internal(raft_entry_t *ety, void* udata, raft_node_id_t id, bool is_self)
{
    /* we shouldn't add a node twice */
    raft_node* _node = raft_get_node(id);
    if (_node)
        return NULL;

    auto node = std::make_shared<raft_node>(udata, id);
    if (raft_is_leader())
        node->raft_node_set_effective_time(get_time());

    nodes.push_back(node);
    if (is_self)
        raft_set_nodeid(id);
    if (raft_get_cbs().notify_membership_event)
        raft_get_cbs().notify_membership_event(this, raft_get_udata(), node.get(), ety, RAFT_MEMBERSHIP_ADD);    
    return node.get();
}

raft_node* raft_server_t::raft_add_non_voting_node_internal(raft_entry_t *ety, void* udata, raft_node_id_t id, bool is_self)
{
    raft_node* node = raft_add_node_internal(ety, udata, id, is_self);
    if (!node)
        return NULL;

    node->raft_node_set_voting(0);
    return node;
}

void raft_server_t::raft_remove_node(raft_node* node)
{
    assert(node);

    if (raft_get_cbs().notify_membership_event)
        raft_get_cbs().notify_membership_event(this, udata, node, NULL, RAFT_MEMBERSHIP_REMOVE);

    int found = 0;
    
    auto it = nodes.begin();
    while(it != nodes.end()){
        if(it->get() == node){
            found = 1;
            break;
        }
        it++;
    }
    assert(found);
    (void)found;
    nodes.erase(it);
}

void raft_server_t::raft_destroy_nodes()
{
    auto it = nodes.begin();
    while(it != nodes.end()){
        raft_node* node = it->get();
        if (raft_get_cbs().notify_membership_event)
            raft_get_cbs().notify_membership_event(this, udata, node, NULL, RAFT_MEMBERSHIP_REMOVE);
        it = nodes.erase(it);
    }
}

void raft_server_t::stop(){
    SPDK_INFOLOG(pg_group, "stop pg %lu.%lu\n", pool_id, pg_id);
    stop_timed_task();
    spdk_poller_unregister(&raft_timer);
    stop_flush(err::RAFT_ERR_PG_SHUTDOWN);
}

void raft_server_t::raft_destroy()
{
    //可能还需要其它处理 ？ todo
    stop();
    raft_destroy_nodes();
    machine.reset();
    log->destroy_log();
}

int raft_server_t::raft_get_nvotes_for_me()
{
    int votes = 0;

    for(auto node : nodes)
    {
        if (node->raft_node_is_voting() &&
            node->raft_node_has_vote_for_me())
        {
            votes += 1;
        }
    }

    return votes;
}

int raft_server_t::raft_vote_for_nodeid(const raft_node_id_t nodeid)
{
    int ret = save_vote_for(nodeid);
    if(0 != ret)
        return ret;
    raft_set_voted_for(nodeid);
    return 0;
}

int raft_entry_is_voting_cfg_change(raft_entry_t* ety)
{
    return RAFT_LOGTYPE_ADD_NODE == ety->type() ||
           RAFT_LOGTYPE_PROMOTE_NODE == ety->type() ||
           RAFT_LOGTYPE_DEMOTE_NODE == ety->type() ||
           RAFT_LOGTYPE_REMOVE_NODE == ety->type();
}

int raft_entry_is_cfg_change(raft_entry_t* ety)
{
    return RAFT_LOGTYPE_ADD_NODE == ety->type() ||
           RAFT_LOGTYPE_ADD_NONVOTING_NODE == ety->type() ||
           RAFT_LOGTYPE_PROMOTE_NODE == ety->type() ||
           RAFT_LOGTYPE_DEMOTE_NODE == ety->type() ||
           RAFT_LOGTYPE_REMOVE_NONVOTING_NODE == ety->type() ||
           RAFT_LOGTYPE_REMOVE_NODE == ety->type();
}



void raft_server_t::raft_offer_log(std::vector<std::shared_ptr<raft_entry_t>>& entries,
                     raft_index_t idx)
{
    int i;
    int n_entries = entries.size();

    for (i = 0; i < n_entries; i++)
    {
        auto ety_ptr = entries[i];
        raft_entry_t *ety = ety_ptr.get();

        if (!raft_entry_is_cfg_change(ety))
            continue;

        if (raft_entry_is_voting_cfg_change(ety))
            raft_set_voting_cfg_change_log_idx(idx + i);

        raft_node_id_t node_id = raft_get_cbs().log_get_node_id(this, udata,
                                                        ety, idx + i);
        raft_node* node = raft_get_node(node_id);
        bool is_self = node_id == raft_get_nodeid();

        switch (ety->type())
        {
            case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
                assert(!node);
                node = raft_add_non_voting_node_internal(ety, NULL, node_id, is_self);
                assert(node);
                break;

            case RAFT_LOGTYPE_ADD_NODE:
                assert(!node);
                node = raft_add_node_internal(ety, NULL, node_id, is_self);
                assert(node);
                break;

            case RAFT_LOGTYPE_PROMOTE_NODE:
                assert(node && !node->raft_node_is_voting());
                node->raft_node_set_voting(1);
                break;

            case RAFT_LOGTYPE_DEMOTE_NODE:
                assert(node && node->raft_node_is_voting());
                node->raft_node_set_voting(0);
                break;

            case RAFT_LOGTYPE_REMOVE_NODE:
                assert(node && node->raft_node_is_voting());
                raft_remove_node(node);
                break;

            case RAFT_LOGTYPE_REMOVE_NONVOTING_NODE:
                assert(node && !node->raft_node_is_voting());
                raft_remove_node(node);
                break;

            default:
                assert(0);
        }
    }
}

void raft_pop_log(void *arg, raft_index_t idx, std::shared_ptr<raft_entry_t> entry)
{
    raft_server_t* me_ = (raft_server_t*)arg;

    if (!raft_entry_is_cfg_change(entry.get()))
        return;   

    if (idx <= me_->raft_get_voting_cfg_change_log_idx())
        me_->raft_set_voting_cfg_change_log_idx(-1);

    raft_node_id_t node_id = me_->raft_get_cbs().log_get_node_id(me_, me_->raft_get_udata(),
                                                        entry.get(), idx);    
    
    raft_node* node = me_->raft_get_node(node_id);
    bool is_self = node_id == me_->raft_get_nodeid();

    switch (entry->type())
    {
        case RAFT_LOGTYPE_DEMOTE_NODE:
            assert(node && !node->raft_node_is_voting());
            node->raft_node_set_voting(1);
            break;
        case RAFT_LOGTYPE_REMOVE_NODE:
            assert(!node);
            node = me_->raft_add_node_internal(entry.get(), NULL, node_id, is_self);
            assert(node);
            break;
        case RAFT_LOGTYPE_REMOVE_NONVOTING_NODE:
            assert(!node);
            node = me_->raft_add_non_voting_node_internal(entry.get(), NULL, node_id, is_self);
            assert(node);
            break;
        case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
            assert(node && !node->raft_node_is_voting());
            me_->raft_remove_node(node);
            break;
        case RAFT_LOGTYPE_ADD_NODE:
            assert(node && node->raft_node_is_voting());
            me_->raft_remove_node(node);
            break;
        case RAFT_LOGTYPE_PROMOTE_NODE:
            assert(node && node->raft_node_is_voting());
            node->raft_node_set_voting(0);
            break;
        default:
            assert(0);
    }
}

raft_index_t raft_server_t::raft_get_num_snapshottable_logs()
{
    assert(raft_get_log()->log_get_base() <= raft_get_commit_idx());
    return raft_get_commit_idx() - raft_get_log()->log_get_base();
}

/** Raft callback for handling periodic logic */
static int periodic_func(void* arg){
    raft_server_t* raft = (raft_server_t*)arg;
    raft->raft_periodic();
    return 0;
}

void raft_server_t::start_raft_timer(){
    raft_timer = SPDK_POLLER_REGISTER(periodic_func, this, TIMER_PERIOD_MSEC * 1000);
	raft_set_election_timeout(ELECTION_TIMER_PERIOD_MSEC);
    raft_set_lease_maintenance_grace(LEASE_MAINTENANCE_GRACE);
    raft_set_heartbeat_timeout(HEARTBEAT_TIMER_PERIOD_MSEC);
    start_timed_task();
}

void raft_server_t::do_recovery(raft_node* node){
    raft_index_t next_idx = node->raft_node_get_next_idx();
    if (next_idx <= raft_get_log()->log_get_base()){
        SPDK_INFOLOG(pg_group, "node %d  next_idx %ld  raft base log: %ld\n", 
                node->raft_node_get_id(), next_idx, raft_get_log()->log_get_base());
        _raft_send_installsnapshot(node);
        return;
    }
    
    auto send_recovery_entries = [this, node](std::vector<raft_entry_t>&& entries){
        msg_appendentries_t*ae = create_appendentries(node);
        for(auto &entry : entries){
            auto entry_ptr = ae->add_entries();
            *entry_ptr = std::move(entry);
        }
        client.send_appendentries(this, node->raft_node_get_id(), ae);
    };

    auto first_idx_cache = raft_get_log()->first_log_in_cache();
    if(next_idx >= first_idx_cache){
        std::vector<std::shared_ptr<raft_entry_t>> entries;
        long entry_num = std::min(recovery_max_entry_num, raft_get_current_idx() - next_idx + 1);
        raft_get_log()->log_get_from_idx(next_idx, entry_num, entries);
        msg_appendentries_t*ae = create_appendentries(node);
        SPDK_DEBUGLOG(pg_group, "read %ld entry first: %ld from cache for recovery to node %d. entry_num: %ld\n", 
                entries.size(), next_idx, node->raft_node_get_id(), entry_num);
        for(auto entry : entries){
            auto entry_ptr = ae->add_entries();
            *entry_ptr = *entry;
        }
        client.send_appendentries(this, node->raft_node_get_id(), ae);
        return;
    }

    auto end_idx = std::min(raft_get_current_idx(), first_idx_cache - 1);
    long entry_num = std::min(recovery_max_entry_num, end_idx - next_idx + 1);

    raft_get_log()->disk_read(
      next_idx, 
      next_idx + entry_num - 1, 
      [this, node, send_entries = std::move(send_recovery_entries), next_idx](std::vector<raft_entry_t>&& entries, int rberrno){
        assert(rberrno == 0);
        SPDK_DEBUGLOG(pg_group, "read %ld entry first: %ld from disk log for recovery to node %d\n", 
                entries.size(), next_idx, node->raft_node_get_id());
        send_entries(std::move(entries));
      });
}

void raft_server_t::dispatch_recovery(raft_node* node){
    node->raft_node_set_recovering(true);
    if(!raft_is_leader()){
        node->raft_node_set_recovering(false);
        return;
    }
    
    if(node->raft_node_get_match_idx() == raft_get_current_idx()){
        SPDK_DEBUGLOG(pg_group, "end recovery. node %d match_idx: %ld  raft current idx: %ld\n", 
                node->raft_node_get_id(), node->raft_node_get_match_idx(), raft_get_current_idx());
        node->raft_node_set_recovering(false);
        return;
    }
    do_recovery(node);    
}

raft_server_t::raft_server_t(raft_client_protocol& _client, disk_log* _log, 
        std::shared_ptr<state_machine> sm_ptr, uint64_t _pool_id, uint64_t _pg_id
       , kvstore *_kv     
        )
    : current_term(0)
    , voted_for(-1)
    , commit_idx(0)
    , state(RAFT_STATE_FOLLOWER)
    , prevote(0)
    , start_time(0)
    , election_timer(0)
    , election_timeout(1000)
    , heartbeat_timeout(200)
    , leader_id(-1)
    , node_id(-1)
    , voting_cfg_change_log_idx(-1)
    , connected(0)
    , snapshot_in_progress(0)
    , snapshot_last_idx(0)
    , snapshot_last_term(0)
    , lease_maintenance_grace(0)
    , first_start(0) 
    , machine(sm_ptr)
    , pool_id(_pool_id)
    , pg_id(_pg_id)
    , first_idx(0)
    , current_idx(0)
    , client(_client)
    , stm_in_apply(false)
    , _append_entries_buffer(this)
    , kv(_kv) 
    , _last_index_before_become_leader(0)      
{
        raft_randomize_election_timeout();  
        log = log_new(std::move(_log)); 
        machine->set_raft(this);
}

raft_server_t::~raft_server_t()
{
    log->log_clear();
    nodes.clear();
}

int raft_server_t::raft_set_current_term(const raft_term_t term)
{
    if (current_term < term)
    {
        raft_node_id_t voted_for_local = -1;
        int ret = save_term(term);
        if(ret != 0)
            return ret;
            
        current_term = term;
        voted_for = voted_for_local;
    }
    return 0;
}

std::shared_ptr<raft_entry_t> raft_server_t::raft_get_last_applied_entry()
{
    if (raft_get_last_applied_idx() == 0)
        return nullptr;
    return log->log_get_at_idx(raft_get_last_applied_idx());
}

void raft_server_t::raft_clear()
{
    current_term = 0;
    voted_for = -1;
    election_timer = 0;
    raft_randomize_election_timeout();
    voting_cfg_change_log_idx = -1;
    raft_set_state(RAFT_STATE_FOLLOWER);
    leader_id = -1;
    commit_idx = 0;
    node_id = -1;
    log->log_clear();
    start_time = 0;
    lease_maintenance_grace = 0;
    first_start = 0;
}