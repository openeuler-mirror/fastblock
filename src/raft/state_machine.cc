#include "state_machine.h"
#include "raft.h"
#include "spdk/log.h"
#include "utils/err_num.h"

constexpr uint32_t default_parallel_apply_num = 32;
constexpr int32_t TIMER_STATE_MACHINE_USEC = 10; // 微秒


static int apply_task(void *arg){
    state_machine* stm = (state_machine *)arg;
    stm->raft_apply_entry();
    return 0;
}

void state_machine::start(){
        _timer = SPDK_POLLER_REGISTER(&apply_task, this, TIMER_STATE_MACHINE_USEC);
}

struct apply_complete : public context{
    apply_complete(raft_index_t _idx, state_machine* _stm)
    : idx(_idx)
    , stm(_stm) {}

    void finish(int r) override {
        if(r == err::E_SUCCESS){
            stm->set_last_applied_idx(idx);
            stm->get_raft()->raft_get_log()->raft_write_entry_finish(idx, idx, r);
            stm->get_raft()->raft_get_log()->set_applied_index(idx);
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

int state_machine::raft_apply_all()
{
    if(_raft->get_stm_in_apply()){
        return 0;
    }
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

#ifdef MERGE_APPLY
//合并重复对象的entry
std::vector<std::shared_ptr<raft_entry_t>>
_merge_object(std::vector<std::shared_ptr<raft_entry_t>> &entrys, int num){
    return entrys;
}

int state_machine::raft_apply_entries(){
    if(_raft->get_stm_in_apply()){
        return 0;
    }

    if (_raft->raft_get_snapshot_in_progress())
        return 0;

    _raft->set_stm_in_apply(true);

    std::vector<std::shared_ptr<raft_entry_t>> entrys;
    int num = 0;
    _raft->raft_get_log()->log_get_from_idx(
            _last_applied_idx + 1, default_parallel_apply_num, entrys);
    for(auto entry : entrys){
        if(entry->idx() > _raft->raft_get_commit_idx())
            break;
        num++;
    }
    if(num == 0)
        return 0;
    
    auto merged_entrys = _merge_object(entrys, num);
    
    return 0;
}
#endif

bool state_machine::linearization() {
    // 在租期不会发生选举，确保 Leader 不会变。
    if (_raft->is_lease_valid()) {
        return true;
    }
  
    return false;
}