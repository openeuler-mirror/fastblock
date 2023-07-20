#ifndef STATE_MACHINE_H_
#define STATE_MACHINE_H_

#include "raft_types.h"
#include "rpc/raft_msg.pb.h"

class raft_server_t;

class state_machine{
public:
    state_machine()
    : _raft(nullptr)
    , _last_applied_idx(0) {}

    void set_raft(raft_server_t* raft){
        _raft = raft;
    }

    void set_last_applied_idx(raft_index_t idx)
    {
        _last_applied_idx = idx;
    }

    /**
     * @return index of last applied entry */
    raft_index_t get_last_applied_idx()
    {
        return _last_applied_idx;
    }

    /**
     * Apply entry at lastApplied + 1. Entry becomes 'committed'.
     * @return 1 if entry committed, 0 otherwise */
    int raft_apply_entry();

    /** Apply all entries up to the commit index
     * @return
     *  0 on success;
     *  RAFT_ERR_SHUTDOWN when server MUST shutdown */
    int raft_apply_all();

    virtual void apply(std::shared_ptr<raft_entry_t> entry) = 0;
private:
    raft_server_t* _raft;

    /* idx of highest log entry applied to state machine */
    raft_index_t _last_applied_idx;
};

#endif