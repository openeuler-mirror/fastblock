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

void osd_service::process_write(google::protobuf::RpcController* controller,
             const write_request* request,
             write_reply* response,
             google::protobuf::Closure* done){
    auto pool_id = request->pool_id();
    auto pg_id = request->pg_id();
    uint32_t shard_id;
    _pm->get_pg_shard(pool_id, pg_id, shard_id);
    auto pg = _pm->get_pg(shard_id, pool_id, pg_id);
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

    _pm->get_shard().invoke_on(
      shard_id, 
      [this, complete, entry_ptr = std::move(entry_ptr), raft = pg->raft](){
        SPDK_NOTICELOG("raft_write_entry in core %u\n", spdk_env_get_current_core());
        auto ret = raft->raft_write_entry(entry_ptr, complete);
        if(ret != 0){
            complete->complete(ret);
        }
      });
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