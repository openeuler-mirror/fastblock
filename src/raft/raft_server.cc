#include <string.h>
#include <assert.h>

/* for varags */
#include <stdarg.h>

#include "raft.h"
#include "raft_log.h"
#include "raft_private.h"
#include "spdk/log.h"
#include "spdk/env.h"

std::shared_ptr<raft_server_t> raft_new(raft_client_protocol& client,
        storage::log&& log, std::shared_ptr<state_machine> sm_ptr, uint64_t pool_id, uint64_t pg_id)
{
    auto raft = std::make_shared<raft_server_t>(client, std::move(log), sm_ptr, pool_id, pg_id);
    return raft;
}

void raft_server_t::raft_set_callbacks(raft_cbs_t* funcs, void* _udata)
{
    memcpy(&cb, funcs, sizeof(raft_cbs_t));
    raft_set_udata(_udata);
    raft_get_log()->log_set_raft((void *)this);

    /* We couldn't initialize the time fields without the callback. */
    raft_time_t now = cb.get_time();
    raft_set_election_timer(now);
    raft_set_start_time(now);
}

int raft_server_t::raft_delete_entry_from_idx(raft_index_t idx)
{
    assert(raft_get_commit_idx() < idx);

    if (idx <= raft_get_voting_cfg_change_log_idx())
        raft_set_voting_cfg_change_log_idx(-1);

    return raft_get_log()->log_delete(idx);
}

int raft_server_t::raft_election_start()
{
    SPDK_NOTICELOG("election starting: pool.pg %lu.%lu %d %ld, term: %ld ci: %ld\n", pool_id, pg_id,
          raft_get_election_timeout_rand(), raft_get_election_timer(), raft_get_current_term(),
          raft_get_current_idx());

    return raft_become_candidate();
}

void raft_server_t::raft_become_leader()
{
    SPDK_NOTICELOG("becoming leader term:%ld\n", raft_get_current_term());

    raft_set_state(RAFT_STATE_LEADER);
    raft_time_t now = raft_get_cbs().get_time();
    raft_set_election_timer(now);
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
    SPDK_NOTICELOG("becoming candidate pool.pg %lu.%lu \n", pool_id, pg_id);

    raft_set_state(RAFT_STATE_CANDIDATE);
    raft_set_prevote(1);

    for(auto node : nodes){
        node->raft_node_vote_for_me(0);
    }
    raft_get_my_node()->raft_node_vote_for_me(1);

    raft_set_current_leader(-1);
    raft_randomize_election_timeout();
    auto election_timer = raft_get_cbs().get_time();
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
    SPDK_NOTICELOG("becoming prevoted candidate\n");

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
    SPDK_NOTICELOG("becoming follower\n");
    raft_set_state(RAFT_STATE_FOLLOWER);
    raft_randomize_election_timeout();
    auto election_timer = raft_get_cbs().get_time();
    raft_set_election_timer(election_timer);
}

int raft_server_t::_has_lease(raft_node* node, raft_time_t now, int with_grace)
{
    if (raft_is_self(node))
        return 1;

    if (with_grace)
    {
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

int raft_server_t::_has_majority_leases(raft_time_t now, int with_grace)
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

int raft_server_t::raft_has_majority_leases()
{
    if (raft_get_state() != RAFT_STATE_LEADER)
        return 0;

    /* Check without grace, because the caller may be checking leadership for
     * linearizability (§6.4). */
    return _has_majority_leases(raft_get_cbs().get_time(), 0 /* with_grace */);
}

int raft_server_t::raft_periodic()
{
    raft_node *my_node = raft_get_my_node();
    raft_time_t now = raft_get_cbs().get_time();

    if (raft_get_state() == RAFT_STATE_LEADER)
    {
        if (_has_majority_leases(now, 1 /* with_grace */))
        {
            /* A leader who can't maintain majority leases shall step down. */
            SPDK_NOTICELOG("unable to maintain majority leases\n");
            raft_become_follower();
            raft_set_current_leader(-1);
        }
        else if (raft_get_request_timeout() <= now - raft_get_election_timer())
        {
            raft_send_appendentries_all();
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

    if (raft_get_last_applied_idx() < raft_get_commit_idx() &&
        !raft_get_snapshot_in_progress())
    {
        int e = machine->raft_apply_all();
        if (0 != e)
            return e;
    }

    return 0;
}

/* Returns nonzero if we've got the term at idx or zero otherwise. */
int raft_server_t::raft_get_entry_term(raft_index_t idx, raft_term_t* term)
{
    int got = 1;
    auto ety = raft_get_entry_from_idx(idx);
    if (ety)
        *term = ety->term();
    else if (idx == raft_get_log()->log_get_base())
        *term = raft_get_log()->log_get_base_term();
    else
        got = 0;
    return got;
}

//接收到失败的response，如何处理raft_entry   todo ?
int raft_server_t::raft_process_appendentries_reply(
                                     msg_appendentries_response_t* r)
{
    SPDK_NOTICELOG(
          "received appendentries response %s ci:%ld rci:%ld 1stidx:%ld ls=%ld\n",
          r->success() == 1 ? "SUCCESS" : "fail",
          raft_get_current_idx(),
          r->current_idx(),
          r->first_idx(),
          r->lease());

    raft_node* node = raft_get_node(r->node_id());
    if (!node)
        return -1;

    if (!raft_is_leader())
        return RAFT_ERR_NOT_LEADER;

    /* If response contains term T > currentTerm: set currentTerm = T
       and convert to follower (§5.3) */
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

    raft_index_t match_idx = node->raft_node_get_match_idx();

    if (0 == r->success())
    {
        /* If AppendEntries fails because of log inconsistency:
           decrement nextIndex and retry (§5.3) */
        raft_index_t next_idx = node->raft_node_get_next_idx();
        assert(0 < next_idx);
        /* Stale response -- ignore */
        assert(match_idx <= next_idx - 1);
        if (match_idx == next_idx - 1)
            return 0;
        if (r->current_idx() < next_idx - 1)
            node->raft_node_set_next_idx(std::min(r->current_idx() + 1, raft_get_current_idx()));
        else
            node->raft_node_set_next_idx(next_idx - 1);

        /* retry */
        raft_send_appendentries(node);
        return 0;
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

    if (r->current_idx() <= match_idx)
        return 0;

    assert(r->current_idx() <= raft_get_current_idx());

    node->raft_node_set_next_idx(r->current_idx() + 1);
    node->raft_node_set_match_idx(r->current_idx());

    /* Update commit idx */
    raft_index_t point = r->current_idx();
    if (point && raft_get_commit_idx() < point)
    {
        raft_term_t term;
        int got = raft_get_entry_term(point, &term);
        if (got && term == raft_get_current_term())
        {
            int votes = 0;
            for(auto _node : nodes)
            {
                raft_node* tmpnode = _node.get();
                if (
                    tmpnode->raft_node_is_voting() &&
                    point <= tmpnode->raft_node_get_match_idx())
                {
                    votes++;
                }
            }

            if (raft_get_num_voting_nodes() / 2 < votes){
                raft_set_commit_idx(point);
                raft_write_entry_finish(first_idx, current_idx, 0); 
            }
        }
    }

    /* Aggressively send remaining entries */
    if (node->raft_node_get_next_idx() <= raft_get_current_idx())
        raft_send_appendentries(node);

    return 0;
}

void raft_server_t::follow_raft_disk_append_finish(raft_index_t start_idx, raft_index_t end_idx, raft_index_t _commit_idx, int result){
    if (raft_get_commit_idx() < _commit_idx)
        raft_set_commit_idx(_commit_idx);    
    raft_write_entry_finish(start_idx, end_idx, result);
}

struct follow_disk_append_complete : public context{
    follow_disk_append_complete(raft_index_t _start_idx, raft_index_t _end_idx,
    raft_index_t _commit_idx, raft_server_t* _raft)
    : start_idx(_start_idx)
    , end_idx(_end_idx)
    , commit_idx(_commit_idx)
    , raft(_raft) {}

    void finish(int r) override {
        raft->follow_raft_disk_append_finish(start_idx, end_idx, commit_idx, r);
    }
    raft_index_t start_idx;
    raft_index_t end_idx;
    raft_index_t commit_idx;
    raft_server_t* raft;
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
    raft_time_t election_timer;
    int i;
    std::vector<std::pair<std::shared_ptr<raft_entry_t>, context*>> entrys;
    int entries_num = ae->entries_size();
    raft_index_t start_idx;
    raft_index_t end_idx;
    follow_disk_append_complete *append_complete;
    raft_index_t commit_idx = 0; 

    if (0 < entries_num)
        SPDK_NOTICELOG("recvd appendentries t:%ld ci:%ld lc:%ld pli:%ld plt:%ld #%d\n",
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
        SPDK_NOTICELOG("AE term %ld is less than current term %ld\n",
              ae->term(), raft_get_current_term());
        goto out;
    }

    /* update current leader because ae->term is up to date */
    raft_set_current_leader(node_id);

    election_timer = raft_get_cbs().get_time();
    raft_set_election_timer(election_timer);
    r->set_lease(election_timer + election_timeout);

    /* Not the first appendentries we've received */
    /* NOTE: the log starts at 1 */
    if (0 < ae->prev_log_idx())
    {
        /* 2. Reply false if log doesn't contain an entry at prevLogIndex
           whose term matches prevLogTerm (§5.3) */
        raft_term_t term;
        int got = raft_get_entry_term(ae->prev_log_idx(), &term);
        if (!got && raft_get_current_idx() < ae->prev_log_idx())
        {
            SPDK_NOTICELOG("AE no log at prev_idx %ld \n", ae->prev_log_idx());
            goto out;
        }
        else if (got && term != ae->prev_log_term())
        {
            SPDK_NOTICELOG("AE term doesn't match prev_term (ie. %ld vs %ld) ci:%ld comi:%ld lcomi:%ld pli:%ld \n",
                  term, ae->prev_log_term(), raft_get_current_idx(),
                  raft_get_commit_idx(), ae->leader_commit(), ae->prev_log_idx());
            if (ae->prev_log_idx() <= raft_get_commit_idx())
            {
                /* Should never happen; something is seriously wrong! */
                SPDK_NOTICELOG("AE prev conflicts with committed entry\n");
                e = RAFT_ERR_SHUTDOWN;
                goto out;
            }
            /* Delete all the following log entries because they don't match */
            e = raft_delete_entry_from_idx(ae->prev_log_idx());
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
        int got = raft_get_entry_term(ety_index, &term);
        if (got && term != ety.term())
        {
            if (ety_index <= raft_get_commit_idx())
            {
                /* Should never happen; something is seriously wrong! */
                SPDK_NOTICELOG("AE entry conflicts with committed entry ci:%ld comi:%ld lcomi:%ld pli:%ld \n",
                      raft_get_current_idx(), raft_get_commit_idx(),
                      ae->leader_commit(), ae->prev_log_idx());
                e = RAFT_ERR_SHUTDOWN;
                goto out;
            }
            e = raft_delete_entry_from_idx(ety_index);
            if (0 != e)
                goto out;
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
    e = raft_append_entries(entrys);
    i += k;
    r->set_current_idx(ae->prev_log_idx() + i);
    if (0 != e)
        goto out;

    /* 5. If leaderCommit > commitIndex, set commitIndex =
        min(leaderCommit, index of last new entry) */
    if (raft_get_commit_idx() < ae->leader_commit())
    {
        commit_idx = std::min(ae->leader_commit(), r->current_idx());
        // if (raft_get_commit_idx() < commit_idx)
            // raft_set_commit_idx(commit_idx);
    }
    
    r->set_term(current_term);
    r->set_first_idx(ae->prev_log_idx() + 1);
    append_complete = new follow_disk_append_complete(start_idx, end_idx, commit_idx, this);
    raft_disk_append_entries(start_idx, end_idx, append_complete);
    return 0;

out:
    r->set_term(current_term);
    if (0 == r->success())
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

    raft_index_t current_idx = raft_get_current_idx();

    raft_term_t term;
    int got = raft_get_entry_term(current_idx, &term);
    assert(got);
    (void)got;
    if (term < vr->last_log_term())
        return 1;

    if (vr->last_log_term() == term && current_idx <= vr->last_log_idx())
        return 1;

    return 0;
}

int raft_server_t::raft_recv_requestvote(raft_node_id_t node_id,
                          const msg_requestvote_t* vr,
                          msg_requestvote_response_t *r)
{
    raft_time_t now = raft_get_cbs().get_time();
    int e = 0;

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
    SPDK_NOTICELOG("node requested vote%s: %d replying: %s \n",
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
    SPDK_NOTICELOG("node responded to requestvote%s status:%s ct:%ld rt:%ld \n",
          r->prevote() ? " (prevote)" : "",
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
                return RAFT_ERR_SHUTDOWN;
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
        r->set_term(current_term);
    }

    if (!raft_is_follower())
        raft_become_follower();

    raft_set_current_leader(node_id);
    auto election_timer = raft_get_cbs().get_time();
    raft_set_election_timer(election_timer);
    r->set_lease(election_timer + election_timeout);

    if (is->last_idx() <= raft_get_commit_idx())
    {
        /* Committed entries must match the snapshot. */
        r->set_complete(1);
        return 0;
    }

    raft_term_t term;
    int got = raft_get_entry_term(is->last_idx(), &term);
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
        return RAFT_ERR_NOT_LEADER;

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

int raft_server_t::_cfg_change_is_valid(msg_entry_t* ety)
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
        raft_write_entry_finish(start_idx, end_idx, result);
    }
}

struct disk_append_complete : public context{
    disk_append_complete(raft_index_t _start_idx, raft_index_t _end_idx, raft_server_t* _raft)
    : start_idx(_start_idx)
    , end_idx(_end_idx)
    , raft(_raft) {}

    void finish(int r) override {
        raft->raft_disk_append_finish(start_idx, end_idx, r);
    }
    raft_index_t start_idx;
    raft_index_t end_idx;
    raft_server_t* raft;
};

int raft_server_t::raft_write_entry(std::shared_ptr<msg_entry_t> ety,
                    context *complete)
{
    auto ety_ptr = ety.get();
    if (!raft_is_leader())
        return RAFT_ERR_NOT_LEADER;

    if (raft_entry_is_cfg_change(ety_ptr))
    {
        /* Multi-threading: need to fail here because user might be
         * snapshotting membership settings. */
        if (raft_get_snapshot_in_progress())
            return RAFT_ERR_SNAPSHOT_IN_PROGRESS;

        /* Only one voting cfg change at a time */
        if (raft_entry_is_voting_cfg_change(ety_ptr) &&
            raft_voting_change_is_in_progress())
                return RAFT_ERR_ONE_VOTING_CHANGE_ONLY;

        if (!_cfg_change_is_valid(ety_ptr))
            return RAFT_ERR_INVALID_CFG_CHANGE;
    }

    // ety->set_term(current_term);
    std::vector<std::pair<std::shared_ptr<msg_entry_t>, context*>> entrys;
    entrys.push_back(std::make_pair(ety, complete));
    int e = raft_append_entries(entrys);
    if (0 != e)
        return e;

    if (raft_entry_is_voting_cfg_change(ety_ptr))
        raft_set_voting_cfg_change_log_idx(ety->idx());

    if(current_idx > commit_idx){
        return 0;
    }
    //上一次的log已经commit了
    auto last_cache_idx = raft_get_last_cache_entry();
    first_idx = current_idx + 1;
    current_idx = last_cache_idx;

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
        if (next_idx == first_idx)
            raft_send_appendentries(node);
    }

    disk_append_complete *append_complete = new disk_append_complete(first_idx, current_idx, this);
    raft_disk_append_entries(first_idx, current_idx, append_complete);

    return 0;
}

int raft_server_t::raft_send_requestvote(raft_node* node)
{
    msg_requestvote_t* rv = new msg_requestvote_t();
    int e = 0;

    assert(node);
    assert(!raft_is_self(node));

    SPDK_NOTICELOG("sending requestvote%s to: %d , pool.pg %lu.%lu\n",
          raft_get_prevote() ? " (prevote)" : "", node->raft_node_get_id(), pool_id, pg_id);

    rv->set_node_id(raft_get_nodeid());
    rv->set_pool_id(pool_id);
    rv->set_pg_id(pg_id);    
    rv->set_term(current_term);
    rv->set_last_log_idx(raft_get_current_idx());
    rv->set_last_log_term(raft_get_last_log_term());
    rv->set_candidate_id(raft_get_nodeid());
    rv->set_prevote(raft_get_prevote());
    client.send_vote(this, node->raft_node_get_id(), rv);
    return e;
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

    SPDK_NOTICELOG("sending installsnapshot: ci:%ld comi:%ld t:%ld lli:%ld llt:%ld \n",
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


int raft_server_t::raft_send_appendentries(raft_node* node)
{
    assert(node);
    assert(!raft_is_self(node));

    msg_appendentries_t* ae = new msg_appendentries_t();
    ae->set_node_id(raft_get_nodeid());
    ae->set_pool_id(pool_id);
    ae->set_pg_id(pg_id);
    ae->set_term(raft_get_current_term());
    ae->set_leader_commit(raft_get_commit_idx());

    raft_index_t next_idx = node->raft_node_get_next_idx();

    if (next_idx <= raft_get_log()->log_get_base())
        return _raft_send_installsnapshot(node);

    _raft_get_entries_from_idx(next_idx, ae);

    ae->set_prev_log_idx(next_idx - 1);
    raft_term_t term;
    int got = raft_get_entry_term(ae->prev_log_idx(), &term);
    assert(got);
    (void)got;
    ae->set_prev_log_term(term);

    SPDK_NOTICELOG("sending appendentries node: ci:%ld comi:%ld t:%ld lc:%ld pli:%ld plt:%ld \n",
          raft_get_current_idx(),
          raft_get_commit_idx(),
          ae->term(),
          ae->leader_commit(),
          ae->prev_log_idx(),
          ae->prev_log_term());

    client.send_appendentries(this, node->raft_node_get_id(), ae);
    return 0;
}

int raft_server_t::raft_send_appendentries_all()
{
    int e;

    SPDK_NOTICELOG("in raft_send_appendentries_all\n");
    auto election_timer = raft_get_cbs().get_time();
    raft_set_election_timer(election_timer);
    for(auto _node : nodes)
    {
        raft_node* node = _node.get();
        if (raft_is_self(node))
            continue;

        e = raft_send_appendentries(node);
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
        node->raft_node_set_effective_time(raft_get_cbs().get_time());

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

void raft_server_t::raft_destroy()
{
    raft_destroy_nodes();
    machine.reset();
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
    if (raft_get_cbs().persist_vote) {
        int e = raft_get_cbs().persist_vote(this, udata, nodeid);
        if (0 != e)
            return e;
    }
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

raft_index_t raft_server_t::raft_get_first_entry_idx()
{
    assert(0 < raft_get_current_idx());

    return raft_get_log()->log_get_base() + 1;
}

raft_index_t raft_server_t::raft_get_num_snapshottable_logs()
{
    assert(raft_get_log()->log_get_base() <= raft_get_commit_idx());
    return raft_get_commit_idx() - raft_get_log()->log_get_base();
}

int raft_server_t::raft_begin_snapshot(raft_index_t idx)
{
    if (raft_get_commit_idx() < idx)
        return -1;

    auto ety = raft_get_entry_from_idx(idx);
    if (!ety)
        return -1;

    /* we need to get all the way to the commit idx */
    int e = machine->raft_apply_all();
    if (e != 0)
        return e;

    assert(raft_get_commit_idx() == raft_get_last_applied_idx());

    raft_set_snapshot_metadata(ety->term(), idx);
    raft_set_snapshot_in_progress(1);

    SPDK_NOTICELOG(
        "begin snapshot sli:%ld slt:%ld slogs:%ld \n",
        raft_get_snapshot_last_idx(),
        raft_get_snapshot_last_term(),
        raft_get_num_snapshottable_logs());

    return 0;
}

int raft_server_t::raft_end_snapshot()
{
    if (!raft_get_snapshot_in_progress() || raft_get_snapshot_last_idx() == 0)
        return -1;

    int e = raft_get_log()->log_poll(raft_get_snapshot_last_idx());
    if (e != 0)
        return e;

    raft_set_snapshot_in_progress(0);

    SPDK_NOTICELOG(
        "end snapshot base:%ld commit-index:%ld current-index:%ld\n",
        raft_get_log()->log_get_base(),
        raft_get_commit_idx(),
        raft_get_current_idx());

    return 0;
}

int raft_server_t::raft_begin_load_snapshot(
    raft_term_t last_included_term,
    raft_index_t last_included_index)
{
    if (last_included_index == -1)
        return -1;

    if (last_included_term == raft_get_snapshot_last_term() && last_included_index == raft_get_snapshot_last_idx())
        return RAFT_ERR_SNAPSHOT_ALREADY_LOADED;

    if (last_included_index <= raft_get_commit_idx())
        return -1;

    raft_get_log()->log_load_from_snapshot(last_included_index, last_included_term);

    raft_set_commit_idx(last_included_index);

    raft_set_last_applied_idx(last_included_index);
    raft_set_snapshot_metadata(last_included_term, raft_get_last_applied_idx());

    /* remove all nodes */
    raft_destroy_nodes();

    SPDK_NOTICELOG(
        "loaded snapshot sli:%ld slt:%ld slogs:%ld\n",
        raft_get_snapshot_last_idx(),
        raft_get_snapshot_last_term(),
        raft_get_num_snapshottable_logs());

    return 0;
}

int raft_server_t::raft_end_load_snapshot()
{
    /* Set nodes' voting status as committed */
    for(auto node : nodes)
    {
        if (node->raft_node_is_voting())
            node->raft_node_set_has_sufficient_logs();
    }

    return 0;
}
