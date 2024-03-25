# Tools

## Block bench tool

配置采用 *json* 文件指定，示例如下：

```json
{
    "io_type": "write",
    "io_size": 4096,
    "io_count": 10000,
    "io_depth": 8,
    "io_queue_size": 128,
    "io_queue_request": 4096,
    "image_name": "test_image",
    "image_size": 107374182400,
    "object_size": 1048576,
    "pool_id": 26,
    "pool_name": "volume",
    "mon_host": ["173.20.4.3","173.20.4.2","173.20.4.1"],
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

压测工具的配置包含两部分，其中一部分是关于 *RDMA RPC* 的配置，这部分说明可以参考 `src/msg/README.md`。

### io_type

`io_type` 有三个可选值：`read`、`write` 和 `write_read`。`write` 和 `read` 对 `image_name` 对象进行读写。`write_read` 会先写 `io_count` 次对象，再读 `io_count` 次。

### io_size

用于指定对象大小。

### io_count

用于指定读写对象次数

### io_depth

指定压测 *IO* 队列深度

### io_queue_size

连接 *IO* 队列深度

### io_queue_request

连接 *IO* 队列创建的请求数量，该值应该大于等于 *io_queue_size*。

### image_name

如果为空，则生成 *32* 位随机字符串

### pool_id

用于指定对象存储的 *pool id*

### pool_name

用于指定对象存储的 *pool name*

### monitor

用于指定 *monitor* 集群地址

### 运行示例

```
$ ./block_bench -m '[31]' -C /etc/fastblock/block_bench.json
...
[2023-10-09 07:22:33.752135] /data/sdb/code/custorage/transfer/fastblock/tools/block_bench.cc: 270:watch_poller: *NOTICE*: ===============================[read  latency]========================================
[2023-10-09 07:22:33.752306] /data/sdb/code/custorage/transfer/fastblock/tools/block_bench.cc: 291:watch_poller: *NOTICE*:  p0.1: 91199us p0.5: 92617us p0.9: 136487us p0.95: 137968us p0.99: 139579us p0.999: 139605us mean 107187us
[2023-10-09 07:22:33.752320] /data/sdb/code/custorage/transfer/fastblock/tools/block_bench.cc: 297:watch_poller: *NOTICE*: ======================================================================================
```
