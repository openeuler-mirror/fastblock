性能测试报告
# 性能测试：
## 1. 测试目标
- 测试当前版本的三副本下，多核并发IOPS及平均延迟和延迟分位统计

## 2. 性能测试环境
- 服务器: 此次使用三台同配置的服务器(server2,server3,server4)进行集群模式测试，CPU为96核，4个numa节点的KunPeng 920, cpu采用性能
模式，服务器拥有512G内存
- 网络：  服务器之间使用100G交换机直连,有100G RoCE网络和100G IB网络，因时间原因，主要采用RoCE网络进行测试
- 磁盘：  每台服务完全4块ES3000 V6 SSD作为后端存储, 采用spdk aio用户态驱动进行测试

## 3. 部署集群,并创建所需的pool和卷
### 3.1 部署monitor
monitor运行在server3，使用的rdma网卡是mlx5_1,以下命令行部署并启动monitor进程:  
```
./vstart_multicore.sh -m pro -n mlx5_1 -M 192.168.80.83
```

### 3.2 部署osd

在server2,server3,server4三个节点上挂载2M大页内存，并准备huge page:  
```
for i in `seq 0 3`
do
  echo 32768 > /sys/devices/system/node/node"$i"/hugepages/hugepages-2048kB/nr_hugepages
done
```

由于每个服务器上只有4个nvme磁盘（nvme0n1、nvme1n1、nvme2n1、nvme3n1），太少，因此使用aio模式，每个磁盘4个分区，一个分区作为一个osd的后端存储，因此一个节点上可以启动16个osd.
部署osd:
```
for i in `seq 82 84`
do
  ./vstart_multicore.sh -m pro -n mlx5_1 -t aio -d ${disks} -i 192.168.80."$i"  
done
```
其中参数disks是使用到的磁盘路径，多个磁盘以“,”隔开，使用16个是 “/dev/nvme0n1p1,/dev/nvme0n1p2,/dev/nvme0n1p3,/dev/nvme0n1p4,/dev/nvme1n1p1,/dev/nvme1n1p2,/dev/nvme1n1p3,/dev/nvme1n1p4,/dev/nvme2n1p1,/dev/nvme2n1p2,/dev/nvme2n1p3,/dev/nvme2n1p4,/dev/nvme3n1p1,/dev/nvme3n1p2,/dev/nvme3n1p3,/dev/nvme3n1p4”

vstart_multicore.sh修改自vstart.sh：每个osd使用两个cpu核

### 3.3 创建pool
创建pool:
fastblock-client -op=createpool -poolname=iopstest -pgcount=16 -pgsize=3 -failure_domain=host

集群部署成功后，可以通过命令行看到集群运行状态和osdmap及pgmap: 
```
[root@server3 fastblock.bak]# fastblock-client -op=status
  cluster:

  services:
    mon: 1 mons, 192.168.80.83
    osd: 48 osds: 48 up, 48 in

  data:
    pools  : 1 pools, 16 pgs
    objects: 0 objects
    pgs    :
      16  active
```

```
[root@server3 fastblock.bak]# fastblock-client -op=getpgmap
PGID         STATE                       OSDLIST                      NEWOSDLIST
1.0          active                      [17,33,1]                    []
1.1          active                      [2,34,18]                    []
1.2          active                      [35,19,3]                    []
1.3          active                      [20,4,36]                    []
1.4          active                      [5,21,37]                    []
1.5          active                      [38,22,6]                    []
1.6          active                      [23,39,7]                    []
1.7          active                      [8,24,40]                    []
1.8          active                      [41,9,25]                    []
1.9          active                      [26,10,42]                   []
1.10         active                      [11,27,43]                   []
1.11         active                      [44,12,28]                   []
1.12         active                      [29,45,13]                   []
1.13         active                      [14,46,30]                   []
1.14         active                      [47,31,15]                   []
1.15         active                      [48,32,16]                   []
```

```
[root@server3 fastblock.bak]# fastblock-client -op=getosdmap
OSDID        ADDRESS                PORT          STATUS-UP    STATUS-IN
1            192.168.80.83          9412          up           in
2            192.168.80.83          9593          up           in
3            192.168.80.83          9260          up           in
4            192.168.80.83          9241          up           in
5            192.168.80.83          9035          up           in
6            192.168.80.83          9866          up           in
7            192.168.80.83          9015          up           in
8            192.168.80.83          9126          up           in
9            192.168.80.83          9311          up           in
10           192.168.80.83          9496          up           in
11           192.168.80.83          9174          up           in
12           192.168.80.83          9351          up           in
13           192.168.80.83          9921          up           in
14           192.168.80.83          9193          up           in
15           192.168.80.83          9352          up           in
16           192.168.80.83          9671          up           in
17           192.168.80.82          9443          up           in
18           192.168.80.82          9915          up           in
19           192.168.80.82          9849          up           in
20           192.168.80.82          9591          up           in
21           192.168.80.82          9310          up           in
22           192.168.80.82          9784          up           in
23           192.168.80.82          9294          up           in
24           192.168.80.82          9238          up           in
25           192.168.80.82          9868          up           in
26           192.168.80.82          9047          up           in
27           192.168.80.82          9750          up           in
28           192.168.80.82          9216          up           in
29           192.168.80.82          9441          up           in
30           192.168.80.82          9923          up           in
31           192.168.80.82          9593          up           in
32           192.168.80.82          9954          up           in
33           192.168.80.84          9435          up           in
34           192.168.80.84          9990          up           in
35           192.168.80.84          9540          up           in
36           192.168.80.84          9236          up           in
37           192.168.80.84          9495          up           in
38           192.168.80.84          9209          up           in
39           192.168.80.84          9557          up           in
40           192.168.80.84          9442          up           in
41           192.168.80.84          9319          up           in
42           192.168.80.84          9391          up           in
43           192.168.80.84          9678          up           in
44           192.168.80.84          9730          up           in
45           192.168.80.84          9627          up           in
46           192.168.80.84          9918          up           in
47           192.168.80.84          9458          up           in
48           192.168.80.84          9349          up           in
```

### 3.4 使用block_bench测试iops
block_bench的配置文件iops.json：
```
{
  "io_dump_enable": false,
  "io_dump_csv_path": "block_bench_iops.csv",
  "io_print_stats_enable": true,
  "io_print_stats_take_single_core": true,
  "io_print_stats_interval_ms": 1000,
  "io_print_stats_acc": false,
  "io_type": "write",
  "io_size": 4096,
  "io_count": 5000000,
  "io_depth": 64,
  "io_queue_size": 1024,
  "io_queue_request": 4096,
  "image_name": "iopsimage2",
  "image_size": "10737418240",
  "object_size": 4194304,
  "pool_id": 1,
  "pool_name": "iopstest",
  "rdma_device_name": "mlx5_1",
  "mon_host": [
    "192.168.80.83"
  ],
  "msg_client_poll_cq_batch_size": 32,
  "msg_client_metadata_memory_pool_capacity": 16384,
  "msg_client_metadata_memory_pool_element_size": 1024,
  "msg_client_data_memory_pool_capacity": 16384,
  "msg_client_data_memory_pool_element_size": 8192,
  "msg_client_per_post_recv_num": 512,
  "msg_client_rpc_timeout_us": 600000000,
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
  "msg_rdma_qp_sig_all": false,
  "msg_server_rpc_timeout_us": 600000000
}
```

block_bench -m '[95,72,48,49,73,94,50,74]' -C iops.json
结果：
```
p0.1: 811.12us, p0.5: 990.15us, p0.9: 1194.65us, p0.95: 1260.64us, p0.99: 1379.23us, p0.999: 1550.19us, mean: 1008.94us, min: 654.01us, max: 16651.9us, biased stdv: 407.352, iops: 392450, iops_dur_sec: 1s, total_io_count: 392451
p0.1: 825.87us, p0.5: 1008.05us, p0.9: 1212.96us, p0.95: 1270.15us, p0.99: 1384.32us, p0.999: 1552.86us, mean: 1016.16us, min: 677.33us, max: 1639.59us, biased stdv: 147.042, iops: 441039, iops_dur_sec: 1.00001s, total_io_count: 441042
p0.1: 807.17us, p0.5: 993.56us, p0.9: 1202.77us, p0.95: 1259.54us, p0.99: 1368.77us, p0.999: 1529us, mean: 1015.43us, min: 661.56us, max: 33253.6us, biased stdv: 674.991, iops: 441779, iops_dur_sec: 1s, total_io_count: 441780
p0.1: 807.33us, p0.5: 991.41us, p0.9: 1200.5us, p0.95: 1263.22us, p0.99: 1397.05us, p0.999: 1590.27us, mean: 1019.24us, min: 672.8us, max: 33436.9us, biased stdv: 771.942, iops: 439772, iops_dur_sec: 1s, total_io_count: 439772
p0.1: 825.44us, p0.5: 998.98us, p0.9: 1200.88us, p0.95: 1261.56us, p0.99: 1384.19us, p0.999: 1724.96us, mean: 1009.61us, min: 671.28us, max: 2168.35us, biased stdv: 146.693, iops: 444238, iops_dur_sec: 1.00001s, total_io_count: 444242
p0.1: 824.79us, p0.5: 1007.02us, p0.9: 1210.46us, p0.95: 1269.04us, p0.99: 1383.87us, p0.999: 1513.19us, mean: 1015.15us, min: 665.53us, max: 1614.25us, biased stdv: 146.021, iops: 441690, iops_dur_sec: 1.00001s, total_io_count: 441692
p0.1: 824.44us, p0.5: 1006.54us, p0.9: 1217.46us, p0.95: 1274.63us, p0.99: 1386.37us, p0.999: 1531.48us, mean: 1016us, min: 669.14us, max: 1674.17us, biased stdv: 148.354, iops: 441400, iops_dur_sec: 1s, total_io_count: 441400
p0.1: 826us, p0.5: 999.44us, p0.9: 1199.81us, p0.95: 1258.37us, p0.99: 1377.43us, p0.999: 1475.08us, mean: 1008.78us, min: 668.47us, max: 1560.57us, biased stdv: 142.391, iops: 444626, iops_dur_sec: 1.00001s, total_io_count: 444629
p0.1: 789.26us, p0.5: 978.85us, p0.9: 1192.64us, p0.95: 1259.62us, p0.99: 1379.69us, p0.999: 2435.72us, mean: 1016.95us, min: 662.7us, max: 66891.2us, biased stdv: 1355.17, iops: 441141, iops_dur_sec: 1s, total_io_count: 441141
p0.1: 781.68us, p0.5: 982.68us, p0.9: 1193.13us, p0.95: 1252.53us, p0.99: 1380.77us, p0.999: 1603.48us, mean: 1026.27us, min: 646.1us, max: 65863.6us, biased stdv: 1544.22, iops: 438094, iops_dur_sec: 1.00001s, total_io_count: 438096
p0.1: 821.45us, p0.5: 994.96us, p0.9: 1195.4us, p0.95: 1258.57us, p0.99: 1370.13us, p0.999: 1496.43us, mean: 1004.02us, min: 652.25us, max: 1690.69us, biased stdv: 143.002, iops: 445466, iops_dur_sec: 1.00001s, total_io_count: 445469
===============================[all iops stats]========================================
mean: 437427, min: 392450, max: 445466, biased stdv: 14369.9, total_io_count: 11
mean: 62773.2, min: 55722.9, max: 65601.9, biased stdv: 2508.59, total_io_count: 11
mean: 62695.4, min: 56698.9, max: 65641.6, biased stdv: 2279.19, total_io_count: 11
mean: 62649.1, min: 56800.9, max: 65847.6, biased stdv: 2308, total_io_count: 11
mean: 62836.2, min: 56822.9, max: 66118.6, biased stdv: 2293.44, total_io_count: 11
mean: 62494.5, min: 55384.9, max: 65127.9, biased stdv: 2540.99, total_io_count: 11
mean: 62456.4, min: 55299.9, max: 65331.9, biased stdv: 2578.91, total_io_count: 11
mean: 62741.5, min: 55931.9, max: 65618.9, biased stdv: 2436.2, total_io_count: 11
===============================[write latency]========================================
p0.1: 811.24us, p0.5: 995.34us, p0.9: 1202.3us, p0.95: 1263us, p0.99: 1382.21us, p0.999: 1554.44us, mean: 1013.47us, min: 157.25us, max: 66891.2us, biased stdv: 699.215, iops: 441148, iops_dur_sec: 11.3341s, total_io_count: 5000000
======================================================================================
===============================[all iops stats]========================================
mean: 437427, min: 392450, max: 445466, biased stdv: 14369.9, total_io_count: 11
mean: 62773.2, min: 55722.9, max: 65601.9, biased stdv: 2508.59, total_io_count: 11
mean: 62695.4, min: 56698.9, max: 65641.6, biased stdv: 2279.19, total_io_count: 11
mean: 62649.1, min: 56800.9, max: 65847.6, biased stdv: 2308, total_io_count: 11
mean: 62836.2, min: 56822.9, max: 66118.6, biased stdv: 2293.44, total_io_count: 11
mean: 62494.5, min: 55384.9, max: 65127.9, biased stdv: 2540.99, total_io_count: 11
mean: 62456.4, min: 55299.9, max: 65331.9, biased stdv: 2578.91, total_io_count: 11
mean: 62741.5, min: 55931.9, max: 65618.9, biased stdv: 2436.2, total_io_count: 11
```
从结果可知iops为441148

### 3.5 使用block_bench测试延时
新建一个只有1个pg的pool
fastblock-client -op=createpool -poolname=iopstest1 -pgcount=1 -pgsize=3 -failure_domain=host

```

pgmap：
[root@server3 fastblock.bak]# fastblock-client -op=getpgmap  | grep 2.0
2.0          active                      [47,30,12]                   []
```

block_bench的配置文件iops.json：
```
{
  "io_dump_enable": false,
  "io_dump_csv_path": "block_bench_iops.csv",
  "io_print_stats_enable": true,
  "io_print_stats_take_single_core": true,
  "io_print_stats_interval_ms": 1000,
  "io_print_stats_acc": false,
  "io_type": "write",
  "io_size": 4096,
  "io_count": 100000,
  "io_depth": 1,
  "io_queue_size": 1024,
  "io_queue_request": 4096,
  "image_name": "iopsimage2",
  "image_size": "10737418240",
  "object_size": 4194304,
  "pool_id": 2,
  "pool_name": "iopstest1",
  "rdma_device_name": "mlx5_1",
  "mon_host": [
    "192.168.80.83"
  ],
  "msg_client_poll_cq_batch_size": 32,
  "msg_client_metadata_memory_pool_capacity": 16384,
  "msg_client_metadata_memory_pool_element_size": 1024,
  "msg_client_data_memory_pool_capacity": 16384,
  "msg_client_data_memory_pool_element_size": 8192,
  "msg_client_per_post_recv_num": 512,
  "msg_client_rpc_timeout_us": 600000000,
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
  "msg_rdma_qp_sig_all": false,
  "msg_server_rpc_timeout_us": 600000000
}
```

block_bench -m '[95]' -C iops.json
```
===============================[write latency]========================================
p0.1: 122.74us, p0.5: 126.18us, p0.9: 129.97us, p0.95: 131.98us, p0.99: 139.03us, p0.999: 182.55us, mean: 126.596us, min: 114.47us, max: 1754.91us, biased stdv: 10.0064, iops: 7895, iops_dur_sec: 12.6662s, total_io_count: 100000
```
由结果可知平均延时为126.596us
