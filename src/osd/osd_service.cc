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
#include "fastblock/msg/rpc_controller.h"
#include "fastblock/utils/utils.h"
#include "fastblock/utils/err_num.h"

#include <spdk/env.h>

#include <infiniband/verbs.h>

#include <errno.h>

osd_service::write_ring_slot::write_ring_slot(write_ring_slot&& other) noexcept
  : data(other.data)
  , mr(other.mr)
  , size(other.size) {
    other.data = nullptr;
    other.mr = nullptr;
    other.size = 0;
}

auto osd_service::write_ring_slot::operator=(write_ring_slot&& other) noexcept -> write_ring_slot& {
    if (this == &other) {
        return *this;
    }

    if (mr) {
        ::ibv_dereg_mr(mr);
    }
    if (data) {
        ::spdk_free(data);
    }

    data = other.data;
    mr = other.mr;
    size = other.size;
    other.data = nullptr;
    other.mr = nullptr;
    other.size = 0;
    return *this;
}

osd_service::write_ring_slot::~write_ring_slot() noexcept {
    if (mr) {
        ::ibv_dereg_mr(mr);
    }
    if (data) {
        ::spdk_free(data);
    }
}

std::shared_ptr<osd_service::write_ring_queue> osd_service::create_write_ring(
  ::ibv_pd* pd,
  std::string peer_address,
  const uint32_t slot_count,
  const uint32_t slot_size,
  const uint64_t lease_us) {
    auto queue = std::make_shared<write_ring_queue>();
    queue->queue_id = _next_write_ring_id.fetch_add(1);
    queue->lease_us = lease_us;
    queue->slot_size = slot_size;
    queue->peer_address = std::move(peer_address);
    queue->lease_deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(lease_us);
    queue->slots.reserve(slot_count);

    for (uint32_t i = 0; i < slot_count; ++i) {
        auto alloc_size = utils::align_up<uint64_t>(slot_size, 4096);
        auto* data = ::spdk_zmalloc(
          alloc_size, 0x1000, nullptr, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
        if (!data) {
            return nullptr;
        }

        auto* mr = ::ibv_reg_mr(
          pd,
          data,
          alloc_size,
          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!mr) {
            ::spdk_free(data);
            return nullptr;
        }

        write_ring_slot slot;
        slot.data = data;
        slot.mr = mr;
        slot.size = slot_size;
        queue->slots.push_back(std::move(slot));
    }

    std::scoped_lock lk(_write_ring_mutex);
    _write_rings.emplace(queue->queue_id, queue);
    if (!queue->peer_address.empty()) {
        _write_ring_owners[queue->peer_address] = queue->queue_id;
    }
    return queue;
}

std::shared_ptr<osd_service::write_ring_queue> osd_service::get_write_ring(const uint64_t queue_id) {
    std::scoped_lock lk(_write_ring_mutex);
    auto it = _write_rings.find(queue_id);
    if (it == _write_rings.end()) {
        return nullptr;
    }

    if (it->second->lease_deadline <= std::chrono::steady_clock::now()) {
        if (!it->second->peer_address.empty()) {
            auto owner_it = _write_ring_owners.find(it->second->peer_address);
            if (owner_it != _write_ring_owners.end() && owner_it->second == queue_id) {
                _write_ring_owners.erase(owner_it);
            }
        }
        _write_rings.erase(it);
        return nullptr;
    }

    it->second->lease_deadline =
      std::chrono::steady_clock::now() + std::chrono::microseconds(it->second->lease_us);

    return it->second;
}

std::shared_ptr<osd_service::write_ring_queue> osd_service::acquire_write_ring(
  ::ibv_pd* pd,
  const std::string& peer_address,
  const uint32_t slot_count,
  const uint32_t slot_size,
  const uint64_t lease_us) {
    if (peer_address.empty()) {
        return create_write_ring(pd, {}, slot_count, slot_size, lease_us);
    }

    {
        std::scoped_lock lk(_write_ring_mutex);
        auto owner_it = _write_ring_owners.find(peer_address);
        if (owner_it != _write_ring_owners.end()) {
            auto queue_it = _write_rings.find(owner_it->second);
            if (queue_it != _write_rings.end()) {
                auto& queue = queue_it->second;
                if (queue->lease_deadline > std::chrono::steady_clock::now()
                    && queue->slots.size() == slot_count
                    && queue->slot_size == slot_size) {
                    queue->lease_us = lease_us;
                    queue->lease_deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(lease_us);
                    SPDK_INFOLOG(
                      osd,
                      "reuse write ring queue %lu for peer %s\n",
                      queue->queue_id,
                      peer_address.c_str());
                    return queue;
                }
            }

            if (queue_it != _write_rings.end()) {
                _write_rings.erase(queue_it);
            }
            _write_ring_owners.erase(owner_it);
        }
    }

    SPDK_INFOLOG(osd, "create write ring for peer %s\n", peer_address.c_str());
    return create_write_ring(pd, peer_address, slot_count, slot_size, lease_us);
}

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

    SPDK_INFOLOG(osd, "recv pg_leader_request, get the leader of pg %lu.%lu\n", request->pool_id(), request->pg_id());
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
    auto res = _monitor_client->get_osd_addr(leader_id, shard_id);
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
        auto new_pg_done = [this, response, done](void *arg, int perrno){
            response->set_state(perrno);
            done->Run();
        };
        _pm->create_partition(pool_id, pg_id, 0, std::move(osds), pool_version, std::move(new_pg_done), nullptr);
    }
}

void osd_service::process_acquire_write_ring(
  google::protobuf::RpcController* controller,
  const osd::acquire_write_ring_request* request,
  osd::acquire_write_ring_response* response,
  google::protobuf::Closure* done) {
    auto* rdma_ctrl = dynamic_cast<msg::rdma::rpc_controller*>(controller);
    if (!rdma_ctrl || !rdma_ctrl->pd()) {
        response->set_state(-EINVAL);
        done->Run();
        return;
    }

    auto slot_count = request->slot_count() == 0 ? 16 : request->slot_count();
    auto slot_size = request->slot_size() == 0 ? 256 * 1024 : request->slot_size();
    auto lease_us = request->lease_us() == 0 ? 30ULL * 1000ULL * 1000ULL : request->lease_us();
    auto queue = acquire_write_ring(
      rdma_ctrl->pd(),
      rdma_ctrl->peer_address(),
      slot_count,
      slot_size,
      lease_us);
    if (!queue) {
        response->set_state(-ENOMEM);
        done->Run();
        return;
    }

    response->set_state(err::E_SUCCESS);
    response->set_queue_id(queue->queue_id);
    response->set_lease_us(queue->lease_us);
    for (uint32_t i = 0; i < queue->slots.size(); ++i) {
        auto* slot = response->add_slots();
        slot->set_slot_index(i);
        slot->set_remote_addr(reinterpret_cast<uint64_t>(queue->slots[i].mr->addr));
        slot->set_remote_key(queue->slots[i].mr->rkey);
        slot->set_slot_size(queue->slots[i].size);
    }
    done->Run();
}

namespace {

class ring_write_done : public google::protobuf::Closure {
public:
    ring_write_done(std::unique_ptr<osd::write_request> request, google::protobuf::Closure* done)
      : _request(std::move(request))
      , _done(done) {}

    void Run() override {
        _done->Run();
        delete this;
    }

private:
    std::unique_ptr<osd::write_request> _request{};
    google::protobuf::Closure* _done{nullptr};
};

} // namespace

void osd_service::process_ring_write(
  std::unique_ptr<osd::write_request> request,
  osd::write_reply* response,
  google::protobuf::Closure* done) {
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
      [this, request = std::move(request), response, done, shard_id]() mutable {
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
        auto* request_ptr = request.get();
        auto* ring_done = new ring_write_done(std::move(request), done);
        process(osd_stm_p, request_ptr, response, ring_done);
      });
}

void osd_service::process_commit_ring_write(
  google::protobuf::RpcController* controller,
  const osd::commit_ring_write_request* request,
  osd::write_reply* response,
  google::protobuf::Closure* done) {
    (void)controller;

    SPDK_NOTICELOG(
      "process commit ring write, queue id %lu, slot %u, serialized bytes %u\n",
      request->queue_id(),
      request->slot_index(),
      request->serialized_size());

    auto queue = get_write_ring(request->queue_id());
    if (!queue) {
        response->set_state(-ENOENT);
        done->Run();
        return;
    }

    if (request->slot_index() >= queue->slots.size()) {
        response->set_state(-EINVAL);
        done->Run();
        return;
    }

    auto& slot = queue->slots[request->slot_index()];
    if (request->serialized_size() > slot.size) {
        response->set_state(-EMSGSIZE);
        done->Run();
        return;
    }

    auto write_req = std::make_unique<osd::write_request>();
    if (!write_req->ParseFromArray(slot.data, request->serialized_size())) {
        response->set_state(-EBADMSG);
        done->Run();
        return;
    }

    process_ring_write(std::move(write_req), response, done);
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
