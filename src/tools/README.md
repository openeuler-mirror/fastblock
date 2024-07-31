# Tools

## Block bench tool

配置采用 *json* 文件指定，示例如下：

```json
{
    "io_print_stats_enable": true,
    "io_print_stats_interval_ms": 1000,
    "io_print_stats_acc": false,
    "io_print_stats_take_single_core": true,
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

### io_print_stats_enable 和 io_print_stats_interval_ms

如果为 `true`， 则会每过 *io_print_stats_interval_ms* 毫秒打印一次延时统计值

### io_print_stats_acc

如果为 `false`，则每次打印的延时统计值都是 *[0, io_print_stats_interval_ms]* 区间内的延时；如果为 `true`，则打印的延时数据，包含的是从压测开始时，到当前的延时数据。

### io_print_stats_take_single_core

由于实时打印在终端打印 *iops* 统计数值对吞吐影响较大，因此在有多核可用的情况下，允许 *block_bench* 单独占用一个核用于实时打印。该配置项如果为 `true`，则使用第一个核作为管理核，该核上不跑 *block io*。

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

## RPC bench tool

假设有三个节点：

```
192.168.0.1
192.168.0.2
192.168.0.3
```

对应的配置文件如下：

```json
{
    "endpoints": [
        {
            "index": 1,
            "list": [
                {
                    "host": "192.168.0.1",
                    "port": 3000
                }
            ]
        },
        {
            "index": 2,
            "list": [
                {
                    "host": "192.168.0.2",
                    "port": 3000
                }
            ]
        },
        {
            "index": 3,
            "list": [
                {
                    "host": "192.168.0.3",
                    "port": 3000
                }
            ]
        }
    ],
    "io_size": 4096,
    "io_depth": 128,
    "io_count": 200000,
    "rpc_client_same_core": true,
    "rdma_device_name": "mlx5_1",
    "rdma_device_port": 1,
    "rdma_gid_index": 4,
    "msg_server_listen_backlog": 1024,
    "msg_server_poll_cq_batch_size": 128,
    "msg_server_metadata_memory_pool_capacity": 16384,
    "msg_server_metadata_memory_pool_element_size": 1024,
    "msg_server_data_memory_pool_capacity": 16384,
    "msg_server_data_memory_pool_element_size": 8192,
    "msg_server_per_post_recv_num": 512,
    "msg_server_rpc_timeout_us": 30000000,
    "msg_rdma_resolve_timeout_us": 2000,
    "msg_rdma_poll_cm_event_timeout_us": 1000000,
    "msg_rdma_max_send_wr": 4096,
    "msg_rdma_max_send_sge": 128,
    "msg_rdma_max_recv_wr": 8192,
    "msg_rdma_max_recv_sge": 1,
    "msg_rdma_max_inline_data": 16,
    "msg_rdma_cq_num_entries": 1024,
    "msg_rdma_qp_sig_all": false,
    "msg_client_connect_max_retry": 30,
    "msg_client_connect_retry_interval_us": 1000,
    "msg_client_shutdown_timeout_us": 60000000
}

```

下面以 `192.168.0.1` 的视角进行说明，其余节点类似。

*rpc_bench* 启动后会监听 `192.168.0.1:3000`，然后根据配置文件连接 `192.168.0.2:3000` 和 `192.168.0.3:3000`。

一个 *rpc_bench* 会启动一个 *rpc server* 对象，占用一个核，启动两个 *rpc client* 对象管理与 `192.168.0.2:3000` 和 `192.168.0.3:3000` 建立的连接，各占用一个核，所以总共占用三个核。

然后给 `192.168.0.2:3000` 和 `192.168.0.3:3000` 分别发送 `io_count` 个 *rpc* 请求，每个 *rpc* 请求大小为 `io_size` 个字节。

运行命令如下：

```bash
# 192.168.0.1
./rpc_bench -C rpc_bench.json -I 1 -m [0,1,2]

# 192.168.0.2
./rpc_bench -C rpc_bench.json -I 2 -m [0,1,2]

# 192.168.0.3
./rpc_bench -C rpc_bench.json -I 3 -m [0,1,2]
```

命令行参数 `-C` 用于指定配置文件路径，`-I` 与指定配置文件中 `endpoints` 中的 `index` 相对应。

### 使用 supervisord 管理 rpc_bench

在 `rpc_bench` 目录下，提供了 *python* 脚本，会根据 *src/tools/rpc_bench/rpc_bench.json* 的内容生成对应的 *supervisord* 配置文件，配置文件位于 `/etc/supervisor/conf.d/rpc_bench.conf`。

命令如下

```bash
mkdir -p /var/log/supervisor/ # create log dir
python3 run.py
```

在每个节点上，使用 `supervisorctl restart rpc_bench:*` 启动 *rpc_bench*。
