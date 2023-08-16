#include "raft/append_entry_buffer.h"
#include "spdk/thread.h"
#include "raft/raft_private.h"
#include "utils/utils.h"

constexpr int32_t TIMER_APPEND_ENTRIER_BUFFER_USEC = 0;    //微秒

struct append_entries_complete : public context{
    google::protobuf::Closure* done;
    append_entries_buffer *buffer;

    append_entries_complete(google::protobuf::Closure* _done, append_entries_buffer *_buffer)
    : done(_done)
    , buffer(_buffer) {}

    void finish(int ) override {
        SPDK_NOTICELOG("append_entries_complete\n");
        buffer->set_in_progress(false);
        done->Run();
    }
};

void append_entries_buffer::enqueue(const msg_appendentries_t* request,
            msg_appendentries_response_t* response,
            google::protobuf::Closure* done){
    item_type item{request, response, done};
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
    auto done = item.done;

    append_entries_complete* complete = new append_entries_complete(done, this);

    int ret = _raft->raft_recv_appendentries(request->node_id(), request, response, complete);
    if(ret != 0){
        complete->complete(ret);
    }                        
}

