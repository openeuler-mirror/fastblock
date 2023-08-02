#ifndef RPC_SERVER_H_
#define RPC_SERVER_H_
#include <google/protobuf/service.h>

#include "base/core_sharded.h"
#include "msg/transport_server.h"

class rpc_server{
public:
    using transport_server_ptr = std::shared_ptr<msg::rdma::transport_server>;

    class rpc_server_context : public core_context{
    public:
        rpc_server_context(transport_server_ptr _transport)
        : transport(_transport) {}
    
        void run_task() override {
            SPDK_NOTICELOG("start transport in core %u\n", spdk_env_get_current_core());
            transport->start();
        }

        transport_server_ptr transport;
    };

    rpc_server(const rpc_server&) = delete;
    rpc_server& operator=(const rpc_server&) = delete;

    void register_service(google::protobuf::Service* service){
        _transport->add_service(service);
    }

    void start(std::string& address, int port){
        _transport->start_listen(address, port);
        auto shard_num = _shard.count();
        for(uint32_t i = 0; i < shard_num; i++){
            rpc_server_context *context = new rpc_server_context(_transport);
            _shard.invoke_on(i, context);
        }
    }

    static rpc_server& get_server(core_sharded &shard){
        static rpc_server s_server(shard);
        return s_server;
    }
private:
    rpc_server(core_sharded &shard)
    : _shard(shard) {
        _transport = std::make_unique<msg::rdma::transport_server>();
        _transport->prepare();
    }

    transport_server_ptr _transport;
    core_sharded &_shard;  
};

#endif