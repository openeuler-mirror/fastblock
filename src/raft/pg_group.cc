#include "pg_group.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/util.h"

std::string pg_id_to_name(uint64_t pool_id, uint64_t pg_id){
    char name[128];
    
    snprintf(name, sizeof(name), "%lu.%lu", pool_id, pg_id);
    return name;
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
    .log_get_node_id = log_get_node_id,
    .node_has_sufficient_logs = node_has_sufficient_logs,
    .notify_membership_event = notify_membership_event,
    .get_time = get_time
};

int pg_group_t::create_pg(std::shared_ptr<state_machine> sm_ptr,  uint32_t shard_id, uint64_t pool_id, 
            uint64_t pg_id, std::vector<osd_info_t>&& osds, disk_log* log){
    int ret = 0;
    auto raft = raft_new(_client, log, sm_ptr, pool_id, pg_id
                                        #ifdef KVSTORE
                                               , _kvs[shard_id]
                                        #endif                        
                                            );
    raft->raft_set_callbacks(&raft_funcs, NULL);

    _pg_add(shard_id, raft, pool_id, pg_id);

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
    
    raft->start_raft_timer();
    return 0;
}

void pg_group_t::delete_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id){
    _pg_remove(shard_id, pool_id, pg_id);
}

#ifdef KVSTORE
struct make_kvstore_context{
	context *complete;
    pg_group_t* pg;
    int num;
    int count;
    int rberrno;
};

static void make_kvstore_done(void *arg, kv_store* kv, int rberrno){
    make_kvstore_context* mkc = (make_kvstore_context*)arg;
    context *complete = mkc->complete;
    pg_group_t* pg = mkc->pg;

    mkc->num++;
    SPDK_NOTICELOG("make_kvstore_done, rberrno %d\n", rberrno);
    if(rberrno){
        mkc->rberrno = rberrno;
    }else{
        pg->add_kvstore(kv);
    }
    if(mkc->num == mkc->count){
        if(mkc->rberrno)
            complete->complete(mkc->rberrno);
        else
            complete->complete(rberrno);
        delete mkc;
    }
}
#endif

void pg_group_t::start(context *complete){
#ifdef KVSTORE
    uint32_t i = 0;
    auto shard_num = _shard_cores.size();
    make_kvstore_context *ctx = new make_kvstore_context{complete, this, 0, shard_num, 0};
    for(i = 0; i < shard_num; i++){
        make_kvstore(global_blobstore(), global_io_channel(), make_kvstore_done, ctx);
    }
#else
    complete->complete(0);
#endif
}

