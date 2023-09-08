#ifndef STATE_MACHINE_H_
#define STATE_MACHINE_H_

#include "raft_types.h"
#include "rpc/raft_msg.pb.h"
#include "utils/utils.h"
#include "spdk/thread.h"

class raft_server_t;

class state_machine{
public:
    state_machine()
    : _raft(nullptr)
    , _last_applied_idx(0)
    , _apply_in_progress(false) {}

    void set_raft(raft_server_t* raft){
        _raft = raft;
    }

    void set_last_applied_idx(raft_index_t idx)
    {
        _last_applied_idx = idx;
    }

    void start();
    void stop(){
        spdk_poller_unregister(&_timer);
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
    int raft_apply_entries();

    virtual void apply(std::shared_ptr<raft_entry_t> entry, context *complete) = 0;
    raft_server_t* get_raft(){
        return _raft;
    }

    bool get_apply_in_progress(){
        return _apply_in_progress;
    }

    void set_apply_in_progress(bool apply_in_progress){
        _apply_in_progress = apply_in_progress;
    }

    bool linearization();

private:
    raft_server_t* _raft;

    /* idx of highest log entry applied to state machine */
    raft_index_t _last_applied_idx;
    bool _apply_in_progress;
    struct spdk_poller * _timer;
};

#endif