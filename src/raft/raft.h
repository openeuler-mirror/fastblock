#ifndef RAFT_H_
#define RAFT_H_
#include <string>
#include <vector>
#include <memory>
#include "raft_types.h"
#include "raft_private.h"
#include "raft_node.h"

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
     * Nodes in this non-voting state are used to catch up with the cluster,
     * when trying to the join the cluster.
     */
    RAFT_LOGTYPE_ADD_NONVOTING_NODE,
    /**
     * Membership change.
     * Add a voting node.
     */
    RAFT_LOGTYPE_ADD_NODE,
    /**
     * Membership change.
     * Demote a voting node to a non-voting node.
     */
    RAFT_LOGTYPE_DEMOTE_NODE,
    /**
     * Membership change.
     * Remove a voting node.
     */
    RAFT_LOGTYPE_REMOVE_NODE,
    /**
     * Membership change.
     * Promote a non-voting node to a voting node.
     */
    RAFT_LOGTYPE_PROMOTE_NODE,
    /**
     * Membership change.
     * Remove a non-voting node.
     */
    RAFT_LOGTYPE_REMOVE_NONVOTING_NODE,
    /**
     * Users can piggyback the entry mechanism by specifying log types that
     * are higher than RAFT_LOGTYPE_NUM.
     */
    RAFT_LOGTYPE_NUM=100,
} raft_logtype_e;

struct raft_node_configuration_t
{
    /** User data pointer for addressing.
     * Examples of what this could be:
     * - void* pointing to implementor's networking data
     * - a (IP,Port) tuple */
    void* udata_address;
};

/** Initialise a new Raft server.
 *
 * Request timeout defaults to 200 milliseconds
 * Election timeout defaults to 1000 milliseconds
 *
 * @return newly initialised Raft server */
extern std::shared_ptr<raft_server_t> raft_new(raft_client_protocol& client,
        disk_log* log, std::shared_ptr<state_machine> sm_ptr, uint64_t pool_id, uint64_t pg_id, kvstore*);

/** Determine if entry is voting configuration change.
 * @param[in] ety The entry to query.
 * @return 1 if this is a voting configuration change. */
int raft_entry_is_voting_cfg_change(raft_entry_t* ety);

/** Determine if entry is configuration change.
 * @param[in] ety The entry to query.
 * @return 1 if this is a configuration change. */
int raft_entry_is_cfg_change(raft_entry_t* ety);

/** Get the entry index of the entry that was snapshotted
 **/
raft_index_t raft_get_snapshot_entry_idx(raft_server_t *me_);

#endif /* RAFT_H_ */
