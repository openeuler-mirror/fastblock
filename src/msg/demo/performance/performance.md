# RDMA RPC 性能测试

## 总结

1. 跨节点测试结果要明显优于单节点的测试结果，原因是单节点网卡的压力是跨界点网卡压力的两倍。
2. *iops* 的增长与分配的核数大致呈对数关系，随着核数的增多，*iops* 的增长速度明显变缓，原因是核数越多，网卡的压力也在不断增加。
3. 多服务端对多客户端的测试结果基本上与在同等核数的条件下，单客户端的单服务端的测试结果一致。
4. *iops* 的增长在 *20* 核左右趋近饱和，再继续增加核数 *iops* 也基本不再增加，原因是网卡带宽已经跑满。
5. 在优化配置后，同等核数下，*iops* 有显著增长，配置优化仍有探索空间。

## 配置文件

服务端 *RDMA* 相关参数配置：

```json
{
    "rdma_device_name": "mlx5_0",
    "msg_server_listen_backlog" : 1024,
    "msg_server_poll_cq_batch_size": 32,
    "msg_server_metadata_memory_pool_capacity": 512,
    "msg_server_metadata_memory_pool_element_size": 512,
    "msg_server_data_memory_pool_capacity": 512,
    "msg_server_data_memory_pool_element_size": 5120,
    "msg_server_per_post_recv_num": 512,
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

客户端 *RDMA* 相关参数配置：

```json
{
    "io_depth": 32,
    "iteration_count": 20000000,
    "rdma_device_name": "mlx5_0",
    "msg_client_poll_cq_batch_size": 32,
    "msg_client_metadata_memory_pool_capacity": 512,
    "msg_client_metadata_memory_pool_element_size": 512,
    "msg_client_data_memory_pool_capacity": 512,
    "msg_client_data_memory_pool_element_size": 5120,
    "msg_client_per_post_recv_num": 512,
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

*RDMA* 网卡参数：

```
CA 'mlx5_0'
	CA type: MT4119
	Number of ports: 1
	Firmware version: 16.31.1014
	Hardware version: 0
	Node GUID: 0xf03f95030052d402
	System image GUID: 0xf03f95030052d402
	Port 1:
		State: Active
		Physical state: LinkUp
		Rate: 100
		Base lid: 2
		LMC: 0
		SM lid: 1
		Capability mask: 0x2651e848
		Port GUID: 0xf03f95030052d402
		Link layer: InfiniBan
```

*CPU* 参数：

```
架构：                  aarch64
  CPU 运行模式：         64-bit
  字节序：               Little Endian
CPU:                    96
  在线 CPU 列表：       0-95
厂商 ID：               HiSilicon
  BIOS Vendor ID:       HiSilicon
  型号名称：            Kunpeng-920
    BIOS Model name:    Kunpeng 920-4826
    型号：              0
    每个核的线程数：    1
    每个座的核数：      48
    座：                2
    步进：              0x1
    BogoMIPS：          200.00
    标记：              fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma dcpop asimddp asimdfhm
Caches (sum of all):
  L1d:                  6 MiB (96 instances)
  L1i:                  6 MiB (96 instances)
  L2:                   48 MiB (96 instances)
  L3:                   96 MiB (4 instances)
```

关于本次测试代码的使用参数，见 [*src/msg/rdma/demo/README.md*](../README.md)

## 单节点单进程测试

### 服务端 1 核 & 客户端 1 核

```
服务端

$ build/src/msg/demo/server -C server.json -m '[0]'

客户端

$ build/src/msg/demo/client -C client.json -m '[1]'
```

![](c1_cli_thds.png)

![](c1_cli_pollers.png)

![](c1_srv_thds.png)

![](c1_srv_pollers.png)

![](c1_bw.png)

```
mean duration is 221.630906us, total dur: 694.122827s, iops: 144066.721541, iodepth: 32
```

### 服务端 4 核 & 客户端 4 核

```
服务端

$ build/src/msg/demo/server -C server.json -m '[0,1,2,3]'

客户端

$ build/src/msg/demo/client -C client.json -m '[10,11,12,13]'
```

![](c4_cli_thds.png)

![](c4_cli_pollers.png)

![](c4_srv_thds.png)

![](c4_srv_pollers.png)

![](c4_bw.png)

```
mean duration is 235.482278us, total dur: 775.149566s, iops: 516029.444708, iodepth: 32
```

### 服务端 8 核 & 客户端 8 核

```
服务端

$ build/src/msg/demo/server -C server.json -m '[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]'

客户端

$ build/src/msg/demo/client -C client.json -m '[20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35]'
```

![](c8_cli_thds.png)

![](c8_cli_pollers.png)

![](c8_srv_thds.png)

![](c8_srv_pollers.png)

![](c8_bw.png)

```
mean duration is 265.445446us, total dur: 437.561113s, iops: 914158.017976, iodepth: 32
```

### 服务端 16 核 & 客户端 16 核

```
服务端

$ build/src/msg/demo/server -C server.json -m '[0,1,2,3,4,5,6,7]'

客户端

$ build/src/msg/demo/client -C client.json -m '[10,11,12,13,14,15,16,17]'
```

![](c16_cli_thds.png)

![](c16_cli_pollers.png)

![](c16_srv_thds.png)

![](c16_srv_pollers_1.png)

![](c16_srv_pollers_2.png)

![](c16_bw.png)

```
mean duration is 389.081004us, total dur: 611.879729s, iops: 1307446.483005, iodepth: 32
```

优化配置后，可测得更好的结果。

服务端配置：

```json
{
    "rdma_device_name": "mlx5_0",
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

客户端配置：

```json
{
    "io_depth": 32,
    "iteration_count": 10000000,
    "rdma_device_name": "mlx5_0",
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

![](imp_c16_cli_thds.png)

![](imp_c16_cli_pollers.png)

![](imp_c16_srv_thds.png)

![](imp_c16_srv_pollers_1.png)

![](imp_c16_srv_pollers_2.png)

![](imp_c16_bw.png)

```
mean duration is 282.648762us, total dur: 449.848468s, iops: 1778376.624813, iodepth: 32
```

## 多进程测试

## 单服务端进程配 1 核 & 双客户端进程，每个客户端两核

*1* 号客户端 `spdk_top` 输出：

![](c1_cli1_multi_proc_thds.png)

![](c1_cli1_multi_proc_pollers.png)

*2* 号客户端 `spdk_top` 输出：

![](c1_cli2_multi_proc_thds.png)

![](c1_cli2_multi_proc_pollers.png)

服务端 `spdk_top` 输出：

![](c1_srv_multi_proc_thds.png)

![](c1_srv_multi_proc_pollers.png)

![](c1_multi_proc_bw.png)

```
1 号客户端

mean duration is 442.795024us, total dur: 277.066881s, iops: 72184.737270, iodepth: 32

2 号客户端

mean duration is 442.782575us, total dur: 277.067570s, iops: 72184.557683, iodepth: 32
```

### 单服务端进程配 1 核 & 4 客户端进程，每个客户端 1 核

*1* 号客户端 `spdk_top` 输出：

![](c1_cli_1_thds.png)

![](c1_cli_1_pollers.png)

*2* 号客户端 `spdk_top` 输出：

![](c1_cli_2_thds.png)

![](c1_cli_2_pollers.png)

*3* 号客户端 `spdk_top` 输出：

![](c1_cli_3_thds.png)

![](c1_cli_3_pollers.png)

*4* 号客户端 `spdk_top` 输出：

![](c1_cli_4_thds.png)

![](c1_cli_4_pollers.png)

服务端 `spdk_top` 输出：

![](c1_srv_1_thds.png)

![](c1_srv_1_pollers.png)

![](c1_4_cli_bw.png)

```
1 号客户端

 mean duration is 868.376705us, total dur: 543.078214s, iops: 36827.107957, iodepth: 32

2 号客户端

mean duration is 886.537615us, total dur: 554.420337s, iops: 36073.712790, iodepth: 32

3 号客户端

 mean duration is 886.677031us, total dur: 554.487794s, iops: 36069.324156, iodepth: 32

4 号客户端

mean duration is 881.749097us, total dur: 551.418893s, iops: 36270.066650, iodepth: 32
```

### 单服务端进程配 4 核 & 4 客户端进程，每个客户端 1 核

*1* 号客户端 `spdk_top` 输出：

![](c4_cli_1_thds.png)

![](c4_cli_1_pollers.png)

*2* 号客户端 `spdk_top` 输出：

![](c4_cli_2_thds.png)

![](c4_cli_2_pollers.png)

*3* 号客户端 `spdk_top` 输出：

![](c4_cli_3_thds.png)

![](c4_cli_3_pollers.png)

*4* 号客户端 `spdk_top` 输出：

![](c4_cli_4_thds.png)

![](c4_cli_4_pollers.png)

服务端 `spdk_top` 输出：

![](c4_srv_1_thds.png)

![](c4_srv_1_pollers.png)

![](c4_4_cli_bw.png)

```
1 号客户端

mean duration is 816.408790us, total dur: 140.014080s, iops: 142842.777091, iodepth: 32

2 号客户端

mean duration is 884.331252us, total dur: 147.165580s, iops: 135901.343150, iodepth: 32

3 号客户端

mean duration is 888.713500us, total dur: 147.596866s, iops: 135504.231779, iodepth: 32

4 号客户端

mean duration is 863.402267us, total dur: 143.645071s, iops: 139232.065994, iodepth: 32
```

---

**以下测试均使用如下配置文件：**

服务端配置：

```json
{
    "rdma_device_name": "mlx5_0",
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

客户端配置：

```json
{
    "io_depth": 32,
    "iteration_count": 10000000,
    "rdma_device_name": "mlx5_0",
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

### 双服务端进程，每个进程配 1 核 & 单客户端进程配两核

客户端 `spdk_top` 输出：

![](1c_2srv_1c_1cli_cli_thds.png)
![](1c_2srv_1c_1cli_cli_pollers.png)

*1* 号服务端 `spdk_top` 输出：

![](1c_2srv_1c_1cli_srv1_thds.png)
![](1c_2srv_1c_1cli_srv1_pollers.png)

*2* 号服务端 `spdk_top` 输出：

![](1c_2srv_1c_1cli_srv2_thds.png)
![](1c_2srv_1c_1cli_srv2_pollers.png)

![](1c_2srv_1c_1cli_bw.png)

```
mean duration is 218.071154us, total dur: 206.154485s, iops: 291043.874250, iodepth: 32
```

### 双服务端进程配 4 核 & 单客户端进程配 8 核

客户端 `spdk_top` 输出：

![](4c_2srv_8c_1cli_cli_thds.png)
![](4c_2srv_8c_1cli_cli_pollers.png)

*1* 号服务端 `spdk_top` 输出：

![](4c_2srv_8c_1cli_srv1_thds.png)
![](4c_2srv_8c_1cli_srv1_pollers.png)

*2* 号服务端 `spdk_top` 输出：

![](4c_2srv_8c_1cli_srv2_thds.png)
![](4c_2srv_8c_1cli_srv2_pollers.png)

![](4c_2srv_8c_1cli_bw.png)

```
mean duration is 232.752561us, total dur: 375.683670s, iops: 1064725.544584, iodepth: 32
```

### 4 服务端进程，每个进程配 4 核 & 单客户端进程配 16 核

客户端 `spdk_top` 输出：

![](4c_4srv_16c_1cli_cli_thds.png)
![](4c_4srv_16c_1cli_cli_pollers.png)

*1* 号服务端 `spdk_top` 输出：

![](4c_4srv_16c_1cli_srv1_thds.png)
![](4c_4srv_16c_1cli_srv1_pollers.png)

*2* 号服务端 `spdk_top` 输出：

![](4c_4srv_16c_1cli_srv2_thds.png)
![](4c_4srv_16c_1cli_srv2_pollers.png)

*3* 号服务端 `spdk_top` 输出：

![](4c_4srv_16c_1cli_srv3_thds.png)
![](4c_4srv_16c_1cli_srv3_pollers.png)

*4* 号服务端 `spdk_top` 输出：

![](4c_4srv_16c_1cli_srv4_thds.png)
![](4c_4srv_16c_1cli_srv4_pollers.png)

![](4c_4srv_16c_1cli_bw.png)

```
mean duration is 281.150846us, total dur: 446.608117s, iops: 1791279.579593, iodepth: 32
```

## 跨节点测试

服务端和客户端节点 *RDMA* 网卡参数：

```
CA 'mlx5_0'
	CA type: MT4119
	Number of ports: 1
	Firmware version: 16.31.1014
	Hardware version: 0
	Node GUID: 0xf03f95030052d402
	System image GUID: 0xf03f95030052d402
	Port 1:
		State: Active
		Physical state: LinkUp
		Rate: 100
		Base lid: 2
		LMC: 0
		SM lid: 1
		Capability mask: 0x2651e848
		Port GUID: 0xf03f95030052d402
		Link layer: InfiniBan
```

服务端和客户端节点 *CPU* 参数：

```
架构：                  aarch64
  CPU 运行模式：         64-bit
  字节序：               Little Endian
CPU:                    96
  在线 CPU 列表：       0-95
厂商 ID：               HiSilicon
  BIOS Vendor ID:       HiSilicon
  型号名称：            Kunpeng-920
    BIOS Model name:    Kunpeng 920-4826
    型号：              0
    每个核的线程数：    1
    每个座的核数：      48
    座：                2
    步进：              0x1
    BogoMIPS：          200.00
    标记：              fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma dcpop asimddp asimdfhm
Caches (sum of all):
  L1d:                  6 MiB (96 instances)
  L1i:                  6 MiB (96 instances)
  L2:                   48 MiB (96 instances)
  L3:                   96 MiB (4 instances)
```

### 服务端 8 核 & 客户端 8 核

客户端 `spdk_top` 输出：

![](clu_c8_cli_thds.png)
![](clu_c8_cli_pollers.png)

服务端 `spdk_top` 输出：

![](clu_c8_srv_thds.png)
![](clu_c8_srv_pollers.png)

![](clu_c8_bw.png)

> 上图中，黄线为服务端，绿线代表客户端

```
mean duration is 215.241979us, total dur: 349.701597s, iops: 1143832.351509, iodepth: 32
```

### 服务端 16 核 & 客户端 16 核

客户端 `spdk_top` 输出：

![](clu_c16_cli_thds.png)
![](clu_c16_cli_pollers.png)

服务端 `spdk_top` 输出：

![](clu_c16_srv_thds.png)
![](clu_c16_srv_pollers1.png)
![](clu_c16_srv_pollers2.png)

![](clu_c16_bw.png)

> 上图中，黄线为服务端，绿线代表客户端

```
mean duration is 236.247827us, total dur: 389.745404s, iops: 2052622.022254, iodepth: 32
```

### 服务端 20 核 & 客户端 20 核

客户端 `spdk_top` 输出：

![](clu_c20_cli_thds.png)
![](clu_c20_cli_pollers.png)

服务端 `spdk_top` 输出：

![](clu_c20_srv_thds.png)
![](clu_c20_srv_pollers1.png)
![](clu_c20_srv_pollers2.png)
![](clu_c20_srv_pollers3.png)

![](clu_c20_bw.png)

> 上图中，黄线为服务端，绿线代表客户端

```
mean duration is 276.735028us, total dur: 446.452010s, iops: 2239882.403156, iodepth: 32
```

### 服务端 24 核 & 客户端 24 核

客户端 `spdk_top` 输出：

![](clu_c24_cli_thds.png)
![](clu_c24_cli_pollers.png)

服务端 `spdk_top` 输出：

![](clu_c24_srv_thds.png)
![](clu_c24_srv_pollers1.png)
![](clu_c24_srv_pollers2.png)
![](clu_c24_srv_pollers3.png)

![](clu_c24_bw.png)

> 上图中，黄线为服务端，绿线代表客户端

```
mean duration is 332.406457us, total dur: 521.250175s, iops: 2302157.499238, iodepth: 32
```
