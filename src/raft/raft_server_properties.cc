#include <assert.h>

#include "raft.h"
#include "raft_log.h"
#include "raft_private.h"

raft_server_t::raft_server_t(raft_client_protocol& _client, disk_log* _log, 
        std::shared_ptr<state_machine> sm_ptr, uint64_t _pool_id, uint64_t _pg_id)
    : current_term(0)
    , voted_for(-1)
    , commit_idx(0)
    , state(RAFT_STATE_FOLLOWER)
    , prevote(0)
    , start_time(0)
    , election_timer(0)
    , election_timeout(1000)
    , request_timeout(200)
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
        if (cb.persist_term)
        {
            int e = cb.persist_term(this, udata, term, voted_for_local);
            if (0 != e)
                return e;
        }
        current_term = term;
        voted_for = voted_for_local;
    }
    return 0;
}

raft_term_t raft_server_t::raft_get_last_log_term()
{
    raft_index_t current_idx = raft_get_current_idx();
    raft_term_t term;
    int got = raft_get_entry_term(current_idx, &term);
    assert(got);
    (void)got;
    return term;
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