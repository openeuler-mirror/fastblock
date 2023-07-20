#ifndef OSD_SERVICE_H_
#define OSD_SERVICE_H_
#include "rpc/osd_msg.pb.h"
#include "partition_manager.h"

class osd_service : public rpc_service_osd{
public:
    osd_service(partition_manager* pm)
    : _pm(pm) {}

    void process_write(::google::protobuf::RpcController* controller,
                 const ::write_request* request,
                 ::write_reply* response,
                 ::google::protobuf::Closure* done) override;
    void process_read(::google::protobuf::RpcController* controller,
                 const ::read_request* request,
                 ::read_reply* response,
                 ::google::protobuf::Closure* done) override;
    void process_delete(::google::protobuf::RpcController* controller,
                 const ::delete_request* request,
                 ::delete_reply* response,
                 ::google::protobuf::Closure* done) override;    
private:
    partition_manager* _pm;
};

#endif