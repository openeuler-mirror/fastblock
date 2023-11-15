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
#include "osd_service.h"
#include "utils/utils.h"
#include "utils/err_num.h"

void osd_service::process_rpc_bench(google::protobuf::RpcController *controller,
                                    const osd::bench_request *request,
                                    osd::bench_response *response,
                                    google::protobuf::Closure *done)
{
    response->set_resp(request->req());
    done->Run();
}

void osd_service::process_get_leader(google::protobuf::RpcController* controller,
            const osd::pg_leader_request* request,
            osd::pg_leader_response* response,
            google::protobuf::Closure* done){
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;

    if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
        response->set_state(err::RAFT_ERR_NOT_FOUND_PG);
        done->Run();
        return;
    }
    auto raft = _pm->get_pg(shard_id, pool_id, pg_id);
    if(!raft){
        SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
        response->set_state(err::RAFT_ERR_NOT_FOUND_PG);
        done->Run();
        return;
    }
    auto leader_id = raft->raft_get_current_leader();
    auto res = _monitor_client->get_osd_addr(leader_id);
    if(res.first.size() == 0){
        response->set_state(err::RAFT_ERR_NOT_FOUND_LEADER);
    }else{
        response->set_state(err::E_SUCCESS);
        response->set_leader_id(leader_id);
        response->set_leader_addr(res.first);
        response->set_leader_port(res.second);
    }
    done->Run();
}

void osd_service::process_create_pg(google::protobuf::RpcController *controller,
            const osd::create_pg_request *request,
            osd::create_pg_response *response,
            google::protobuf::Closure *done){
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    auto pool_version = request->vision_id();
    uint32_t shard_id;

    if(_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_INFOLOG(osd, "pg %lu.%lu already exist\n", pool_id, pg_id);
        response->set_state(err::E_SUCCESS);
        done->Run();
        return;        
    }else{
        std::vector<utils::osd_info_t> osds;
        SPDK_INFOLOG(osd, "create pg %lu.%lu\n", pool_id, pg_id);
        auto ret = _pm->create_partition(pool_id, pg_id, std::move(osds), pool_version);
        response->set_state(ret);
        done->Run();
    }    
}   

template<typename request_type, typename reply_type> 
void osd_service::process(const request_type* request, reply_type* response, google::protobuf::Closure* done){
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;

    if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_WARNLOG("not find pg %lu.%lu\n", request->pool_id(), request->pg_id());
        response->set_state(err::RAFT_ERR_NOT_FOUND_PG);
        done->Run();
        return;
    }  
    
    _pm->get_shard().invoke_on(
      shard_id,
      [this, request, response, done, shard_id](){
        auto raft = _pm->get_pg(shard_id, request->pool_id(), request->pg_id());
        if(!raft){
            SPDK_WARNLOG("not find pg %lu.%lu\n", request->pool_id(), request->pg_id());
            response->set_state(err::RAFT_ERR_NOT_FOUND_PG);
            done->Run();
            return;
        }
        if(!raft->raft_is_leader()){
            SPDK_WARNLOG("node %d is not the leader of pg %lu.%lu\n", 
                    raft->raft_get_nodeid(), request->pool_id(), request->pg_id());
            response->set_state(err::RAFT_ERR_NOT_LEADER);
            done->Run();
            return;
        }

        auto err_num = raft_state_to_errno(raft->raft_get_op_state());
        if(err_num != err::E_SUCCESS){
            SPDK_WARNLOG("hand osd request for pg %lu.%lu in node %d failed: %s\n", 
                    request->pool_id(), request->pg_id(), raft->raft_get_nodeid(), err::string_status(err_num));
            response->set_state(err_num);
            done->Run();
            return;                        
        }

        auto osd_stm_p = _pm->get_osd_stm(shard_id, request->pool_id(), request->pg_id());
        if(!osd_stm_p){
            SPDK_WARNLOG("not find pg %lu.%lu\n", request->pool_id(), request->pg_id());
            response->set_state(err::RAFT_ERR_NOT_FOUND_PG);
            done->Run();
            return;
        }
        process(osd_stm_p, request, response, done);
      });    
}

void osd_service::process(
        std::shared_ptr<osd_stm> osd_stm_p, 
        const osd::write_request* request, 
        osd::write_reply* response, 
        google::protobuf::Closure* done){
    osd_stm_p->write_and_wait(request, response, done);
}

void osd_service::process(
        std::shared_ptr<osd_stm> osd_stm_p, 
        const osd::read_request* request, 
        osd::read_reply* response, 
        google::protobuf::Closure* done){
    
    osd_stm_p->read_and_wait(request, response, done);
}

void osd_service::process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::delete_request* request, 
        osd::delete_reply* response, 
        google::protobuf::Closure* done){
    osd_stm_p->delete_and_wait(request, response, done);
}

template<typename response_type>
class membership_complete : public utils::context {
public:
    membership_complete(
            response_type* response, 
            google::protobuf::Closure* done, 
            std::vector<int32_t> new_nodes) 
    : _response(response)
    , _done(done)
    , _new_nodes(std::move(new_nodes)){}

    void finish(int r) override {
        SPDK_INFOLOG(osd, "finish change  membership, r %d\n", r);
        if(r == 0){
            for(auto node_id : _new_nodes){
                _response->add_new_nodes(node_id);
            }
        }
        _response->set_state(r);
        _done->Run();
    }
private:
    response_type* _response;
    google::protobuf::Closure* _done;
    std::vector<int32_t> _new_nodes;
};

void osd_service::process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::add_node_request* request, 
        osd::add_node_response* response, 
        google::protobuf::Closure* done){
    auto raft = osd_stm_p->get_raft();
    std::vector<raft_node_id_t> new_nodes = raft->raft_get_nodes_id();
    new_nodes.push_back(request->node().node_id());
    auto *complete = new membership_complete<osd::add_node_response>(response, done, std::move(new_nodes));
    raft->add_raft_membership(request->node(), complete);
}

void osd_service::process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::remove_node_request* request, 
        osd::remove_node_response* response, 
        google::protobuf::Closure* done){
    auto raft = osd_stm_p->get_raft();
    std::vector<raft_node_id_t> new_nodes = raft->raft_get_nodes_id();
    auto it = new_nodes.begin();
    bool found = false;
    while(it != new_nodes.end()){
        if(*it == request->node().node_id()){
            found = true;
            it = new_nodes.erase(it);
        }else 
            it++;
    }

    if(!found){
        response->set_state(err::RAFT_ERR_NO_FOUND_NODE);
        done->Run();
        return;
    }

    auto *complete = new membership_complete<osd::remove_node_response>(response, done, std::move(new_nodes));
    raft->remove_raft_membership(request->node(), complete);
}

void osd_service::process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::change_nodes_request* request, 
        osd::change_nodes_response* response, 
        google::protobuf::Closure* done){
    auto raft = osd_stm_p->get_raft();
    std::vector<int32_t> new_nodes;
    std::vector<raft_node_info> nodes;
    for(int i = 0; i < request->new_nodes_size(); i++){
        new_nodes.push_back(request->new_nodes(i).node_id());
        nodes.emplace_back(request->new_nodes(i));
    }

    auto *complete = new membership_complete<osd::change_nodes_response>(response, done, std::move(new_nodes));
    raft->change_raft_membership(std::move(nodes), complete);
}