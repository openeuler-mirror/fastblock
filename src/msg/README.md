# MSG

该目录下的代码包含了所有 *RDMA RPC* 的代码。

## 模块配置

*RPC* 没有单独的配置文件，而是作为一个单独的 *json* 块嵌入到其他应用的配置文件中，一般内容如下：

```json
{
    // 其他配置内容
    "msg": {
        "client": {
            "poll_cq_batch_size": 8,
            "metadata_memory_pool_capacity": 16384,
            "metadata_memory_pool_element_size_byte": 1024,
            "data_memory_pool_capacity": 16384,
            "data_memory_pool_element_size_byte": 8192,
            "per_post_recv_num": 512,
            "rpc_timeout_us": 1000000,
            "rpc_batch_size": 1024
        },
        
        "server": {
            "listen_backlog": 1024,
            "poll_cq_batch_size": 32,
            "metadata_memory_pool_capacity": 16384,
            "metadata_memory_pool_element_size_byte": 1024,
            "data_memory_pool_capacity": 16384,
            "data_memory_pool_element_size_byte": 8192,
            "per_post_recv_num": 512,
            "rpc_timeout_us": 1000000
        },

        "rdma": {
            "resolve_timeout_us": 2000,
            "poll_cm_event_timeout_us": 1000000,
            "max_send_wr": 4096,
            "max_send_sge": 128,
            "max_recv_wr": 8192,
            "max_recv_sge": 128,
            "max_inline_data": 16,
            "cq_num_entries": 1024,
            "qp_sig_all": false
        }
    }
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
    de
