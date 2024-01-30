# OSD

当前的 osd 把对象数据和日志数据放到一个磁盘里，使用一个 bdev 设备。osd 进程根据此 json 文件创建 bdev。

`src/osd/conf/disk_bdev.json`为 json 文件样例。

## 启动 OSD

OSD 使用 json 配置用户选项，命令行中使用 `-C` 指定 json 文件的路径：

```json
{
    "current_osd_id": 1,
    "osds": [
        {
            "pid_path": "/var/tmp/osd_1.pid",
            "osd_id": 1,
            "bdev_disk": "nvme0n1",
            "address": "osd1_addr",
            "port": osd1_port,
            "uuid": "d685a1ca-4a59-4c4f-80ff-59997f3d0494",
            "monitor": [
                {"host": "127.0.0.1", "port": 3333},
                {"host": "127.0.0.1", "port": 4333},
                {"host": "127.0.0.1", "port": 5333}
            ]
        },

        {
            "pid_path": "/var/tmp/osd_2.pid",
            "osd_id": 2,
            "bdev_disk": "nvme1n1",
            "address": "osd2_addr",
            "port": osd2_port,
            "uuid": "ee6289a5-74ee-4a41-ba62-3b465aa08ffd",
            "monitor": [
                {"host": "127.0.0.1", "port": 3333},
                {"host": "127.0.0.1", "port": 4333},
                {"host": "127.0.0.1", "port": 5333}
            ]
        },

        {
            "pid_path": "/var/tmp/osd_3.pid",
            "osd_id": 3,
            "bdev_disk": "nvme2n1",
            "address": "osd3_addr",
            "port": osd3_port,
            "uuid": "fd69cf95-f022-4529-bd45-7381a51f7359",
            "monitor": [
                {"host": "127.0.0.1", "port": 3333},
                {"host": "127.0.0.1", "port": 4333},
                {"host": "127.0.0.1", "port": 5333}
            ]
        }
    ],

    "msg": {
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

        "client": {
            "poll_cq_batch_size": 32,
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
            "max_recv_sge": 1,
            "max_inline_data": 16,
            "cq_num_entries": 1024,
            "qp_sig_all": false,
	        "rdma_device_name": "mlx5_0"
        }
    }
}
```

OSD 的配置文件包含两部分，一部分用于配置 OSD 自身的，另一部分用于配置 RPC。关于 RPC 的配置说明，可以参考 `src/msg/README.md`。

- **osds**  
    OSD 节点配置数组，里面可以包含多个 OSD 的配置信息  
- **current_osd_id**  
    指定当前使用哪个 OSD 配置信息  
- **pid_path**  
    OSD pid 文件路径  
- **osd_id**  
    OSD 的 id  
- **bdev_disk**  
    bdev disk 名  
- **address**  
    OSD listen address  
- **port**  
    OSD listen port  
- **uuid**  
    OSD UUID  
- **monitor**  
    monitor 集群地址

### 使用 Nvme 盘

#### 1 Spdk 接管磁盘 

```
sh setup.sh status   //查看磁盘的BDF
sh setup.sh 
```     

`setup.sh` 在 `fastblock/build/deps_build/spdk-prefix/src/spdk/scripts` 中。

#### 2 配置 json 文件

上一步 `setup.sh status` 获取了磁盘的 BDF，把此 BDF 值作为 json 文件的 traddr 值

`setup.sh status` 的结果，例：

```
Type     BDF             Vendor Device NUMA    Driver           Device     Block devices
NVMe     0000:03:00.0    15ad   07f0   0       nvme             nvme2      nvme2n1
NVMe     0000:0b:00.0    15ad   07f0   0       nvme             nvme1      nvme1n1
NVMe     0000:13:00.0    15ad   07f0   0       nvme             nvme0      nvme0n1
```  


磁盘 `/dev/nvme0n1` 的 BDF 为 `0000:13:00.0`

json文件的实例：`osd1_disk_bdev.json`

```json
{
    "subsystems": [
        {
        "subsystem": "bdev",
        "config": [
            {
            "method": "bdev_nvme_attach_controller",
            "params": {
                "name": "nvme0",
                "trtype": "pcie",
                "traddr": "0000:13:00.0"
            }
            }
        ]
        }
    ]
}
```

#### 3 启动 OSD

启动命令：

```bash
fastblock/build/src/osd/fastblock-osd -s 1024 -m 0x1 -c osd1_disk_bdev.json -C osd1.json
```

参数 `-D` 为磁盘的名字，要和参数 `-c` 指定的 json 文件中的 `traddr` 对应

### 使用 Aio

#### 1 配置 json 文件

json 文件的实例：`osd1_disk_bdev.json`

```json
{
    "subsystems": [
    {
        "subsystem": "bdev",
        "config": [
        {
            "params": {
                "name": "AIO0",
                "block_size": 2048,
                "filename": "/tmp/aiofile"
            },
            "method": "bdev_aio_create"
        }
        ]
    }
    ]
}
```
 
注意：**json 文件里的 `filename` 指向的文件（即 `/tmp/aiofile`）必须大于 1GB，可以使用 `dd` 出来一个 2GB 的文件**

```bash
dd if=/dev/zero of=/tmp/aiofile bs=1G count=2
```
#### 2 启动 OSD
启动命令：

```bash
fastblock/build/src/osd/fastblock-osd -s 1024 -m 0x1 -c osd1_disk_bdev.json -C osd.json
```

注：**OSD 启动命令中的 `-U` 参数是此 OSD 对应的 uuid，使用下面的命令向 monitor 注册（fbclient 是 monitor 编译出来的命令）**

```bash
fbclient -op=fakeapplyid -uuid=`uuidgen`
```
