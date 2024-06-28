#include "data_statistics.h"
#include "utils/log.h"
#include "monclient/client.h"

std::shared_ptr<data_statistics> g_data_statistics{nullptr};

static int hand_data(void *arg) {
    data_statistics *ds = (data_statistics *)arg;
    ds->send_data_to_mon();
    return 0;
}

void data_statistics::send_data_to_mon() {
    if(!_mon_client)
        return;
    if(_ios.size() == 0)
        return;
    auto ios = std::exchange(_ios, {});
    _mon_client->send_data_statistics_request(ios);
}

data_statistics::data_statistics()
: _thread(spdk_get_thread()){
    _timer = SPDK_POLLER_REGISTER(&hand_data, this, 1 * 1000 * 1000);  //1秒
}

struct thread_context {
    data_statistics *ds;
    utils::operation_type type;
    std::string pg_name;
    uint64_t bytes;
};

static void thread_done(void *arg){
    thread_context *ctx = (thread_context *)arg;
    ctx->ds->insert_data(ctx->type, ctx->pg_name, ctx->bytes);
    delete ctx;
}

void data_statistics::add_data(utils::operation_type type,  std::string &pg_name, uint64_t bytes){
    if(spdk_get_thread() != _thread){
        thread_context *ctx = new thread_context{.ds = this, .type = type, .pg_name = pg_name, .bytes = bytes};
        spdk_thread_send_msg(_thread, thread_done, ctx);
    } else {
        insert_data(type, pg_name, bytes);
    }
}