#include "raft/append_entry_buffer.h"
#include "spdk/thread.h"
#include "raft/raft_private.h"
#include "utils/utils.h"

constexpr int32_t TIMER_APPEND_ENTRIER_BUFFER_USEC = 0;    //微秒

struct flush_complete : public context{
    context* comp;
    append_entries_buffer *buffer;

    flush_complete(context* _complete, append_entries_buffer *_buffer)
    : comp(_complete)
    , buffer(_buffer) {}

    void finish(int res) override {
        buffer->set_in_progress(false);
        comp->complete(res);
    }
};

void append_entries_buffer::enqueue(const msg_appendentries_t* request,
            msg_appendentries_response_t* response,
            context* complete){
    item_type item{request, response, complete};
    _request.push(std::move(item));
}

void append_entries_buffer::start(){
    _timer = SPDK_POLLER_REGISTER(&append_entries_buffer::buffer_flush, this, TIMER_APPEND_ENTRIER_BUFFER_USEC);
}

void append_entries_buffer::stop(){
    spdk_poller_unregister(&_timer);
}

void append_entries_buffer::do_flush(){
    if(_request.empty())
        return;
    if(_in_progress)
        return;
    
    _in_progress = true;

    
    auto item = _request.front();
    _request.pop();
    auto request = item.request;
    auto response = item.response;
    auto comp = item.complete;

    flush_complete* complete = new flush_complete(comp, this);

    int ret = _raft->raft_recv_appendentries(request->node_id(), request, response, complete);
    if(ret != 0){
        complete->complete(ret);
    }                        
}
