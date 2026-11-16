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
#include "raft/append_entry_buffer.h"
#include "spdk/thread.h"
#include "raft/raft.h"
#include "fastblock/utils/utils.h"
#include "fastblock/utils/err_num.h"

SPDK_LOG_REGISTER_COMPONENT(entry_buffer)

constexpr int32_t TIMER_APPEND_ENTRIER_BUFFER_USEC = 0;    //微秒

struct flush_complete : public utils::context{
    utils::context* comp;
    append_entries_buffer *buffer;

    flush_complete(utils::context* _complete, append_entries_buffer *_buffer)
    : comp(_complete)
    , buffer(_buffer) {}

    void finish(int res) override {
        buffer->flush_done(res, comp);
    }
};

void append_entries_buffer::enqueue(const msg_appendentries_t* request,
            msg_appendentries_response_t* response,
            utils::context* complete){
    if (_stopping) {
        response->set_success(err::RAFT_ERR_PG_SHUTDOWN);
        response->set_node_id(_raft->raft_get_nodeid());
        complete->complete(err::RAFT_ERR_PG_SHUTDOWN);
        return;
    }

    item_type item{request, response, complete};
    _request.push(std::move(item));
    SPDK_DEBUGLOG(entry_buffer, "append to buffer, pg %s, prev_log_idx %ld, buffer count %ld\n",
            _raft->raft_get_pg_name().c_str(), request->prev_log_idx(), _request.size());
}

void append_entries_buffer::start(){
    _timer = SPDK_POLLER_REGISTER(&append_entries_buffer::buffer_flush, this, TIMER_APPEND_ENTRIER_BUFFER_USEC);
}

void append_entries_buffer::drain(int res){
    while(!_request.empty()){
       auto item = _request.front();
       _request.pop();
       auto response = item.response;
       auto comp = item.complete;
       response->set_success(res);
       response->set_node_id(_raft->raft_get_nodeid());
       comp->complete(res);
    }
}

void append_entries_buffer::finish_stop_if_idle(){
    if (!_stopping || _in_progress || !_request.empty() || !_stop_fun) {
        return;
    }

    auto fun = std::move(_stop_fun);
    auto arg = _stop_arg;
    _stop_arg = nullptr;
    fun(arg, err::E_SUCCESS);
}

void append_entries_buffer::flush_done(int res, utils::context* complete){
    _in_progress = false;
    complete->complete(res);
    finish_stop_if_idle();
}

void append_entries_buffer::stop(utils::complete_fun fun, void *arg){
    if (_timer) {
        spdk_poller_unregister(&_timer);
    }
    _stopping = true;
    _stop_fun = std::move(fun);
    _stop_arg = arg;
    drain(err::RAFT_ERR_PG_SHUTDOWN);
    finish_stop_if_idle();
}

void append_entries_buffer::do_flush(){
    if(_request.empty())
        return;
    if(_in_progress)
        return;
    if(_stopping)
        return;

    _in_progress = true;


    auto item = _request.front();
    _request.pop();
    auto request = item.request;
    auto response = item.response;
    auto comp = item.complete;

    flush_complete* complete = new flush_complete(comp, this);
    SPDK_DEBUGLOG(entry_buffer, "handle the append entry, pg %s, prev_log_idx %ld, buffer count %ld\n",
            _raft->raft_get_pg_name().c_str(), request->prev_log_idx(), _request.size());

    _raft->raft_recv_appendentries(request->node_id(), request, response, complete);
}
