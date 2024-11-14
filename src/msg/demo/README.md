# RDMA RPC DEMO

## 1 demo

```
$ make -j client server
```

**服务端**

*json* 配置文件如下：

```json
{
    "bind_ports": [8012, 8013, 8014, 8015],
    "rdma_device_name": <rdma device name>,
    "msg_server_listen_backlog" : 1024,
    "msg_server_poll_cq_batch_size": 32,
    "msg_server_metadata_memory_pool_capacity": 64,
    "msg_server_metadata_memory_pool_element_size": 512,
    "msg_server_data_memory_pool_capacity": 64,
    "msg_server_data_memory_pool_element_size": 5120,
    "msg_server_per_post_recv_num": 64,
    "msg_server_rpc_timeout_us": 1000000,
    "msg_rdma_resolve_timeout_us": 2000,
    "msg_rdma_poll_cm_event_timeout_us": 1000000,
    "msg_rdma_max_send_wr": 4096,
    "msg_rdma_max_send_sge": 128,
    "msg_rdma_max_recv_wr": 8192,
    "msg_rdma_max_recv_sge": 1,
    "msg_rdma_max_inline_data": 16,
    "msg_rdma_cq_num_entries": 1024,
    "msg_rdma_qp_sig_all": false
}
```

未在本文档做额外说明的配置见 [*src/msg/README.md*](../README.md)。

配置中的 `bind_ports` 是服务端 *listen* 的端口，可以配置多个端口，但是需要保证分配给 *demo server* 的 *cpu* 数量要大于等于 `bind_ports` 的数量。

```
$ src/msg/demo/server -C <json conf path> -m '[0,1,2,3]'
```

**客户端**

*json* 配置文件如下：

```json
{
    "server_address": "<server ip>",
    "server_ports": [8012, 8013, 8014, 8015],
    "io_depth": 32,
    "iteration_count": 50000000,
    "rdma_device_name": <rdma device name>,
    "msg_client_poll_cq_batch_size": 32,
    "msg_client_metadata_memory_pool_capacity": 64,
    "msg_client_metadata_memory_pool_element_size": 512,
    "msg_client_data_memory_pool_capacity": 64,
    "msg_client_data_memory_pool_element_size": 5120,
    "msg_client_per_post_recv_num": 64,
    "msg_client_rpc_timeout_us": 30000000,
    "msg_client_rpc_batch_size": 1024,
    "msg_client_connect_max_retry": 30,
    "msg_client_connect_retry_interval_us": 1000000,
    "msg_rdma_resolve_timeout_us": 2000,
    "msg_rdma_poll_cm_event_timeout_us": 1000000,
    "msg_rdma_max_send_wr": 4096,
    "msg_rdma_max_send_sge": 128,
    "msg_rdma_max_recv_wr": 8192,
    "msg_rdma_max_recv_sge": 1,
    "msg_rdma_max_inline_data": 16,
    "msg_rdma_cq_num_entries": 1024,
    "msg_rdma_qp_sig_all": false
}
```

未在本文档做额外说明的配置见 [*src/msg/README.md*](../README.md)。

- `server_address` 用来指定 *demo server* 的 *listen address*。
- `server_ports` 用来指定 *demo server* 的 *listen ports*，其内容可以是 *demo server* 配置中 `bind_ports` 的子集，需要保证分配给 *demo client* 的 *cpu* 数量要大于等于 `server_ports` 的数量。
- `io_depth` 指定客户端 *RDMA RPC* 的 *io depth*，即同时发起的 *RPC* 请求数量。
- `iteration_count` 指定客户端 *RDMA RPC* 的发送请求的次数，**这里的次数是每个核上都会发送 `iteration_count` 次**

```
$ src/msg/demo/client -C <json conf path> -m '[4,5,6,7]'
```
