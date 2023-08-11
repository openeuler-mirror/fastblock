#include "pg_group.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/util.h"

constexpr int32_t TIMER_PERIOD_MSEC = 500;    //毫秒
constexpr int32_t HEARTBEAT_TIMER_PERIOD_MSEC = 1000;   //毫秒
constexpr int32_t ELECTION_TIMER_PERIOD_MSEC = 2 * HEARTBEAT_TIMER_PERIOD_MSEC; //毫秒
constexpr int32_t LEASE_MAINTENANCE_GRACE = 1000;   //毫秒

std::string pg_id_to_name(uint64_t pool_id, uint64_t pg_id){
    char name[128];
    
    snprintf(name, sizeof(name), "%lu.%lu", pool_id, pg_id);
    return name;
}

void pg_t::free_pg(){
    //可能还需要其它处理 ？

    spdk_poller_unregister(&timer);
    raft->raft_destroy();
    raft.reset();
}

/** Raft callback for handling periodic logic */
static int periodic_func(void* arg){
    pg_t* pg = (pg_t*)arg;
	// SPDK_NOTICELOG("_periodic in core %u\n", spdk_env_get_current_core());
    pg->raft->raft_periodic();
    return 0;
}

void pg_t::start_raft_periodic_timer(){
    timer = SPDK_POLLER_REGISTER(periodic_func, this, TIMER_PERIOD_MSEC * 1000);
	raft->raft_set_election_timeout(ELECTION_TIMER_PERIOD_MSEC);
    raft->raft_set_lease_maintenance_grace(LEASE_MAINTENANCE_GRACE);
    raft->raft_set_heartbeat_timeout(HEARTBEAT_TIMER_PERIOD_MSEC);
}

static raft_time_t get_time(){
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    return now_ms.time_since_epoch().count();
}

static int recv_installsnapshot(
       raft_server_t* raft,
       void *user_data,
       raft_node* node,
       const msg_installsnapshot_t* msg,
       msg_installsnapshot_response_t* r){
    (void)raft;
    (void)user_data;
    (void)node;
    (void)msg;
    (void)r;
    return 0;    
}

static int recv_installsnapshot_response(
        raft_server_t* raft,
        void *user_data,
        raft_node* node,
        msg_installsnapshot_response_t* r){
    (void)raft;
    (void)user_data;
    (void)node;
    (void)r;
    return 0;        
}

static int persist_vote(
        raft_server_t* raft,
        void *user_data,
        raft_node_id_t vote){
    (void)raft;
    (void)user_data;
    (void)vote;
    return 0;        
}

static int persist_term(
        raft_server_t* raft,
        void *user_data,
        raft_term_t term,
        raft_node_id_t vote){
    (void)raft;
    (void)user_data;
    (void)term;
    (void)vote;
    return 0;            
}

int log_get_node_id(
        raft_server_t* raft,
        void *user_data,
        raft_entry_t *entry,
        raft_index_t entry_idx){
    (void)raft;
    (void)user_data;
    (void)entry;
    (void)entry_idx;
    return 0;                 
}

static int node_has_sufficient_logs(
        raft_server_t* raft,
        void *user_data,
        raft_node* node){
    (void)raft;
    (void)user_data;
    (void)node;
    return 0;          
}

static void notify_membership_event(
        raft_server_t* raft,
        void *user_data,
        raft_node *node,
        raft_entry_t *entry,
        raft_membership_e type){
    (void)raft;
    (void)user_data;
    (void)node;
    (void)entry;
    (void)type;         
}


raft_cbs_t raft_funcs = {
    .recv_installsnapshot = recv_installsnapshot,
    .recv_installsnapshot_response = recv_installsnapshot_response,
    .persist_vote = persist_vote,
    .persist_term = persist_term,
    .log_get_node_id = log_get_node_id,
    .node_has_sufficient_logs = node_has_sufficient_logs,
    .notify_membership_event = notify_membership_event,
    .get_time = get_time
};

int pg_group_t::create_pg(std::shared_ptr<state_machine> sm_ptr,  uint32_t shard_id, uint64_t pool_id, 
            uint64_t pg_id, std::vector<osd_info_t>&& osds, disk_log* log){
    int ret = 0;
    auto raft = raft_new(_client, log, sm_ptr, pool_id, pg_id);
    raft->raft_set_callbacks(&raft_funcs, NULL);

    ret = _pg_add(shard_id, raft, pool_id, pg_id);
    if(ret != 0){
        return ret; 
    }

    for(auto& osd : osds){
        SPDK_NOTICELOG("-- raft_add_node node %d in node %d ---\n", osd.node_id, get_current_node_id());
        if(osd.node_id == get_current_node_id()){
            raft->raft_add_node(NULL, osd.node_id, true);
        }else{
            /*
               这里需要连接osd。raft_add_node函数的user_data参数可以是osd连接的接口
            */

            raft->raft_add_node(NULL, osd.node_id, false);
        }
    }

    raft->raft_set_current_term(1);
    auto pg = get_pg(shard_id, pool_id, pg_id);

    /*  这里需要与其它osd建立链接，链接信息保存在raft_server_t中 ？ */

    pg->start_raft_periodic_timer();

    return 0;
}

void pg_group_t::delete_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id){
    _pg_remove(shard_id, pool_id, pg_id);
}


