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
#pragma once
#include "raft/pg_group.h"
#include "osd/osd_stm.h"
#include "base/core_sharded.h"
#include "raft/pg_group.h"

#include <memory>

struct shard_revision {
    uint32_t _shard;
    int64_t _revision;
};

enum class osd_state {
    OSD_STARTING, 
    OSD_ACTIVE, 
    OSD_DOWN
};

class partition_manager : public std::enable_shared_from_this<partition_manager> {
public:
    partition_manager(int node_id)
      : _pgs(node_id)
      , _next_shard(0)
      , _shard(core_sharded::get_core_sharded())
      , _shard_cores(get_shard_cores())
      , _state(osd_state::OSD_STARTING) {
          uint32_t i = 0;
          auto shard_num = _shard_cores.size();
          for(i = 0; i < shard_num; i++){
              _sm_table.push_back(std::map<std::string, std::shared_ptr<osd_stm>>());
          }
      }
      
    void start(utils::context *complete);

    void stop(utils::complete_fun fun, void *arg){
        if(_state == osd_state::OSD_DOWN){
            return;
        }  
        _state = osd_state::OSD_DOWN;  

        uint64_t shard_id = 0;
        auto shard_num = _sm_table.size();
        utils::multi_complete *complete = new utils::multi_complete(shard_num, fun, arg);

        for(shard_id = 0; shard_id < shard_num; shard_id++){
            _shard.invoke_on(
              shard_id,
              [this, shard_id, complete](){
                _pgs.stop(shard_id);

                for(auto& [name, stm] : _sm_table[shard_id]){
                    stm->stop(
                      [](void *, int){}, 
                      nullptr);
                }
                complete->complete(0);
              });
        }
    }

    int create_partition(uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t>&& osds, int64_t revision_id);
    int delete_partition(uint64_t pool_id, uint64_t pg_id);

    bool get_pg_shard(uint64_t pool_id, uint64_t pg_id, uint32_t &shard_id);

    void create_pg(uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t> osds, uint32_t shard_id, int64_t revision_id);
    void delete_pg(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id);

    std::shared_ptr<osd_stm> get_osd_stm(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        if(_sm_table[shard_id].count(name) == 0)
            return nullptr;
        return _sm_table[shard_id][name];
    }

    std::shared_ptr<raft_server_t> get_pg(uint32_t shard_id, uint64_t pool_id, uint64_t pg_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        return _pgs.get_pg(shard_id, name);
    }

    core_sharded& get_shard(){
        return _shard;
    }

    pg_group_t& get_pg_group(){
        return _pgs;
    }

    void add_osd_stm(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id, std::shared_ptr<osd_stm> sm){
        auto name = pg_id_to_name(pool_id, pg_id);
        _sm_table[shard_id][std::move(name)] = sm;
    }

    void del_osd_stm(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id){
        auto name = pg_id_to_name(pool_id, pg_id);
        auto iter = _sm_table[shard_id].find(name);
        if(iter == _sm_table[shard_id].end())
            return;
        auto stm = iter->second;
        stm->stop(
          [this, shard_id, name](void *, int){
            _sm_table[shard_id].erase(name);   
          },
          nullptr
        );     
    }

    int get_current_node_id(){
        return _pgs.get_current_node_id();
    }

    void set_osd_state(osd_state state){
        _state = state;
    }

    int change_pg_membership(uint64_t pool_id, uint64_t pg_id, std::vector<utils::osd_info_t> new_osds, utils::context* complete);

private:
    int osd_state_is_not_active();
    uint32_t get_next_shard_id(){
        uint32_t shard_id = _next_shard;
        _next_shard = (_next_shard + 1) % _shard_cores.size();
        return shard_id;
    }

    int _add_pg_shard(uint64_t pool_id, uint64_t pg_id, uint32_t shard_id, int64_t revision_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        _shard_table[std::move(name)] = shard_revision{shard_id, revision_id};
        return 0;
    }

    int _remove_pg_shard(uint64_t pool_id, uint64_t pg_id){
        std::string name = pg_id_to_name(pool_id, pg_id);
        auto ret = _shard_table.erase(std::move(name));
        if(ret == 0)
            return -EEXIST;
        return 0;
    }

    pg_group_t _pgs;
    //记录pg到cpu核的对应关系
    std::map<std::string, shard_revision> _shard_table;
    uint32_t _next_shard;
    core_sharded&  _shard;
    std::vector<uint32_t> _shard_cores;
    std::vector<std::map<std::string, std::shared_ptr<osd_stm>>> _sm_table;
    osd_state _state;
};
