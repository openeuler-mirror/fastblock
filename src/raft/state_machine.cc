#include "state_machine.h"
#include "raft.h"
#include "spdk/log.h"

int state_machine::raft_apply_entry()
{
    if (_raft->raft_get_snapshot_in_progress())
        return -1;

    /* Don't apply after the commit_idx */
    if (_last_applied_idx == _raft->raft_get_commit_idx())
        return -1;

    raft_index_t log_idx = _last_applied_idx + 1;

    auto ety =  _raft->raft_get_entry_from_idx(log_idx);
    if (!ety)
        return -1;

    SPDK_NOTICELOG("applying log: %ld, idx: %ld size: %u \n",
          log_idx, ety->idx(), (uint32_t)ety->data().size());

    _last_applied_idx += 1;

    apply(ety);

    /* voting cfg change is now complete */
    if (log_idx == _raft->raft_get_voting_cfg_change_log_idx())
        _raft->raft_set_voting_cfg_change_log_idx(-1);

    return 0;
}

int state_machine::raft_apply_all()
{
    if (_raft->raft_get_snapshot_in_progress())
        return 0;

    while (_last_applied_idx < _raft->raft_get_commit_idx())
    {
        int e = raft_apply_entry();
        if (0 != e)
            return e;
    }

    return 0;
}