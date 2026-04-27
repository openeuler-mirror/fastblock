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
#pragma once

#include "fastblock/rpc/osd_msg.pb.h"
#include "partition_manager.h"
#include "mon_client.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ibv_mr;
struct ibv_pd;

class osd_service : public osd::rpc_service_osd
{
public:
    struct write_ring_slot {
        void* data{nullptr};
        ::ibv_mr* mr{nullptr};
        uint32_t size{0};

        write_ring_slot() = default;
        write_ring_slot(const write_ring_slot&) = delete;
        write_ring_slot& operator=(const write_ring_slot&) = delete;
        write_ring_slot(write_ring_slot&& other) noexcept;
        write_ring_slot& operator=(write_ring_slot&& other) noexcept;
        ~write_ring_slot() noexcept;
    };

    struct write_ring_queue {
        uint64_t queue_id{0};
        uint64_t lease_us{0};
        uint32_t slot_size{0};
        std::string peer_address{};
        std::chrono::steady_clock::time_point lease_deadline{};
        std::vector<write_ring_slot> slots{};
    };

    osd_service(partition_manager *pm, std::shared_ptr<monitor_client> mon_cli)
        : _pm(pm), _monitor_client{mon_cli} {}

    void process_write(google::protobuf::RpcController *controller,
                       const osd::write_request *request,
                       osd::write_reply *response,
                       google::protobuf::Closure *done) override
    {
        process<osd::write_request, osd::write_reply>(request, response, done);
    }

    void process_read(google::protobuf::RpcController *controller,
                      const osd::read_request *request,
                      osd::read_reply *response,
                      google::protobuf::Closure *done) override
    {
        process<osd::read_request, osd::read_reply>(request, response, done);
    }

    void process_delete(google::protobuf::RpcController *controller,
                        const osd::delete_request *request,
                        osd::delete_reply *response,
                        google::protobuf::Closure *done) override
    {
        process<osd::delete_request, osd::delete_reply>(request, response, done);
    }

    void process_rpc_bench(google::protobuf::RpcController *controller,
                           const osd::bench_request *request,
                           osd::bench_response *response,
                           google::protobuf::Closure *done) override;
    void process_get_leader(google::protobuf::RpcController *controller,
                            const osd::pg_leader_request *request,
                            osd::pg_leader_response *response,
                            google::protobuf::Closure *done) override;

    void process_create_pg(google::protobuf::RpcController *controller,
                            const osd::create_pg_request *request,
                            osd::create_pg_response *response,
                            google::protobuf::Closure *done) override;

    void process_add_node(google::protobuf::RpcController* controller,
                         const osd::add_node_request* request,
                         osd::add_node_response* response,
                         google::protobuf::Closure* done) override {
        process<osd::add_node_request, osd::add_node_response>(request, response, done);
    }

    void process_remove_node(google::protobuf::RpcController* controller,
                         const osd::remove_node_request* request,
                         osd::remove_node_response* response,
                         google::protobuf::Closure* done) override {
        process<osd::remove_node_request, osd::remove_node_response>(request, response, done);
    }

    void process_change_nodes(google::protobuf::RpcController* controller,
                         const osd::change_nodes_request* request,
                         osd::change_nodes_response* response,
                         google::protobuf::Closure* done) override {
        process<osd::change_nodes_request, osd::change_nodes_response>(request, response, done);
    }

    void process_acquire_write_ring(google::protobuf::RpcController* controller,
                         const osd::acquire_write_ring_request* request,
                         osd::acquire_write_ring_response* response,
                         google::protobuf::Closure* done) override;

    void process_commit_ring_write(google::protobuf::RpcController* controller,
                         const osd::commit_ring_write_request* request,
                         osd::write_reply* response,
                         google::protobuf::Closure* done) override;

    template <typename request_type, typename reply_type>
    void process(const request_type *request, reply_type *response, google::protobuf::Closure *done);

    void process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::write_request *request,
        osd::write_reply *response,
        google::protobuf::Closure *done);
    void process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::read_request *request,
        osd::read_reply *response,
        google::protobuf::Closure *done);
    void process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::delete_request *request,
        osd::delete_reply *response,
        google::protobuf::Closure *done);

    void process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::add_node_request* request,
        osd::add_node_response* response,
        google::protobuf::Closure* done);

    void process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::remove_node_request* request,
        osd::remove_node_response* response,
        google::protobuf::Closure* done);

    void process(
        std::shared_ptr<osd_stm> osd_stm_p,
        const osd::change_nodes_request* request,
        osd::change_nodes_response* response,
        google::protobuf::Closure* done);

    void process_ring_write(
        std::unique_ptr<osd::write_request> request,
        osd::write_reply* response,
        google::protobuf::Closure* done);

private:
    std::shared_ptr<write_ring_queue> create_write_ring(
      ::ibv_pd* pd,
      std::string peer_address,
      uint32_t slot_count,
      uint32_t slot_size,
      uint64_t lease_us);

    std::shared_ptr<write_ring_queue> get_write_ring(uint64_t queue_id);

    std::shared_ptr<write_ring_queue> acquire_write_ring(
      ::ibv_pd* pd,
      const std::string& peer_address,
      uint32_t slot_count,
      uint32_t slot_size,
      uint64_t lease_us);

private:
    partition_manager *_pm;
    std::shared_ptr<monitor_client> _monitor_client{nullptr};
    std::mutex _write_ring_mutex{};
    std::unordered_map<uint64_t, std::shared_ptr<write_ring_queue>> _write_rings{};
    std::unordered_map<std::string, uint64_t> _write_ring_owners{};
    std::atomic<uint64_t> _next_write_ring_id{1};
};

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
