# Tools

## Block bench tool

配置采用 *json* 文件指定，示例如下：

```json
{
    "io_type": "write",
    "io_size": 4096,
    "io_count": 1,
    "io_depth": 8,
    "io_queue_size": 128,
    "io_queue_request": 4096,
    "image_name": "test_image",
    "image_size": 2907152,
    "object_size": 1048576,
    "pool_id": 1,
    "pool_name": "test_bdev_2",
    "monitor": [

        {"host": "127.0.0.1", "port": 3333}
    ],
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

该压测工具会在每个核上运行一个 *SPDK POLLER*，在 *SPDK POLLER* 中向 *osd* 发送 `io_count` 次请求。举个例子，如果 `io_count` 是 *128*，启动进程时通过 `-m` 指定值为 `[0-9]`，则总共会发送 `128 x 10`，即 *1280* 次请求。

### io_type

`io_type` 有三个可选值：`read`、`write` 和 `write_read`。`write` 和 `read` 对 `image_name` 对象进行读写。`write_read` 会先写 `io_count` 次对象，再读 `io_count` 次。

### io_size

用于指定对象大小。

### io_count

用于指定读写对象次数

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
