# MSG

该目录下的代码包含了所有 *RDMA RPC* 的代码。

## 模块配置

*RPC* 没有单独的配置文件，而是作为一个单独的 *json* 块嵌入到其他应用的配置文件中，一般内容如下：

```json
{
    // 其他配置内容
    "rdma_device_name": "mlx5_0",
    "rdma_device_port": 1,
    "rdma_gid_index": 2,
    "msg_server_listen_backlog" : 1024,
    "msg_server_poll_cq_batch_size": 32,
    "msg_server_metadata_memory_pool_capacity": 16384,
    "msg_server_metadata_memory_pool_element_size": 1024,
    "msg_server_data_memory_pool_capacity": 16384,
    "msg_server_data_memory_pool_element_size": 8192,
    "msg_server_per_post_recv_num": 512,
    "msg_server_rpc_timeout_us": 1000000,
    "msg_server_slow_method_us": 100000,
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
    "msg_client_slow_rpc_warn_us": 100000,
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
    指定 *RPC* 层用于传递元信息的内存池大小  
- *metadata_memory_pool_element_size_byte*  
    指定元信息内存池中内存块的大小，该内存块用于 *RDMA Send* 发送消息  
- *data_memory_pool_capacity*  
    指定 *RPC* 层用于传递 *RPC* 请求数据的内存池大小。这是该运输实例的**上限**，不是「调到这个数就不会失败」。池空或低于水位时这次 RPC `resource_exhausted`，心跳走 priority 额度。  
- *data_memory_pool_element_size_byte*  
    指定数据内存池中内存块的大小，该内存块用于 *RDMA Read* 发送消息  
- *pool_low_watermark* / *pool_high_watermark*  
    空闲 chunk 低于 low 时拒绝 bulk（大包 / 非 inline）；回升到 high 后恢复 bulk。json 省略则按 capacity 推默认（约 1/16 与 2×low）。  
- *reserved_per_connection* / *reserved_priority*  
    每条连接预留的小 RPC chunk，以及 heartbeat 专用额度。bulk 分配不得把这两档吃光。json 省略则 priority=4、每连接=2。  
- *per_post_recv_num*  
    每个连接建立时，均会预先通过 [`ibv_post_recv()`](https://man7.org/linux/man-pages/man3/ibv_post_recv.3.html) *post* *per_post_recv_num* 个 *Recvive WR*  
- *rpc_timeout_us*  
    一次 *RPC* 请求的超时时间。client / server **代码默认都是 1s**（`1000000`），与本 README 示例一致；json 省略该键即用此默认。  
    `600s`（`600000000`）只用于长回归 / 等 snapshot，必须在 json 里显式写出。`vstart.sh` 开发默认也是 1s，长回归设 `FASTBLOCK_RPC_TIMEOUT_US=600000000`。
- *rpc_batch_size*  
    用于限制客户端同时最多发送的 *RPC* 请求数量  
- *listen_backlog*  
    该字段用于配置 [`rdma_listen()` 函数的 `backlog` 参数](https://man7.org/linux/man-pages/man3/rdma_listen.3.html#ARGUMENTS)  
- *resolve_timeout_us*  
    指定 [`rdma_resolve_addr()`](https://man7.org/linux/man-pages/man3/rdma_resolve_addr.3.html) 和 [`rdma_resolve_route()`](https://man7.org/linux/man-pages/man3/rdma_resolve_route.3.html) 的超时时间。  
    数字 IPv4 / IPv6 字面量用 `inet_pton` 直接填 `sockaddr`，不在 reactor 上调 `rdma_getaddrinfo`。hostname 仍走阻塞 `rdma_getaddrinfo` 并打 NOTICE；解析失败抛错，不再带着空 `_res` 继续。生产地址是数字 IPv4。  
- *poll_cm_event_timeout_us*  
    指定 *poll rdma cm* 事件的超时时间  
- *reactor / lifecycle*  
    `server::handle_stop_timeout` 只对 **server 自己 `spdk_thread_create` 出来的线程** 调 `spdk_thread_exit`。OSD 传入的 shard 线程是借来的，stop 后仍在。  
    启动期 `ibv_reg_mr` 失败抛 `std::runtime_error`（回滚已注册 MR），不再 `SIGABRT`。OSD `create_rpc_server` 已 catch，该 shard 标失败、不向 mon boot。
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
- *rdma_gid_index*  
    指定 *gid* 索引，可以参考 [`show_gids`](https://enterprise-support.nvidia.com/s/article/understanding-show-gids-script) 命令配置  
- *connect_max_retry*  
    指定 *RPC* 重连最大尝试次数  
- *connect_retry_interval_us*  
    指定每次重连的时间间隔
