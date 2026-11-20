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
#include "fastblock/utils/utils.h"
#include "fastblock/utils/err_num.h"

#include <spdk/env.h>

#include <condition_variable>
#include <errno.h>
#include <mutex>




void osd_service::process_rpc_bench(google::protobuf::RpcController *controller,
                                    const osd::bench_request *request,
                                    osd::bench_response *response,
                                    google::protobuf::Closure *done)
{
    response->set_resp(request->req());
    done->Run();
}

osd_service::leader_endpoint osd_service::resolve_pg_leader(
  const uint64_t pool_id,
  const uint64_t pg_id,
  const bool use_raw_port) {
    leader_endpoint endpoint{};
    uint32_t shard_id{};

    if(!_pm->get_pg_shard(pool_id, pg_id, shard_id)){
        SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
        endpoint.state = err::RAFT_ERR_NOT_FOUND_PG;
        return endpoint;
    }

    // If already on home SPDK thread, resolve inline (no hop).
    if (rtc::is_on_home_spdk_thread(shard_id)) {
        auto raft = _pm->get_pg(shard_id, pool_id, pg_id);
        if(!raft){
            SPDK_WARNLOG("not find pg %lu.%lu\n", pool_id, pg_id);
            endpoint.state = err::RAFT_ERR_NOT_FOUND_PG;
            return endpoint;
        }
        endpoint.leader_id = raft->raft_get_current_leader();
        auto leader = use_raw_port
          ? _monitor_client->get_osd_raw_addr(endpoint.leader_id, shard_id)
          : _monitor_client->get_osd_addr(endpoint.leader_id, shard_id);
        if(leader.first.empty() || leader.second == 0){
            endpoint.state = err::RAFT_ERR_NOT_FOUND_LEADER;
            endpoint.leader_id = -1;
            return endpoint;
        }
        endpoint.state = err::E_SUCCESS;
        endpoint.leader_addr = std::move(leader.first);
        endpoint.leader_port = leader.second;
        return endpoint;
    }

    // Off-home (including raw std::thread): hop to home + block until done.
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    leader_endpoint result{};

    rtc::global_metrics().leader_lookup_hops.fetch_add(1, std::memory_order_relaxed);
    const int rc = _pm->get_shard().invoke_on(
      shard_id,
      [this, pool_id, pg_id, shard_id, use_raw_port, &mu, &cv, &ready, &result]() {
        leader_endpoint ep{};
        auto raft = _pm->get_pg(shard_id, pool_id, pg_id);
        if (!raft) {
            ep.state = err::RAFT_ERR_NOT_FOUND_PG;
        } else {
            ep.leader_id = raft->raft_get_current_leader();
            auto leader = use_raw_port
              ? _monitor_client->get_osd_raw_addr(ep.leader_id, shard_id)
              : _monitor_client->get_osd_addr(ep.leader_id, shard_id);
            if (leader.first.empty() || leader.second == 0) {
                ep.state = err::RAFT_ERR_NOT_FOUND_LEADER;
                ep.leader_id = -1;
            } else {
                ep.state = err::E_SUCCESS;
                ep.leader_addr = std::move(leader.first);
                ep.leader_port = leader.second;
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            result = std::move(ep);
            ready = true;
        }
        cv.notify_one();
      });
    if (rc != 0) {
        SPDK_ERRLOG(
          "failed to dispatch get_leader for pg %lu.%lu to shard %u, rc %d\n",
          pool_id,
          pg_id,
          shard_id,
          rc);
        result.state = err::RAFT_ERR_UNKNOWN;
        return result;
    }

    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&ready]() { return ready; });
    return result;
}

void osd_service::process_get_leader(google::protobuf::RpcController* controller,
            const osd::pg_leader_request* request,
            osd::pg_leader_response* response,
            google::protobuf::Closure* done){
    SPDK_INFOLOG(osd, "recv pg_leader_request, get the leader of pg %lu.%lu\n", request->pool_id(), request->pg_id());
    uint32_t shard_id{};
    if (!_pm->get_pg_shard(request->pool_id(), request->pg_id(), shard_id)) {
        SPDK_WARNLOG("not find pg %lu.%lu\n", request->pool_id(), request->pg_id());
        response->set_state(err::RAFT_ERR_NOT_FOUND_PG);
        done->Run();
        return;
    }

    // RTC PR-3: only read raft on home SPDK thread; done->Run on home.
    const int rc = rtc::local_first(
      _pm->get_shard(),
      shard_id,
      [this, request, response, done, shard_id]() {
        auto raft = _pm->get_pg(shard_id, request->pool_id(), request->pg_id());
        if (!raft) {
            SPDK_WARNLOG("not find pg %lu.%lu\n", request->pool_id(), request->pg_id());
            response->set_state(err::RAFT_ERR_NOT_FOUND_PG);
            done->Run();
            return;
        }

        const int32_t leader_id = raft->raft_get_current_leader();
        auto leader = _monitor_client->get_osd_addr(leader_id, shard_id);
        if (leader.first.empty() || leader.second == 0) {
            response->set_state(err::RAFT_ERR_NOT_FOUND_LEADER);
            done->Run();
            return;
        }

        response->set_state(err::E_SUCCESS);
        response->set_leader_id(leader_id);
        response->set_leader_addr(leader.first);
        response->set_leader_port(leader.second);
        done->Run();
      },
      rtc::hop_kind::leader_lookup);
    if (rc != 0) {
        SPDK_ERRLOG(
          "failed to dispatch get_leader for pg %lu.%lu to shard %u, rc %d\n",
          request->pool_id(),
          request->pg_id(),
          shard_id,
          rc);
        response->set_state(err::RAFT_ERR_UNKNOWN);
        done->Run();
    }
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
    }

    const uint32_t core_index = request->coreindex();
    std::vector<utils::osd_info_t> osds;
    osds.reserve(request->nodes_size());
    for (const auto& node : request->nodes()) {
        utils::osd_info_t osd_info;
        osd_info.node_id = node.node_id();
        osd_info.isin = true;
        osd_info.isup = true;
        osd_info.ispendingcreate = false;
        osd_info.address = node.addr();
        osd_info.sharded_ports[core_index] = utils::core_shard_map{
          .port = static_cast<uint32_t>(node.port()),
          .raw_port = 0,
          .core_id = core_index,
          .shard_id = core_index};
        osds.emplace_back(std::move(osd_info));
    }
    SPDK_INFOLOG(osd,
      "create pg %lu.%lu via create_pg RPC on coreindex %u\n",
      pool_id,
      pg_id,
      core_index);
    auto new_pg_done = [response, done](void *arg, int perrno){
        (void)arg;
        response->set_state(perrno);
        done->Run();
    };
    _pm->create_partition(
      pool_id,
      pg_id,
      core_index,
      std::move(osds),
      pool_version,
      std::move(new_pg_done),
      nullptr);
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

void osd_service::process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::image_snap_request* request,
        osd::image_snap_reply* response,
        google::protobuf::Closure* done) {
    osd_stm_p->image_snap_and_wait(request, response, done);
}
