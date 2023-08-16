#ifndef APPEND_ENTRY_BUFFER_H_
#define APPEND_ENTRY_BUFFER_H_
#include <queue>
#include "rpc/raft_msg.pb.h"

class raft_server_t;

class append_entries_buffer{
public:
    struct item_type{
        const msg_appendentries_t* request;
        msg_appendentries_response_t* response;
        google::protobuf::Closure* done;
    };

    append_entries_buffer(raft_server_t* raft)
    : _raft(raft)
    , _in_progress(false) {}

    void enqueue(const msg_appendentries_t* request,
                msg_appendentries_response_t* response,
                google::protobuf::Closure* done);

    void start();

    static int buffer_flush(void *arg){
        append_entries_buffer* buff = (append_entries_buffer *)arg;
        buff->do_flush();
        return 0;
    }

    void do_flush();

    void stop();
    void set_in_progress(bool is_in_progress){
        _in_progress = is_in_progress;
    }
private:
    raft_server_t* _raft;
    std::queue<item_type> _request;
    struct spdk_poller * _timer;
    bool _in_progress;
};
#endif