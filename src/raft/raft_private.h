#ifndef RAFT_PRIVATE_H_
#define RAFT_PRIVATE_H_
#include <assert.h>

#include "raft_types.h"
#include "raft_node.h"
#include "raft_log.h"
#include "state_machine.h"
#include "rpc/raft_msg.pb.h"
#include "utils/utils.h"
#include "raft/raft_client_protocol.h"
#include "raft/append_entry_buffer.h"

constexpr int32_t TIMER_PERIOD_MSEC = 500;    //毫秒
constexpr int32_t HEARTBEAT_TIMER_INTERVAL_MSEC = 500;   //毫秒
constexpr int32_t HEARTBEAT_TIMER_PERIOD_MSEC = 2 * HEARTBEAT_TIMER_INTERVAL_MSEC;   //毫秒
constexpr int32_t ELECTION_TIMER_PERIOD_MSEC = 2 * HEARTBEAT_TIMER_PERIOD_MSEC; //毫秒
constexpr int32_t LEASE_MAINTENANCE_GRACE = HEARTBEAT_TIMER_PERIOD_MSEC;   //毫秒

enum {
    RAFT_NODE_STATUS_DISCONNECTED,
    RAFT_NODE_STATUS_CONNECTED,
    RAFT_NODE_STATUS_CONNECTING,
    RAFT_NODE_STATUS_DISCONNECTING
};

typedef enum {
    RAFT_MEMBERSHIP_ADD,
    RAFT_MEMBERSHIP_REMOVE,
} raft_membership_e;

typedef enum {
    RAFT_STATE_NONE,
    RAFT_STATE_FOLLOWER,
    RAFT_STATE_CANDIDATE,
    RAFT_STATE_LEADER
} raft_state_e;

/** Message sent from client to server.
 * The client sends this message to a server with the intention of having it
 * applied to the FSM. */
typedef raft_entry_t msg_entry_t;

class raft_server_t;

/** Callback for receiving InstallSnapshot request messages.
 * @param[in] raft The Raft server making this callback
 * @param[in] user_data User data that is passed from Raft server
 * @param[in] node The node that we are receiving this message from
 * @param[in] msg The InstallSnapshot message
 * @param[in] r The InstallSnapshot response to be sent
 * @return
 *  0 if this chunk is successful received
 *  1 if the whole snapshot is successfully received
 *  or an error */
typedef int (
*func_recv_installsnapshot_f
)   (
    raft_server_t* raft,
    void *user_data,
    raft_node* node,
    const msg_installsnapshot_t* msg,
    msg_installsnapshot_response_t* r
    );

/** Callback for receiving InstallSnapshot response messages.
 * @param[in] raft The Raft server making this callback
 * @param[in] user_data User data that is passed from Raft server
 * @param[in] node The node that we are sending this message to
 * @param[in] msg The InstallSnapshot message
 * @param[in] r The InstallSnapshot response to be sent
 * @return
 *  0 if this chunk is successful received
 *  1 if the whole snapshot is successfully received
 *  or an error */
typedef int (
*func_recv_installsnapshot_response_f
)   (
    raft_server_t* raft,
    void *user_data,
    raft_node* node,
    msg_installsnapshot_response_t* r
    );

/** Callback for saving changes to one log entry. See also
 * func_logentries_event_f. */
typedef int (
*func_logentry_event_f
)   (
    raft_server_t* raft,
    void *user_data,
    raft_entry_t *entry,
    raft_index_t entry_idx
    );

/** Callback for detecting when non-voting nodes have obtained enough logs.
 * This triggers only when there are no pending configuration changes.
 * @param[in] raft The Raft server making this callback
 * @param[in] user_data User data that is passed from Raft server
 * @param[in] node The node
 * @return 0 does not want to be notified again; otherwise -1 */
typedef int (
*func_node_has_sufficient_logs_f
)   (
    raft_server_t* raft,
    void *user_data,
    raft_node* node
    );

/** Callback for being notified of membership changes.
 *
 * Implementing this callback is optional.
 *
 * Remove notification happens before the node is about to be removed.
 *
 * @param[in] raft The Raft server making this callback
 * @param[in] user_data User data that is passed from Raft server
 * @param[in] node The node that is the subject of this log
 * @param[in] entry The entry that was the trigger for the event. Could be NULL.
 * @param[in] type The type of membership change */
typedef void (
*func_membership_event_f
)   (
    raft_server_t* raft,
    void *user_data,
    raft_node *node,
    raft_entry_t *entry,
    raft_membership_e type
    );
    
struct raft_cbs_t
{
    /** Callback for receiving InstallSnapshot messages */
    func_recv_installsnapshot_f recv_installsnapshot;

    /** Callback for receiving InstallSnapshot responses */
    func_recv_installsnapshot_response_f recv_installsnapshot_response;

    /** Callback for determining which node this configuration log entry
     * affects. This call only applies to configuration change log entries.
     * @note entry_idx may be 0, indicating that the index is unknown.
     * @return the node ID of the node */
    func_logentry_event_f log_get_node_id;

    /** Callback for detecting when a non-voting node has sufficient logs. */
    func_node_has_sufficient_logs_f node_has_sufficient_logs;

    func_membership_event_f notify_membership_event;
};

class raft_server_t{
public:
    raft_server_t(raft_client_protocol& client, disk_log* log, std::shared_ptr<state_machine> sm_ptr, uint64_t pool_id, uint64_t pg_id);

    ~raft_server_t();

    void raft_randomize_election_timeout(){
        /* [election_timeout, 2 * election_timeout) */
        election_timeout_rand = election_timeout + rand() % election_timeout;
    }

    int raft_get_election_timeout_rand(){
        return election_timeout_rand;
    }    

    void raft_set_snapshot_metadata(raft_term_t term, raft_index_t idx)
    {
        snapshot_last_term = term;
        snapshot_last_idx = idx;
    }

    /** Set election timeout.
     * The amount of time that needs to elapse before we assume the leader is down
     * @param[in] msec Election timeout in milliseconds */
    void raft_set_election_timeout(int millisec)
    {
        election_timeout = millisec;
        raft_randomize_election_timeout();
    }

    /** Set request timeout in milliseconds.
     * The amount of time before we resend an appendentries message
     * @param[in] msec Request timeout in milliseconds */
    void raft_set_heartbeat_timeout(int millisec)
    {
        heartbeat_timeout = millisec;
    }

    /** Set lease maintenance grace.
     * The amount of time granted to a leader to reestablish an expired lease
     * before the leader is concluded to be unable to maintain the lease (a leader
     * who is unable to maintain leases from a majority steps down voluntarily)
     * @param[in] msec Lease mainenace grace in milliseconds */
    void raft_set_lease_maintenance_grace(int millisec)
    {
        lease_maintenance_grace = millisec;
    }

    /** Set "first start" flag.
     * This indicates that me represents the first start of the (persistent)
     * server. */
    void raft_set_first_start()
    {
        first_start = 1;
    }

    int raft_get_first_start()
    {
        return first_start;
    }


   /** Set this server's node ID.
     * This should be called right after raft_new/raft_clear. */
    void raft_set_nodeid(raft_node_id_t id)
    {
        assert(node_id == -1);
        node_id = id;
    }

    /**
     * @return server's node ID; -1 if it doesn't know what it is */
    raft_node_id_t raft_get_nodeid()
    {
        return node_id;
    }

    /**
     * @return currently configured election timeout in milliseconds */
    int raft_get_election_timeout()
    {
        return election_timeout;
    }

    /**
     * @return request timeout in milliseconds */
    int raft_get_heartbeat_timeout()
    {
        return heartbeat_timeout;
    }
    
    /**
     * @return lease maintenance grace in milliseconds */
    int raft_get_lease_maintenance_grace()
    {
        return lease_maintenance_grace;
    }

    /**
     * @return currently elapsed timeout in milliseconds */
    int raft_get_timeout_elapsed()
    {
        return get_time() - election_timer;
    }

    void raft_set_voted_for(raft_node_id_t _voted_for)
    {
        voted_for = _voted_for;
    }

    /**
     * @return node ID of who I voted for */
    raft_node_id_t raft_get_voted_for()
    {
        return voted_for;
    }

    /**
     * @return number of voting nodes that this server has */
    int raft_get_num_voting_nodes()
    {
        int num = 0;
        for(auto node : nodes){
            if (node->raft_node_is_voting())
                num++;
        }
        return num;
    }

    /** Set the current term.
     * This should be used to reload persistent state, ie. the current_term field.
     * @param[in] term The new current term
     * @return
     *  0 on success */
    int raft_set_current_term(const raft_term_t term);

    raft_term_t raft_get_current_term()
    {
        return current_term;
    }

    /**
     * @return current log index */
    raft_index_t raft_get_current_idx()
    {
        return current_idx;
    }

    /** Set the commit idx.
     * This should be used to reload persistent state, ie. the commit_idx field.
     * @param[in] commit_idx The new commit index. */
    void raft_set_commit_idx(raft_index_t idx)
    {
        assert(commit_idx <= idx);
        assert(idx <= raft_get_current_idx());
        commit_idx = idx;
    }

    void raft_set_last_applied_idx(raft_index_t idx)
    {
        machine->set_last_applied_idx(idx);
    }

    /**
     * @return index of last applied entry */
    raft_index_t raft_get_last_applied_idx()
    {
        return machine->get_last_applied_idx();
    }
    
    raft_index_t raft_get_commit_idx()
    {
        return commit_idx;
    }

    void raft_set_state(int state_local)
    {
        /* if became the leader, then update the current leader entry */
        if (state_local == RAFT_STATE_LEADER)
            leader_id = node_id;
        state = state_local;
    }
    
    /** Tell if we are a leader, candidate or follower.
     * @return get state of type raft_state_e. */
    int raft_get_state()
    {
        return state;
    }

    /**
     * @param[in] node The node's ID
     * @return node pointed to by node ID */
    raft_node* raft_get_node(raft_node_id_t nodeid)
    {
        for(auto node : nodes){
            if (nodeid == node->raft_node_get_id())
                return node.get();
        }
    
        return nullptr;
    }

    /**
     * @return the server's node */
    raft_node* raft_get_my_node()
    {
        return raft_get_node(node_id);
    }

    /** Get what this node thinks the node ID of the leader is.
     * @return node of what this node thinks is the valid leader;
     *   -1 if the leader is unknown */
    raft_node_id_t raft_get_current_leader()
    {
        return leader_id;
    }

    void raft_set_current_leader(raft_node_id_t _leader_id)
    {
        leader_id = _leader_id;
    }

    /** Get what this node thinks the node of the leader is.
     * @return node of what this node thinks is the valid leader;
     *   NULL if the leader is unknown */
    raft_node* raft_get_current_leader_node()
    {
        return raft_get_node(leader_id);
    }

    /**
     * @return callback user data */
    void* raft_get_udata()
    {
        return udata;
    }

    void raft_set_udata(void* _udata)
    {
        udata = _udata;
    }    

    /**
     * @return 1 if follower; 0 otherwise */
    int raft_is_follower()
    {
        return raft_get_state() == RAFT_STATE_FOLLOWER;
    }

    /**
     * @return 1 if leader; 0 otherwise */
    int raft_is_leader()
    {
        return raft_get_state() == RAFT_STATE_LEADER;
    }

    /**
     * @return 1 if candidate; 0 otherwise */
    int raft_is_candidate()
    {
        return raft_get_state() == RAFT_STATE_CANDIDATE;
    }

    int raft_is_prevoted_candidate()
    {
        return raft_get_state() == RAFT_STATE_CANDIDATE && !prevote;
    }

    /**
     * @return 1 if node ID matches the server; 0 otherwise */
    int raft_is_self(raft_node* node)
    {
        return (node && node->raft_node_get_id() == node_id);
    }

    int raft_is_connected()
    {
        return connected;
    }

    void raft_set_connected(int _connected){
        connected = _connected;
    }

    void raft_set_snapshot_in_progress(int _snapshot_in_progress) {
        snapshot_in_progress = _snapshot_in_progress;
    }

    /** Check is a snapshot is in progress **/
    int raft_get_snapshot_in_progress()
    {
        return snapshot_in_progress;
    }

    /** Get last applied entry **/
    std::shared_ptr<raft_entry_t> raft_get_last_applied_entry();

    raft_index_t raft_get_snapshot_last_idx()
    {
        return snapshot_last_idx;
    }
    
    raft_term_t raft_get_snapshot_last_term()
    {
        return snapshot_last_term;
    }

    raft_cbs_t&  raft_get_cbs(){
        return cb;
    }
    
    /** De-initialise Raft server. */
    void raft_clear();

    std::shared_ptr<raft_log> raft_get_log(){
        return log;
    }

    void raft_set_election_timer(raft_time_t _election_timer){
        election_timer = _election_timer;
    }

    raft_time_t raft_get_election_timer(){
        return election_timer;
    }

    void raft_set_start_time(raft_time_t _start_time){
        start_time = _start_time;
    }

    raft_time_t raft_get_start_time(){
        return start_time;
    }

    void raft_set_voting_cfg_change_log_idx(raft_index_t _voting_cfg_change_log_idx){
        voting_cfg_change_log_idx = _voting_cfg_change_log_idx;
    }

    raft_index_t raft_get_voting_cfg_change_log_idx(){
        return voting_cfg_change_log_idx;
    }

    int raft_get_prevote(){
        return prevote;
    }

    void raft_set_prevote(int _prevote){
        prevote = _prevote;
    }

    /**
     * @param[in] idx The entry's index
     **/
    std::shared_ptr<raft_entry_t> raft_get_entry_from_idx(raft_index_t idx)
    {
        return raft_get_log()->log_get_at_idx(idx);
    }

    raft_node* raft_add_node_internal(raft_entry_t *ety, void* udata, raft_node_id_t id, bool is_self);

    /** Add node.
     *
     * If a node with the same ID already exists the call will fail.
     *
     * @param[in] user_data The user data for the node.
     *  This is obtained using raft_node_get_udata.
     *  Examples of what this could be:
     *  - void* pointing to implementor's networking data
     *  - a (IP,Port) tuple
     * @param[in] id The integer ID of this node
     *  This is used for identifying clients across sessions.
     * @param[in] is_self Set to 1 if this "node" is this server
     * @return
     *  node if it was successfully added;
     *  NULL if the node already exists */
    raft_node* raft_add_node(void* udata, raft_node_id_t id, bool is_self){
        return raft_add_node_internal(NULL, udata, id, is_self);
    }

    raft_node* raft_add_non_voting_node_internal(raft_entry_t *ety, void* udata, raft_node_id_t id, bool is_self);

    /** Add a node which does not participate in voting.
     * If a node already exists the call will fail.
     * Parameters are identical to raft_add_node
     * @return
     *  node if it was successfully added;
     *  NULL if the node already exists */
    raft_node* raft_add_non_voting_node(void* udata, raft_node_id_t id, bool is_self)
    {
        return raft_add_non_voting_node_internal(NULL, udata, id, is_self);
    }

    /** Remove node.
     * @param node The node to be removed. */
    void raft_remove_node(raft_node* node);

    /*
     *  clear all nodes
     */
    void raft_destroy_nodes();

    void raft_destroy();

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

    /** Vote for a server.
     * This should be used to reload persistent state, ie. the voted-for field.
     * @param[in] node The server to vote for
     * @return
     *  0 on success */
    int raft_vote(raft_node* node)
    {
        return raft_vote_for_nodeid(node ? node->raft_node_get_id() : -1);
    }

    int raft_count_votes();

    /** @return
     *  nonzero if server is leader and has leases from majority of voting nodes;
     *  0 if server is not leader or lacks leases from majority of voting nodes;
    */
    bool raft_has_majority_leases();

    /** Process events that are dependent on time passing.
     * @return
     *  0 on success;
     *  -1 on failure;
     *  RAFT_ERR_SHUTDOWN when server MUST shutdown */
    int raft_periodic();

    void follow_raft_disk_append_finish(raft_index_t start_idx, raft_index_t end_idx, raft_index_t _commit_idx, int result);

    /** Receive an appendentries message.
     *
     * Will block (ie. by syncing to disk) if we need to append a message.
     *
     * Might call malloc once to increase the log entry array size.
     *
     * The log_offer callback will be called.
     *
     * @note The memory pointer (ie. raft_entry_data_t) for each msg_entry_t is
     *   copied directly. If the memory is temporary you MUST either make the
     *   memory permanent (ie. via malloc) OR re-assign the memory within the
     *   log_offer callback.
     *
     * @param[in] node The node who sent us this message
     * @param[in] ae The appendentries message
     * @param[out] r The resulting response
     * @return
     *  0 on success
     *  RAFT_ERR_NEEDS_SNAPSHOT
     *  */
    int raft_recv_appendentries(raft_node_id_t node_id,
                                const msg_appendentries_t* ae,
                                msg_appendentries_response_t *r,
                                context* complete);

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

    void raft_disk_append_finish(raft_index_t start_idx, raft_index_t end_idx, int result);

    /** Receive an entry message from the client.
     *
     * Append the entry to the log and send appendentries to followers.
     *
     * Will block (ie. by syncing to disk) if we need to append a message.
     *
     * Might call malloc once to increase the log entry array size.
     *
     * The log_offer callback will be called.
     *
     * @note The memory pointer (ie. raft_entry_data_t) in msg_entry_t is
     *  copied directly. If the memory is temporary you MUST either make the
     *  memory permanent (ie. via malloc) OR re-assign the memory within the
     *  log_offer callback.
     *
     * Will fail:
     * <ul>
     *      <li>if the server is not the leader
     * </ul>
     *
     * @param[in] node The node who sent us this message
     * @param[in] ety The entry message
     * @param[out] r The resulting response
     * @return
     *  0 on success;
     *  RAFT_ERR_NOT_LEADER server is not the leader;
     *  RAFT_ERR_SHUTDOWN server MUST shutdown;
     *  RAFT_ERR_ONE_VOTING_CHANGE_ONLY there is a non-voting change inflight;
     *  RAFT_ERR_NOMEM memory allocation failure
     */
    int raft_write_entry(std::shared_ptr<msg_entry_t> ety, context *complete);

    void raft_flush();

    int raft_voting_change_is_in_progress()
    {
        return voting_cfg_change_log_idx != -1;
    }

    int raft_send_heartbeat(raft_node* node);

    int raft_send_appendentries(raft_node* node);

    bool raft_get_entry_term(raft_index_t idx, raft_term_t& term);
    
    int raft_send_heartbeat_all();

    /**
     * @return number of votes this server has received this election */
    int raft_get_nvotes_for_me();

    /** Start loading snapshot
     *
     * This is usually the result of a snapshot being loaded.
     * We need to send an appendentries response.
     *
     * @param[in] last_included_term Term of the last log of the snapshot
     * @param[in] last_included_index Index of the last log of the snapshot 
     *
     * @return
     *  0 on success
     *  -1 on failure
     *  RAFT_ERR_SNAPSHOT_ALREADY_LOADED
     **/
    int raft_begin_load_snapshot(raft_term_t last_included_term,
    		                     raft_index_t last_included_index);

    /** Stop loading snapshot.
     *
     * @return
     *  0 on success
     *  -1 on failure
     **/
    int raft_end_load_snapshot();

    int raft_delete_entry_from_idx(raft_index_t idx);

    /** Add entries to the server's log.
     * This should be used to reload persistent state, ie. the commit log.
     * @param[in] entries List of entries to be appended
     * @return
     *  0 on success;
     *  RAFT_ERR_SHUTDOWN server should shutdown
     *  RAFT_ERR_NOMEM memory allocation failure */
    int raft_append_entries(std::vector<std::pair<std::shared_ptr<msg_entry_t>, context*>>& entries){
        return raft_get_log()->log_append(entries);
    }

    void raft_disk_append_entries(raft_index_t start_idx, raft_index_t end_idx, context* complete){
        raft_get_log()->disk_append(start_idx, end_idx, complete);
    }

    raft_index_t raft_get_last_cache_entry(){
        return raft_get_log()->get_last_cache_entry();
    }

    /**
     * @return 0 on error */
    int raft_send_requestvote(raft_node* node);

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

    /** Receive an InstallSnapshot message. */
    int raft_recv_installsnapshot(raft_node_id_t node_id,
                                  const msg_installsnapshot_t* is,
                                  msg_installsnapshot_response_t *r,
                                  context* complete);

    /** Receive an InstallSnapshot message. */
    int raft_process_installsnapshot_reply(msg_installsnapshot_response_t *r);
    void raft_offer_log(std::vector<std::shared_ptr<raft_entry_t>>& entries,
                        raft_index_t idx);

    /** Set callbacks and user data.
     *
     * @param[in] funcs Callbacks
     * @param[in] user_data "User data" - user's context that's included in a callback */
    void raft_set_callbacks(raft_cbs_t* funcs, void* user_data);

    int raft_election_start();

    /** Begin snapshotting an index.
     *
     * While snapshotting, raft will:
     *  - not apply log entries
     *  - not start elections
     *
     * @return 0 on success
     *
     **/
    int raft_begin_snapshot(raft_index_t idx);

    /** Stop snapshotting.
     *
     * The user MUST include membership changes inside the snapshot. This means
     * that membership changes are included in the size of the snapshot. For peers
     * that load the snapshot, the user needs to deserialize the snapshot to
     * obtain the membership changes.
     *
     * The user MUST compact the log up to the commit index. This means all
     * log entries up to the commit index MUST be deleted (aka polled).
     *
     * @return
     *  0 on success
     *  -1 on failure
     **/
    int raft_end_snapshot();

    raft_index_t raft_get_num_snapshottable_logs();

    raft_index_t raft_get_first_entry_idx();

    bool get_stm_in_apply(){
        return stm_in_apply;
    }

    void set_stm_in_apply(bool _stm_in_apply){
        stm_in_apply = _stm_in_apply;
    }

    void start_timed_task(){
        _append_entries_buffer.start();
        machine->start();
    }

    void stop_timed_task(){
        _append_entries_buffer.stop();
        machine->stop();
    }

    void append_entries_to_buffer(const msg_appendentries_t* request,
                msg_appendentries_response_t* response,
                context* complete){
        _append_entries_buffer.enqueue(request, response, complete);
    }

    int  save_vote_for(const raft_node_id_t nodeid){
#ifdef KVSTORE
        std::string key = std::to_string(pool_id) + "." + std::to_string(pg_id) + ".vote_for";
        std::string val = std::to_string(nodeid);
        kv.set(key, val);
#endif
        return 0;
    }

    std::optional<raft_node_id_t> load_vote_for(){
#ifdef KVSTORE
        std::string key = std::to_string(pool_id) + "." + std::to_string(pg_id) + ".vote_for";
        auto val = kv.get(key);
        if(!val.has_value())
            return std::nnullopt;
        return atoi(val.value().c_str());
#else
        return std::nullopt;
#endif 
    }

    int save_term(const raft_term_t term){
#ifdef KVSTORE
        std::string key = std::to_string(pool_id) + "." + std::to_string(pg_id) + ".term";
        std::string val = std::to_string(term);
        kv.set(key, val);
#endif
        return 0;
    }

    std::optional<raft_term_t> load_term(){
#ifdef KVSTORE
        std::string key = std::to_string(pool_id) + "." + std::to_string(pg_id) + ".term";
        auto val = kv.get(key);
        if(!val.has_value())
            return std::nullopt;
        return atol(val.value().c_str());
#else
        return std::nullopt;
#endif
    }

    void start_raft_timer();
    template<typename Func>
    void for_each_osd_id(Func&& f) const {
        std::for_each(
          std::cbegin(nodes), std::cend(nodes), std::forward<Func>(f));
    }

    uint64_t raft_get_pool_id(){
        return pool_id;
    }

    uint64_t raft_get_pg_id(){
        return pg_id;
    }

    bool is_lease_valid(){
        return raft_has_majority_leases();
    }

    msg_appendentries_t* create_appendentries(raft_node* node);
    void dispatch_recovery(raft_node* node);
    void do_recovery(raft_node* node);
private:
    bool _has_majority_leases(raft_time_t now, int with_grace);
    int _cfg_change_is_valid(msg_entry_t* ety);
    int _should_grant_vote(const msg_requestvote_t* vr);
    int _raft_send_installsnapshot(raft_node* node);
    int _has_lease(raft_node* node, raft_time_t now, int with_grace);
    void _raft_get_entries_from_idx(raft_index_t idx, msg_appendentries_t* ae);

    std::vector<std::shared_ptr<raft_node>> nodes;

    /* the server's best guess of what the current term is
     * starts at zero */
    raft_term_t current_term;

    /* The candidate the server voted for in its current term,
     * or Nil if it hasn't voted for any.  */
    raft_node_id_t voted_for;

    /* the log which is replicated */
    std::shared_ptr<raft_log> log; 

    /* Volatile state: */

    /* idx of highest log entry known to be committed */
    raft_index_t commit_idx;

    /* follower/leader/candidate indicator */
    int state;

    /* true if this server is in the candidate prevote state (§4.2.3, §9.6) */
    int prevote;

    /* start time of this server */
    raft_time_t start_time;

    /* start time of election timer */
    raft_time_t election_timer;

    int election_timeout;
    int election_timeout_rand;
    int heartbeat_timeout;

    /* what this node thinks is the node ID of the current leader,
     * or -1 if there isn't a known current leader. */
    raft_node_id_t leader_id;

    /* my node ID */
    raft_node_id_t node_id;

    /* callbacks */
    raft_cbs_t cb;
    void* udata;

    /* the log which has a voting cfg change, otherwise -1 */
    raft_index_t voting_cfg_change_log_idx;

    /* Our membership with the cluster is confirmed (ie. configuration log was
     * committed) */
    int connected;

    int snapshot_in_progress;

    /* Last compacted snapshot */
    raft_index_t snapshot_last_idx;
    raft_term_t snapshot_last_term;

    /* grace period after each lease expiration time honored when we determine
     * if a leader is maintaining leases from a majority (see raft_periodic) */
    int lease_maintenance_grace;

    /* represents the first start of this (persistent) server; not a restart */
    int first_start;

    std::shared_ptr<state_machine>  machine;

    uint64_t pool_id;
    uint64_t pg_id;

    raft_index_t first_idx;     //当前正在处理的一批msg中第一个的idx
    raft_index_t current_idx;   //当前正在处理的一批msg中最后一个的idx
    raft_client_protocol& client;

    bool stm_in_apply;     //状态机正在apply

    append_entries_buffer _append_entries_buffer;

#ifdef KVSTORE
    kv_store *kv;
#endif
   
    struct spdk_poller * raft_timer;

    raft_index_t _last_index_before_become_leader;
}; 

int raft_votes_is_majority(const int nnodes, const int nvotes);

void raft_pop_log(void *arg, raft_index_t idx, std::shared_ptr<raft_entry_t> entry);

#endif /* RAFT_PRIVATE_H_ */
