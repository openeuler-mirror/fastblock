# MSG

该目录下的代码包含了所有 *RDMA RPC* 的代码。

## 模块配置

*RPC* 没有单独的配置文件，而是作为一个单独的 *json* 块嵌入到其他应用的配置文件中，一般内容如下：

```json
{
    // 其他配置内容
    "rdma_device_name": "mlx5_0",
    "rdma_device_port": 1,
    "msg_server_listen_backlog" : 1024,
    "msg_server_poll_cq_batch_size": 32,
    "msg_server_metadata_memory_pool_capacity": 16384,
    "msg_server_metadata_memory_pool_element_size": 1024,
    "msg_server_data_memory_pool_capacity": 16384,
    "msg_server_data_memory_pool_element_size": 8192,
    "msg_server_per_post_recv_num": 512,
    "msg_server_rpc_timeout_us": 1000000,
    "msg_client_poll_cq_batch_size": 32,
    "msg_client_metadata_memory_pool_capacity": 16384,
    "msg_client_metadata_memory_pool_element_size": 1024,
    "msg_client_data_memory_pool_capacity": 16384,
    "msg_client_data_memory_pool_element_size": 8192,
    "msg_client_per_post_recv_num": 512,
    "msg_client_rpc_timeout_us": 1000000,
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

- *poll_cq_batch_size*  
    用于指定函数 [`ibv_poll_cq()`](https://man7.org/linux/man-pages/man3/ibv_poll_cq.3.html) 一次 *poll CQE* 的数量  
- *metadata_memory_pool_capacity*  
    指定 *RPC* 层用于传递元信息的内池持大小  
- *metadata_memory_pool_element_size_byte*  
    指定元信息内存池中内存块的大小，该内存块用于 *RDMA Send* 发送消息  
- *data_memory_pool_capacity*  
    指定 *RPC* 层用于传递 *RPC* 请求数据的内存池大小  
- *data_memory_pool_element_size_byte*  
    指定数据内存池中内存块的大小，该内存块用于 *RDMA Read* 发送消息  
- *per_post_recv_num*  
    每个连接建立时，均会预先通过 [`ibv_post_recv()`](https://man7.org/linux/man-pages/man3/ibv_post_recv.3.html) *post* *per_post_recv_num* 个 *Recvive WR*  
- *rpc_timeout_us*  
    一次 *RPC* 请求的超时时间  
- *rpc_batch_size*  
    用于限制客户端同时最多发送的 *RPC* 请求数量  
- *listen_backlog*  
    该字段用于配置 [`rdma_listen()` 函数的 `backlog` 参数](https://man7.org/linux/man-pages/man3/rdma_listen.3.html#ARGUMENTS)  
- *resolve_timeout_us*  
    指定 [`rdma_resolve_addr()`](https://man7.org/linux/man-pages/man3/rdma_resolve_addr.3.html) 和 [`rdma_resolve_route()`](https://man7.org/linux/man-pages/man3/rdma_resolve_route.3.html) 的超时时间  
- *poll_cm_event_timeout_us*  
    指定 *poll rdma cm* 事件的超时时间  
- *max_send_wr & max_send_sge & max_recv_wr & max_recv_sge & max_inline_data*  
    [`ibv_create_qp()`](https://man7.org/linux/man-pages/man3/ibv_create_qp.3.html) 的相关参数  
- *cq_num_entries*  
    [`ibv_create_cq()`](https://man7.org/linux/man-pages/man3/ibv_create_cq.3.html) 的 `cqe` 参数  
- *qp_sig_all*  
    配置 *RDMA Send WR* 是否产生 *CQE*
- *rdma_device_name*  
    指定了 *RDMA* 网卡的设备名，如果是空字符串或者未指定，则默认使用网卡列表中第一个 *active* 的。可以参考 [`show_gids`](https://enterprise-support.nvidia.com/s/article/understanding-show-gids-script) 命令配置  
- *rdma_device_port*  
    指定网卡端口名，可以参考 [`show_gids`](https://enterprise-support.nvidia.com/s/article/understanding-show-gids-script) 命令配置  
- *connect_max_retry*  
    指定 *RPC* 重连最大尝试次数  
- *connect_retry_interval_us*  
    指定每次重连的时间间隔
