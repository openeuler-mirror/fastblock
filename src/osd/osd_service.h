#ifndef OSD_SERVICE_H_
#define OSD_SERVICE_H_
#include "rpc/osd_msg.pb.h"
#include "partition_manager.h"
#include "mon/client.h"

class osd_service : public osd::rpc_service_osd{
public:
    osd_service(partition_manager* pm, std::shared_ptr<monitor::client> mon_cli)
    : _pm(pm), _monitor_client{mon_cli} {}

    void process_write(google::protobuf::RpcController* controller,
                 const osd::write_request* request,
                 osd::write_reply* response,
                 google::protobuf::Closure* done) override {
        process<osd::write_request, osd::write_reply>(request, response, done); 
    }

    void process_read(google::protobuf::RpcController* controller,
                 const osd::read_request* request,
                 osd::read_reply* response,
                 google::protobuf::Closure* done) override {
        process<osd::read_request, osd::read_reply>(request, response, done);
    }

    void process_delete(google::protobuf::RpcController *controller,
                        const osd::delete_request *request,
                        osd::delete_reply *response,
                        google::protobuf::Closure *done) override {
        process<osd::delete_request, osd::delete_reply>(request, response, done);
    }

    void process_rpc_bench(google::protobuf::RpcController *controller,
                           const osd::bench_request *request,
                           osd::bench_response *response,
                           google::protobuf::Closure *done) override;
    void process_get_leader(google::protobuf::RpcController* controller,
                       const osd::pg_leader_request* request,
                       osd::pg_leader_response* response,
                       google::protobuf::Closure* done) override;

    template<typename request_type, typename reply_type> 
    void process(const request_type* request, reply_type* response, google::protobuf::Closure* done);

    void process(
            std::shared_ptr<osd_stm> osd_stm_p, 
            const osd::write_request* request, 
            osd::write_reply* response, 
            google::protobuf::Closure* done);
    void process(
            std::shared_ptr<osd_stm> osd_stm_p,
            const osd::read_request* request, 
            osd::read_reply* response, 
            google::protobuf::Closure* done);
    void process(
            std::shared_ptr<osd_stm> osd_stm_p,
            const osd::delete_request* request, 
            osd::delete_reply* response, 
            google::protobuf::Closure* done);
private:
    partition_manager* _pm;
    std::shared_ptr<monitor::client> _monitor_client{nullptr};
};

#endif