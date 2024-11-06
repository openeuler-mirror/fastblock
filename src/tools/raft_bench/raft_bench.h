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
#include "raft/pg_group.h"
#include "base/core_sharded.h"
#include "osd/osd_stm.h"

class pg_manager
{
public:
    pg_manager(int node_id)
    : _next_shard(0)
    , _pgs(node_id, nullptr)
    , _shard_cores(core_sharded::get_shard_cores()) {
        auto shard_num = _shard_cores.size();
        for(uint32_t i = 0; i < shard_num; i++){
            _sm_table.push_back(std::map<std::string, std::shared_ptr<osd_stm>>());
        }
    }

    void start(utils::context *complete){
        auto cb = [](void *arg, int rerror){
            utils::context *complete = (utils::context *)arg;
            complete->complete(rerror);
        };

        auto ctx = new utils::switch_core_context{.cb_fn = std::move(cb), .arg = complete, .thread = spdk_get_thread(), .serror = 0};
        _pgs.start(
            [](void *arg, int){
                utils::switch_core_context * ctx = (utils::switch_core_context *)arg;
                utils::switch_core_func(ctx, err::E_SUCCESS);
            },
            ctx
        );
    }

    void stop_stm(uint64_t shard_id, utils::complete_fun fun, void *arg) {
        if(_sm_table[shard_id].size() == 0){
            fun(arg, 0);
            return;
        }
        utils::multi_complete *complete = new utils::multi_complete(_sm_table[shard_id].size(), 1, fun, arg);
        for(auto& [_, stm] : _sm_table[shard_id]){
            stm->stop(utils::complete_done, complete);
        }
    }

    void stop(utils::complete_fun fun, void *arg) {
        uint64_t shard_id = 0;
        auto shard_num = _sm_table.size();
        if(shard_num == 0){
            fun(arg, 0);
            return;
        }

        auto &shard = core_sharded::get_core_sharded();
        utils::multi_complete *complete = new utils::multi_complete(shard_num, _shard_cores.size(), fun, arg);
        for(shard_id = 0; shard_id < shard_num; shard_id++){
            shard.invoke_on(
              shard_id,
              [this, shard_id, complete](){
    
                _pgs.stop(
                  shard_id,
                  [this, shard_id](void *arg, int serrno){
                    utils::multi_complete *complete = (utils::multi_complete *)arg;
                    if(serrno != 0){
                        complete->complete(serrno);
                        return;
                    }
    
                    stop_stm(shard_id, utils::complete_done, complete);
                  },
                  complete);
              });
        }        
    }

    uint32_t get_next_shard_id(){
        uint32_t shard_id = _next_shard;
        _next_shard = (_next_shard + 1) % _shard_cores.size();
        return shard_id;
    }

    void create_pg(uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t> osds, uint32_t shard_id, utils::context *ctx);

    void add_osd_stm(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id, std::shared_ptr<osd_stm> sm){
        auto name = pg_id_to_name(pool_id, pg_id);
        _sm_table[shard_id][std::move(name)] = sm;
    }

    std::shared_ptr<osd_stm> get_osd_stm(uint32_t shard_id, std::string& pg_name){
        if(_sm_table[shard_id].count(pg_name) == 0)
            return nullptr;
        return _sm_table[shard_id][pg_name];
    }

    pg_group_t& get_pg_group(){
        return _pgs;
    }

private:
    uint32_t _next_shard;
    pg_group_t _pgs;
    std::vector<uint32_t> _shard_cores;
    std::map<std::string, uint32_t> _shard_table;
    std::vector<std::map<std::string, std::shared_ptr<osd_stm>>> _sm_table;
};
