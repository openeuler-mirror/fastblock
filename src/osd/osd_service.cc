#include "osd_service.h"
#include "utils/utils.h"

struct write_data_complete : public context{
    write_reply* response;
    google::protobuf::Closure* done;

    write_data_complete(write_reply* _response, google::protobuf::Closure* _done)
    : response(_response)
    , done(_done) {}

    void finish(int r) override {
        response->set_state(r);
        done->Run();
    }
};

class write_context : public core_context{
public:
    write_context(std::shared_ptr<raft_server_t> _raft, write_data_complete *_complete, 
            std::shared_ptr<msg_entry_t> _ety)
    : raft(_raft)
    , complete(_complete)
    , ety(_ety) {}

    void run_task() override {
        SPDK_NOTICELOG("delete pg in core %u\n", spdk_env_get_current_core());
        auto ret = raft->raft_write_entry(ety, complete);
        if(ret != 0){
            complete->response->set_state(ret);
            complete->done->Run();
        }               
    }
    std::shared_ptr<raft_server_t> raft;
    write_data_complete *complete;
    std::shared_ptr<msg_entry_t> ety;
};

void osd_service::process_write(google::protobuf::RpcController* controller,
             const write_request* request,
             write_reply* response,
             google::protobuf::Closure* done){
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t core_id;
    _pm->get_pg_core(pool_id, pg_id, core_id);
    auto pg = _pm->get_pg(core_id, pool_id, pg_id);
    write_cmd cmd;
    cmd.set_object_name(request->object_name());
    cmd.set_offset(request->offset());
    cmd.set_buf(std::move(request->data()));
    std::string buf;
    cmd.SerializeToString(&buf);

    auto entry_ptr = std::make_shared<msg_entry_t>();
    entry_ptr->set_type(RAFT_LOGTYPE_NORMAL);
    auto data = entry_ptr->mutable_data();
    data->set_obj_name(request->object_name());
    data->set_buf(std::move(buf));

    write_data_complete *complete = new write_data_complete(response, done);
    write_context* context = new write_context(pg->raft, complete, entry_ptr);
    _pm->invoke_on(core_id, context);
}

void osd_service::process_read(::google::protobuf::RpcController* controller,
             const ::read_request* request,
             ::read_reply* response,
             ::google::protobuf::Closure* done) {
    (void)controller;
    (void)request;
    (void)response;
    (void)done;
}

void osd_service::process_delete(::google::protobuf::RpcController* controller,
             const ::delete_request* request,
             ::delete_reply* response,
             ::google::protobuf::Closure* done){
    (void)controller;
    (void)request;
    (void)response;
    (void)done;
}  