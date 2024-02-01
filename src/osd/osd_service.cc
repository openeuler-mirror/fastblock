/* Copyright (c) 2023-2024 ChinaUnicom
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