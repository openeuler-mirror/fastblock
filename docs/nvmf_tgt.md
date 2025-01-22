NVMe over Fabrics 规范定义了可通过不同传输导出的子系统。SPDK 选择将导出这些子系统的软件称为“目标”，这是 iSCSI 使用的术语。该规范将连接到目标的“客户端”称为“主机”。许多人还将主机称为“发起者”，这在 iSCSI 用语中是等同的。SPDK 将尝试坚持使用术语“目标”和“主机”以符合规范。

fastblock-nvmf-tgt就是nvmf目标，Linux 内核还实现了 NVMe-oF主机，linux NVMe-oF主机可以通过连接fastblock-nvmf-tgt访问fastblock镜像。

使用方法：
# 1 部署fastblock集群
部署fastblock方法参考qemu_vhost_test.md文件，这里不再重复。

# 2 创建pool
```
fastblock-client -op=createpool -poolname=fb -pgcount=6 -pgsize=3 -failure_domain="osd"
fastblock-client -op=createimage -imagesize=$((100*1024*1024*1024))  -imagename=fbimage -poolname=fb
```

# 3 启动nvmf目标
nvmf目标的配置文件nvmf-tgt.json：
```json
{
    "mon_host": ["175.5.24.252"],
    "mon_rpc_address": "175.5.24.252",
    "mon_rpc_port": 3333,
    "rdma_device_name": "rocep125s0f0",
    "msg_client_poll_cq_batch_size": 1024,
    "msg_client_metadata_memory_pool_capacity": 4096,
    "msg_client_metadata_memory_pool_element_size": 1024,
    "msg_client_data_memory_pool_capacity": 4096,
    "msg_client_data_memory_pool_element_size": 8192,
    "msg_client_per_post_recv_num": 512,
    "msg_client_rpc_timeout_us": 1000000,
    "msg_client_rpc_batch_size": 1024,
    "msg_client_connect_max_retry": 30,
    "msg_client_connect_retry_interval_us": 1000000,
    "msg_rdma_resolve_timeout_us": 2000,
    "msg_rdma_poll_cm_event_timeout_us": 1000000,
    "msg_rdma_max_send_wr": 1024,
    "msg_rdma_max_send_sge": 128,
    "msg_rdma_max_recv_wr": 8192,
    "msg_rdma_max_recv_sge": 1,
    "msg_rdma_max_inline_data": 16,
    "msg_rdma_cq_num_entries": 1024,
    "msg_rdma_qp_sig_all": false
}
```

启动nvmf目标：
```
fastblock-nvmf-tgt -C nvmf-tgt.json -S 1
```

# 4 使用rpc.py创建bdev设备
创建bdev设备
```
spdk/scripts/rpc.py -s /var/tmp/fastblock_nvmf_tgt3217499.sock  bdev_fastblock_create -P  1 -p fb -i fbimage -k 4096 -I 100G -m "175.5.24.252:3333" -b fbdev
```
参数说明：
  -s /var/tmp/fastblock_nvmf_tgt3217499.sock： 是/var/tmp目录下fastblock_nvmf_tgt${fastblock-nvmf-tgt进程id}.sock
  -P ： pool id
  -p ： pool名字
  -i ： image名字
  -k ： image的block大小
  -I ： image的大小
  -m ： monitor地址
  -b ： bdev名字

# 5 创建nvmf transport
## 5.1 TCP transport
```
spdk/scripts/rpc.py nvmf_create_transport --server-addr /var/tmp/fastblock_nvmf_tgt3217499.sock -t TCP -u 16384 -m 8 -c 8192
```
参数说明：
  --server-addr fastblock_nvmf_tgt3217499.sock： 是/var/tmp目录下fastblock_nvmf_tgt${vhost进程id}.sock
  -t TCP：  TCP 传输
  -u    ：  I/O 单元大小，单位字节
  -m    ：  每个控制器qpair的最大数
  -c    ：  Max number of in-capsule data size，单位字节
## 5.2  RDMA transport
```
spdk/scripts/rpc.py nvmf_create_transport  --server-addr /var/tmp/fastblock_nvmf_tgt3217499.sock  -t RDMA -u 8192 -i 131072 -c 8192
```
参数说明：
  --server-addr fastblock_nvmf_tgt3217499.sock： 是/var/tmp目录下fastblock_nvmf_tgt${vhost进程id}.sock
  -t RDMA:  RDMA 传输
  -u     ：  I/O 单元大小，单位字节
  -i     ：  最大的io size，单位字节
  -c     ：  Max number of in-capsule data size，单位字节

# 6 创建NVMe-oF子系统
```
spdk/scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 --server-addr /var/tmp/fastblock_nvmf_tgt3217499.sock
-a -s SPDK00000000000001 -d SPDK_Controller1
```
参数说明：
  --server-addr fastblock_nvmf_tgt3217499.sock： 是/var/tmp目录下fastblock_nvmf_tgt${vhost进程id}.sock
  -a: Allow any host to connect
  -s: 序列号

# 7 向NVMe-oF子系统添加namespace
```
spdk/scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 fbdev --server-addr /var/tmp/fastblock_nvmf_tgt3217499.sock 
```
参数说明：
  --server-addr fastblock_nvmf_tgt3217499.sock： 是/var/tmp目录下fastblock_nvmf_tgt${vhost进程id}.sock
  fbdev为前面创建的bdev名字

# 8 向NVMe-oF子系统添加listener
## 8.1 TCP transport
```
spdk/scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 --server-addr /var/tmp/fastblock_nvmf_tgt3217499.sock -t TCP -a 192.168.100.8 -s 4420
```
参数说明：
  --server-addr fastblock_nvmf_tgt3217499.sock： 是/var/tmp目录下fastblock_nvmf_tgt${vhost进程id}.sock
  -a： NVMe-oF transport address
  -s: NVMe-oF transport port

## 8.2 RDMA transport
```
spdk/scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 --server-addr /var/tmp/fastblock_nvmf_tgt3217499.sock -t rdma -a 
192.168.100.8 -s 4420
```
参数说明：
  --server-addr fastblock_nvmf_tgt3217499.sock： 是/var/tmp目录下fastblock_nvmf_tgt${vhost进程id}.sock
  -a： NVMe-oF transport address
  -s:  NVMe-oF transport port

# 9 配置nvme-of主机

## 9.1 加载nvme-tcp和nvme-rdma模块
```
modprobe nvme-tcp
modprobe nvme-rdma
```

## 9.2 发现nvme-of目标
### 9.2.1 TCP transport
```
nvme discover -t tcp -a 192.168.100.8 -s 4420
```
### 9.2.2 RDMA transport
```
nvme discover -t rdma -a 192.168.100.8 -s 4420
```

## 9.3 连接nvme-of目标
### 9.3.1 TCP transport
```
nvme connect -t tcp -n "nqn.2016-06.io.spdk:cnode1" -a 192.168.100.8 -s 4420
```

### 9.3.2 RDMA transport
```
nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a 192.168.100.8 -s 4420
```

## 9.4 查看nvme磁盘
```
nvme list
```
可以看到一个SN为SPDK00000000000001（创建NVMe-oF子系统时指定的序列号）的磁盘
这个盘就可以作为一个磁盘使用了

## 9.5 断开连接
不想使用了，就可以断开连接
```
nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
```