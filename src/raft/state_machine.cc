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

#include "state_machine.h"
#include "raft.h"
#include "spdk/log.h"
#include "utils/err_num.h"

constexpr uint32_t default_parallel_apply_num = 32;

static int apply_task(void *arg){
    state_machine* stm = (state_machine *)arg;
    stm->raft_apply_entry();
    return 0;
}

void state_machine::start(){
    _timer = SPDK_POLLER_REGISTER(&apply_task, this, 0);
}

struct apply_complete : public context{
    apply_complete(raft_index_t _idx, state_machine* _stm)
    : idx(_idx)
    , stm(_stm) {}

    void finish(int r) override {
        if(r == err::E_SUCCESS){
            auto last_applied_idx = stm->get_last_applied_idx();
            stm->set_last_applied_idx(idx);
            stm->get_raft()->raft_get_log()->raft_write_entry_finish(idx, idx, r);
            stm->get_raft()->raft_get_log()->set_applied_index(last_applied_idx, idx);
            /* voting cfg change is now complete */
            if (idx == stm->get_raft()->raft_get_voting_cfg_change_log_idx())
                stm->get_raft()->raft_set_voting_cfg_change_log_idx(-1);
        }
        stm->set_apply_in_progress(false);
    }
    raft_index_t idx;
    state_machine* stm;
};

int state_machine::raft_apply_entry()
{
    if (_raft->raft_get_snapshot_in_progress())
        return -1;

    /* Don't apply after the commit_idx */
    if (_last_applied_idx == _raft->raft_get_commit_idx())
        return -1;

    if(get_apply_in_progress())
        return 0;
    set_apply_in_progress(true);

    raft_index_t log_idx = _last_applied_idx + 1;

    auto ety =  _raft->raft_get_entry_from_idx(log_idx);
    if (!ety){
        set_apply_in_progress(false);
        return -1;
    }

    SPDK_INFOLOG(pg_group, "osd %d applying log: %ld, idx: %ld size: %u \n",
                 get_raft()->raft_get_nodeid(), log_idx, ety->idx(), (uint32_t)ety->data().size());

    apply_complete *complete = new apply_complete(log_idx, this);
    apply(ety, complete);
    return 0;
}

bool state_machine::linearization() {
    // 在租期不会发生选举，确保 Leader 不会变。
    if (_raft->is_lease_valid()) {
        return true;
    }
  
    return false;
}