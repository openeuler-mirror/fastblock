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

#include "state_machine.h"
#include "raft.h"
#include "spdk/log.h"
#include "utils/err_num.h"
#include "rpc/osd_msg.pb.h"

constexpr uint32_t default_parallel_apply_num = 32;

static int apply_task(void *arg){
    state_machine* stm = (state_machine *)arg;
    if(stm->get_raft()->raft_get_op_state() == raft_op_state::RAFT_INIT){
        return SPDK_POLLER_IDLE;
    }
    return stm->raft_apply_entry();
}

void state_machine::start(){
    SPDK_INFOLOG(pg_group, "pg %lu.%lu in shard %u\n",
      get_raft()->raft_get_pool_id(), get_raft()->raft_get_pg_id(), core_sharded::get_core_sharded().this_shard_id());
    _timer = SPDK_POLLER_REGISTER(&apply_task, this, 0);
}

// struct apply_complete : public utils::context{
    // apply_complete(raft_index_t start_idx, raft_index_t end_idx, state_machine* stm)
    // : utils::context(false)
    // , _start_idx(start_idx)
    // , _end_idx(end_idx)
    // , _stm(stm)
    // , _count(end_idx - start_idx + 1)
    // , _index(0)
    // , _result(0) {
        // 
    // }
// 
    // void finish_del(int r) override {
        // _index++;
        // if(r != err::E_SUCCESS){
            // SPDK_ERRLOG("apply log failed: %s\n", spdk_strerror(r));
            // _result = r;
        // }
        // if(_index == _count){
            // if(_result == 0){
                // SPDK_INFOLOG(pg_group, "in pg %lu.%lu, apply log [%ld, %ld]\n",
                    //    _stm->get_raft()->raft_get_pool_id(), _stm->get_raft()->raft_get_pg_id(), _start_idx, _end_idx);
                // auto last_applied_idx = _stm->get_last_applied_idx();
                // _stm->set_last_applied_idx(_end_idx);
                // _stm->get_raft()->raft_get_log()->raft_write_entry_finish(_start_idx, _end_idx, r);
                // _stm->get_raft()->raft_get_log()->set_applied_index(last_applied_idx, _end_idx);
            // }
            // _stm->set_apply_in_progress(false);
            // delete this;
        // }
    // }
// 
    // void finish(int ) override {}
// private:
    // raft_index_t _start_idx;
    // raft_index_t _end_idx;
    // state_machine* _stm;
    // int64_t     _count;
    // int64_t     _index;
    // int         _result;
// };

struct apply_context{
    apply_context(raft_index_t start_idx, raft_index_t end_idx, state_machine* stm)
    : _start_idx(start_idx)
    , _end_idx(end_idx)
    , _stm(stm)
    , _count(end_idx - start_idx + 1)
    , _index(0)
    , _result(0) {
        // SPDK_WARNLOG("--------------  apply count %ld\n", _count);
    }    

    void complete(raft_index_t index, int r){
        _index++;
        if(r != err::E_SUCCESS){
            // SPDK_ERRLOG("apply log failed: %s\n", spdk_strerror(r));
            _result = r;
        }
        // SPDK_WARNLOG("apply index %ld\n", index);
        _stm->get_raft()->raft_get_log()->raft_write_entry_finish(index, index, r);
        if(_index == _count){
            if(_result == 0){
                SPDK_INFOLOG(pg_group, "in pg %lu.%lu, apply log [%ld, %ld]\n",
                       _stm->get_raft()->raft_get_pool_id(), _stm->get_raft()->raft_get_pg_id(), _start_idx, _end_idx);
                auto last_applied_idx = _stm->get_last_applied_idx();
                _stm->set_last_applied_idx(_end_idx);
                // _stm->get_raft()->raft_get_log()->raft_write_entry_finish(_start_idx, _end_idx, r);
                _stm->get_raft()->raft_get_log()->set_applied_index(last_applied_idx, _end_idx);
            }
            _stm->set_apply_in_progress(false);
            delete this;
        }
    }
private:
    raft_index_t _start_idx;
    raft_index_t _end_idx;
    state_machine* _stm;
    int64_t     _count;
    int64_t     _index;
    int         _result;
};

static void apply_done(void *arg, int64_t index, int objerrno){
    apply_context *complete = (apply_context *)arg;
    complete->complete(index, objerrno);
}

int state_machine::raft_apply_entry()
{
    if (_raft->raft_get_snapshot_in_progress())
        return SPDK_POLLER_IDLE;

    auto cur_time = utils::get_time();
    if((cur_time - _last_save_time) / 1000 > 1 || _last_applied_idx - _last_save_index >= 100){
        if(_last_save_index != _last_applied_idx){
            SPDK_INFOLOG(pg_group, "pg %lu.%lu save last_apply_index %lu  g_last_index %lu\n",
                               get_raft()->raft_get_pool_id(), get_raft()->raft_get_pg_id(), _last_applied_idx, _last_save_index);
            _last_save_index = _last_applied_idx;
            _last_save_time = cur_time;
            get_raft()->save_last_apply_index(_last_applied_idx);
        }
    }

    /* Don't apply after the commit_idx */
    if (_last_applied_idx == _raft->raft_get_commit_idx())
        return SPDK_POLLER_IDLE;

    if(get_apply_in_progress())
        return SPDK_POLLER_IDLE;
    set_apply_in_progress(true);

    uint32_t  apply_num = std::min(uint32_t(_raft->raft_get_commit_idx() - _last_applied_idx), default_parallel_apply_num);

    raft_index_t start_idx = _last_applied_idx + 1;
    raft_index_t end_idx = _last_applied_idx + apply_num;

    _raft->raft_get_entry_by_idx(
      start_idx,
      end_idx,
      [this, start_idx, end_idx](std::vector<std::shared_ptr<raft_entry_t>> entries){
        if (entries.size() == 0){
            SPDK_WARNLOG("pg %lu.%lu not find log %ld\n",
                               get_raft()->raft_get_pool_id(), get_raft()->raft_get_pg_id(), start_idx);
            set_apply_in_progress(false);
            return;
        }
        SPDK_INFOLOG(pg_group, "pg %lu.%lu osd %d applying log: [%ld, %ld], size: %ld \n",
                           get_raft()->raft_get_pool_id(), get_raft()->raft_get_pg_id(),
                           get_raft()->raft_get_nodeid(), start_idx, end_idx, entries.size());
        
        raft_index_t end_index = start_idx + entries.size() - 1;
        apply_context *complete = new apply_context(start_idx, end_index, this);
        
        for(size_t i = 0; i < entries.size(); i++){
            auto &ety = entries[i];
            apply(ety, apply_done, complete);
        }
      });
    return SPDK_POLLER_BUSY;
}

struct obj_meta{
    uint64_t len;
    size_t  idx;
};

// void state_machine::merge_apply(std::vector<std::shared_ptr<raft_entry_t>> &entries, raft_index_t start_idx) {
    // raft_index_t end_index = start_idx + entries.size() - 1;
    // apply_context *complete = new apply_context(start_idx, end_index, this);
// 
    // std::map<std::string, std::map<uint64_t, obj_meta>> objs;
    // 记录需要合并的raft_entry的index
    // std::map<raft_index_t, std::vector<raft_index_t>> index_map;
// 
    // for(size_t i = 0; i < entries.size(); i++){
        // auto &entry = entries[i];
        // osd::write_cmd write;
        // write.ParseFromString(entry->meta());
// 
        // if(objs.find(write.object_name()) == objs.end()){
// 
        // } else {
// 
        // }
    // }
// }

bool state_machine::linearization() {
    // 在租期不会发生选举，确保 Leader 不会变。
    if (_raft->is_lease_valid()) {
        return true;
    }

    return false;
}

std::string &state_machine::get_pg_name(){
    return _raft->raft_get_pg_name();
}

bool state_machine::raft_is_leader(){
    return _raft->raft_is_leader();
}