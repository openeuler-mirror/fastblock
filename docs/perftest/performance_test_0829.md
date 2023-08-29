性能测试报告
# 测试环境
- commit hash: `1f04b215124238ce6ef0d2322c17d00d71ef6411`, Release模式编译
- 服务器: 40核80线程，网卡25G支持RDMA，384G内存, CPU采用性能模式
- 磁盘：测试是采用3个P4510作为后端存储(仅启动三个osd), 采用spdk nvme用户态驱动和aio bdev分别进行测试
- 测试项: 单pg测试，分别测试了一副本、二副本和三副本下的fbbench的write和rpc，以及一副本下的fbbench的rpc测试, 另外benchmark工具也能够控制写同一个对象还是不通对象，且能够展示ops、平均延迟和延迟分位统计
- 重大变更事项: 相比8.17测试报告，本次最大变更时对关键路径上的SPDK_NOTICELOG进行了注释，因为测试结果表明，这些注释对性能有超乎想象的影响, rsyslog体系会成为重大性能瓶颈
- 其它: 测试时，所有进程都仅使用一个核，且是大核，避免使用同一个物理核上的两个线程
- 其它：因目前raft log回收存在bug，此次测试时因iops较高会导致raft log空间不够用，所以修改了raft log的size为100GB

# 测试脚本:
参考`tools/bench_singlepg.sh`进行修改，适配aio bdev和nvme bdev, 并支持1、2、3副本测试, 配置方式可参考src/osd/readme.md  

# 测试结果:
## 单PG的fbbench的rpc测试:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 30 -m 0x8 -T rpc
[2023-08-29 14:57:10.851857] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 14:57:10.851987] [ DPDK EAL parameters: [2023-08-29 14:57:10.852017] fbbench [2023-08-29 14:57:10.852041] --no-shconf [2023-08-29 14:57:10.852061] -c 0x8 [2023-08-29 14:57:10.852082] --huge-unlink [2023-08-29 14:57:10.852104] --log-level=lib.eal:6 [2023-08-29 14:57:10.852138] --log-level=lib.cryptodev:5 [2023-08-29 14:57:10.852159] --log-level=user1:6 [2023-08-29 14:57:10.852178] --iova-mode=pa [2023-08-29 14:57:10.852201] --base-virtaddr=0x200000000000 [2023-08-29 14:57:10.852222] --match-allocations [2023-08-29 14:57:10.852243] --file-prefix=spdk_pid2750062 [2023-08-29 14:57:10.852266] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 14:57:10.941601] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 14:57:10.941632] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 14:57:11.090814] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 14:57:11.090892] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 14:57:11.136549] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 14:57:11.136605] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x55cf27925b40
[2023-08-29 14:57:11.155680] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x55cf27ea1b20, a new poll group = 0x55cf27ea1ef0
[2023-08-29 14:57:11.155741] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 14:57:11.155756] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 14:57:11.155797] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 14:57:11.155811] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 14:57:11.156137] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 14:57:11.158539] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x55cf27faf380, ctrlr is 0x55cf27ea1b20, group is 0x55cf27ea1ef0
[2023-08-29 14:57:11.158565] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 14:57:11.169899] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 14:57:11.169931] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 14:57:11.169942] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 14:57:12.138883] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 86153
[2023-08-29 14:57:13.141239] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89376
[2023-08-29 14:57:14.143583] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89523
[2023-08-29 14:57:15.145935] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89314
[2023-08-29 14:57:16.148283] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89407
[2023-08-29 14:57:17.150633] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89215
[2023-08-29 14:57:18.152988] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 88776
[2023-08-29 14:57:19.155337] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89261
[2023-08-29 14:57:20.157687] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89406
[2023-08-29 14:57:21.160038] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 88855
[2023-08-29 14:57:22.162387] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89188
[2023-08-29 14:57:23.164738] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89245
[2023-08-29 14:57:24.167087] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89232
[2023-08-29 14:57:25.169437] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89278
[2023-08-29 14:57:26.171790] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 88914
[2023-08-29 14:57:27.174137] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89303
[2023-08-29 14:57:28.176488] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89411
[2023-08-29 14:57:29.178837] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89418
[2023-08-29 14:57:30.181188] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89012
[2023-08-29 14:57:31.183539] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89332
[2023-08-29 14:57:32.185888] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89477
[2023-08-29 14:57:33.188239] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89424
[2023-08-29 14:57:34.190589] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89334
[2023-08-29 14:57:35.192940] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89583
[2023-08-29 14:57:36.195290] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89478
[2023-08-29 14:57:37.197641] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89333
[2023-08-29 14:57:38.199991] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89442
[2023-08-29 14:57:39.202344] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89256
[2023-08-29 14:57:40.204692] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89356
[2023-08-29 14:57:41.207042] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 89183
ops is: 89182, average latency is 11.213025us
latency histogram: min:10.000000us 10.000000%:10.727619us 50.000000%:11.032381us 90.000000%:11.702857us 95.000000%:12.007619us 99.000000%:13.287619us 99.900000%:16.822857us max:10163.000000us
[2023-08-29 14:57:41.207180] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```
相比8.17版本，本版本的延迟从18us降低到11.2us，ops从54832提升到了89192，iops提升了62%.

## 单PG一副本测试
### 写不同对象,采用aio bdev:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 10:06:14.202481] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 10:06:14.202614] [ DPDK EAL parameters: [2023-08-29 10:06:14.202644] fbbench [2023-08-29 10:06:14.202669] --no-shconf [2023-08-29 10:06:14.202690] -c 0x8 [2023-08-29 10:06:14.202714] --huge-unlink [2023-08-29 10:06:14.202735] --log-level=lib.eal:6 [2023-08-29 10:06:14.202757] --log-level=lib.cryptodev:5 [2023-08-29 10:06:14.202777] --log-level=user1:6 [2023-08-29 10:06:14.202799] --iova-mode=pa [2023-08-29 10:06:14.202820] --base-virtaddr=0x200000000000 [2023-08-29 10:06:14.202842] --match-allocations [2023-08-29 10:06:14.202864] --file-prefix=spdk_pid2636291 [2023-08-29 10:06:14.202886] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 10:06:14.288333] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 10:06:14.288361] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 10:06:14.435255] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 10:06:14.435322] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 10:06:14.481038] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 10:06:14.481090] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x55e6c51aab40
[2023-08-29 10:06:14.524727] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x55e6c5726b20, a new poll group = 0x55e6c5726ef0
[2023-08-29 10:06:14.524792] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 10:06:14.524814] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 10:06:14.524874] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 10:06:14.524893] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 10:06:14.525244] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 10:06:14.528165] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x55e6c5834380, ctrlr is 0x55e6c5726b20, group is 0x55e6c5726ef0
[2023-08-29 10:06:14.528204] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 10:06:14.539140] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 10:06:14.539172] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 10:06:14.539184] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 10:06:15.483387] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 24984
[2023-08-29 10:06:16.485753] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 26557
[2023-08-29 10:06:17.488118] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 26814
[2023-08-29 10:06:18.490485] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27090
[2023-08-29 10:06:19.492851] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27267
[2023-08-29 10:06:20.495217] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27321
[2023-08-29 10:06:21.497582] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27242
[2023-08-29 10:06:22.499948] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27245
[2023-08-29 10:06:23.502314] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 26995
[2023-08-29 10:06:24.504680] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27244
[2023-08-29 10:06:25.507045] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27205
[2023-08-29 10:06:26.509411] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27005
[2023-08-29 10:06:27.511776] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27203
[2023-08-29 10:06:28.514147] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 26239
[2023-08-29 10:06:29.516516] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27130
[2023-08-29 10:06:30.518881] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27016
[2023-08-29 10:06:31.521247] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27018
[2023-08-29 10:06:32.523613] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27123
[2023-08-29 10:06:33.525978] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27143
[2023-08-29 10:06:34.528344] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 27179
ops is: 26951, average latency is 37.104375us
latency histogram: min:29.000000us 10.000000%:33.645714us 50.000000%:34.864762us 90.000000%:37.790476us 95.000000%:40.716190us 99.000000%:78.019048us 99.900000%:144.335238us max:26514.000000us
[2023-08-29 10:06:34.528482] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```
### 写不同对象,使用nvme bdev: 
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 10:10:34.973843] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 10:10:34.973930] [ DPDK EAL parameters: [2023-08-29 10:10:34.973950] fbbench [2023-08-29 10:10:34.973966] --no-shconf [2023-08-29 10:10:34.973983] -c 0x8 [2023-08-29 10:10:34.974000] --huge-unlink [2023-08-29 10:10:34.974015] --log-level=lib.eal:6 [2023-08-29 10:10:34.974030] --log-level=lib.cryptodev:5 [2023-08-29 10:10:34.974046] --log-level=user1:6 [2023-08-29 10:10:34.974061] --iova-mode=pa [2023-08-29 10:10:34.974077] --base-virtaddr=0x200000000000 [2023-08-29 10:10:34.974092] --match-allocations [2023-08-29 10:10:34.974107] --file-prefix=spdk_pid2638050 [2023-08-29 10:10:34.974122] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 10:10:35.059188] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 10:10:35.059213] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 10:10:35.205544] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 10:10:35.205615] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 10:10:35.251293] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 10:10:35.251345] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x55d50dcf7b40
[2023-08-29 10:10:35.299732] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x55d50e273b20, a new poll group = 0x55d50e273ef0
[2023-08-29 10:10:35.299794] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 10:10:35.299814] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 10:10:35.299867] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 10:10:35.299885] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 10:10:35.300186] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 10:10:35.303099] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x55d50e381380, ctrlr is 0x55d50e273b20, group is 0x55d50e273ef0
[2023-08-29 10:10:35.303136] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 10:10:35.320642] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 10:10:35.320673] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 10:10:35.320685] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 10:10:36.253640] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 28831
[2023-08-29 10:10:37.256004] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30600
[2023-08-29 10:10:38.258373] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30364
[2023-08-29 10:10:39.260737] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30405
[2023-08-29 10:10:40.263102] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30006
[2023-08-29 10:10:41.265467] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30028
[2023-08-29 10:10:42.267831] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 29917
[2023-08-29 10:10:43.270195] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30077
[2023-08-29 10:10:44.272559] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30196
[2023-08-29 10:10:45.274925] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30043
[2023-08-29 10:10:46.277269] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 29800
[2023-08-29 10:10:47.279576] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30041
[2023-08-29 10:10:48.281882] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 29956
[2023-08-29 10:10:49.284189] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30239
[2023-08-29 10:10:50.286496] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30010
[2023-08-29 10:10:51.288803] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 29875
[2023-08-29 10:10:52.291109] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 29933
[2023-08-29 10:10:53.293417] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 29920
[2023-08-29 10:10:54.295724] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30110
[2023-08-29 10:10:55.298031] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30176
ops is: 30026, average latency is 33.304469us
latency histogram: min:27.000000us 10.000000%:29.988571us 50.000000%:31.085714us 90.000000%:33.645714us 95.000000%:35.596190us 99.000000%:77.043810us 99.900000%:136.533333us max:16457.000000us
[2023-08-29 10:10:55.298170] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```

### 写相同对象，使用aio bdev:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 10:42:36.610393] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 10:42:36.610506] [ DPDK EAL parameters: [2023-08-29 10:42:36.610531] fbbench [2023-08-29 10:42:36.610552] --no-shconf [2023-08-29 10:42:36.610572] -c 0x8 [2023-08-29 10:42:36.610592] --huge-unlink [2023-08-29 10:42:36.610610] --log-level=lib.eal:6 [2023-08-29 10:42:36.610629] --log-level=lib.cryptodev:5 [2023-08-29 10:42:36.610648] --log-level=user1:6 [2023-08-29 10:42:36.610668] --iova-mode=pa [2023-08-29 10:42:36.610687] --base-virtaddr=0x200000000000 [2023-08-29 10:42:36.610707] --match-allocations [2023-08-29 10:42:36.610728] --file-prefix=spdk_pid2649839 [2023-08-29 10:42:36.610748] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 10:42:36.694878] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 10:42:36.694904] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 10:42:36.840773] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 10:42:36.840844] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 10:42:36.886438] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 10:42:36.886493] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x55f099342b40
[2023-08-29 10:42:36.948286] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x55f0998beb20, a new poll group = 0x55f0998beef0
[2023-08-29 10:42:36.948377] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 10:42:36.948408] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 10:42:36.948483] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 10:42:36.948510] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 10:42:36.948930] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 10:42:36.952621] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x55f0999cc380, ctrlr is 0x55f0998beb20, group is 0x55f0998beef0
[2023-08-29 10:42:36.952663] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 10:42:36.969077] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 10:42:36.969122] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 10:42:36.969141] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 10:42:37.888775] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 28880
[2023-08-29 10:42:38.891127] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31413
[2023-08-29 10:42:39.893481] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31411
[2023-08-29 10:42:40.895834] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31401
[2023-08-29 10:42:41.898186] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31351
[2023-08-29 10:42:42.900540] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31292
[2023-08-29 10:42:43.902892] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31331
[2023-08-29 10:42:44.905250] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31165
[2023-08-29 10:42:45.907603] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31346
[2023-08-29 10:42:46.909956] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31346
[2023-08-29 10:42:47.912308] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31328
[2023-08-29 10:42:48.914661] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31454
[2023-08-29 10:42:49.917014] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31384
[2023-08-29 10:42:50.919368] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31335
[2023-08-29 10:42:51.921721] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 30914
[2023-08-29 10:42:52.924073] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31318
[2023-08-29 10:42:53.926426] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31239
[2023-08-29 10:42:54.928779] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31364
[2023-08-29 10:42:55.931132] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31363
[2023-08-29 10:42:56.933485] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 31432
ops is: 31203, average latency is 32.048200us
latency histogram: min:27.000000us 10.000000%:30.841905us 50.000000%:31.695238us 90.000000%:33.158095us 95.000000%:33.889524us 99.000000%:38.278095us 99.900000%:42.422857us max:16218.000000us
[2023-08-29 10:42:56.933624] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```

### 写相同对象，nvme bdev:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 10:40:21.018856] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 10:40:21.018955] [ DPDK EAL parameters: [2023-08-29 10:40:21.018976] fbbench [2023-08-29 10:40:21.018995] --no-shconf [2023-08-29 10:40:21.019012] -c 0x8 [2023-08-29 10:40:21.019029] --huge-unlink [2023-08-29 10:40:21.019044] --log-level=lib.eal:6 [2023-08-29 10:40:21.019060] --log-level=lib.cryptodev:5 [2023-08-29 10:40:21.019077] --log-level=user1:6 [2023-08-29 10:40:21.019093] --iova-mode=pa [2023-08-29 10:40:21.019109] --base-virtaddr=0x200000000000 [2023-08-29 10:40:21.019125] --match-allocations [2023-08-29 10:40:21.019142] --file-prefix=spdk_pid2648010 [2023-08-29 10:40:21.019159] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 10:40:21.104472] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 10:40:21.104498] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 10:40:21.250714] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 10:40:21.250786] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 10:40:21.296450] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 10:40:21.296503] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x561cf221ab40
[2023-08-29 10:40:21.343151] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x561cf2796b20, a new poll group = 0x561cf2796ef0
[2023-08-29 10:40:21.343212] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 10:40:21.343231] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 10:40:21.343288] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 10:40:21.343303] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 10:40:21.343614] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 10:40:21.346324] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x561cf28a4380, ctrlr is 0x561cf2796b20, group is 0x561cf2796ef0
[2023-08-29 10:40:21.346359] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 10:40:21.358162] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 10:40:21.358196] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 10:40:21.358210] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 10:40:22.298788] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 33474
[2023-08-29 10:40:23.301140] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35685
[2023-08-29 10:40:24.303493] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35414
[2023-08-29 10:40:25.305845] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35431
[2023-08-29 10:40:26.308197] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35383
[2023-08-29 10:40:27.310550] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35539
[2023-08-29 10:40:28.312902] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35228
[2023-08-29 10:40:29.315255] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35569
[2023-08-29 10:40:30.317607] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35448
[2023-08-29 10:40:31.319960] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35434
[2023-08-29 10:40:32.322313] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35317
[2023-08-29 10:40:33.324665] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35311
[2023-08-29 10:40:34.327021] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35307
[2023-08-29 10:40:35.329377] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35072
[2023-08-29 10:40:36.331729] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35322
[2023-08-29 10:40:37.334082] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35503
[2023-08-29 10:40:38.336434] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35312
[2023-08-29 10:40:39.338787] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35337
[2023-08-29 10:40:40.341140] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 35178
[2023-08-29 10:40:41.343492] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 34687
ops is: 35247, average latency is 28.371209us
latency histogram: min:24.000000us 10.000000%:26.697143us 50.000000%:27.550476us 90.000000%:29.013333us 95.000000%:29.866667us 99.000000%:34.377143us 99.900000%:96.548571us max:12583.000000us
[2023-08-29 10:40:41.343630] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```

## 单PG二副本测试
### 写相同对象，aio bdev:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 10:27:46.317750] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 10:27:46.317852] [ DPDK EAL parameters: [2023-08-29 10:27:46.317875] fbbench [2023-08-29 10:27:46.317895] --no-shconf [2023-08-29 10:27:46.317913] -c 0x8 [2023-08-29 10:27:46.317931] --huge-unlink [2023-08-29 10:27:46.317947] --log-level=lib.eal:6 [2023-08-29 10:27:46.317964] --log-level=lib.cryptodev:5 [2023-08-29 10:27:46.317982] --log-level=user1:6 [2023-08-29 10:27:46.317999] --iova-mode=pa [2023-08-29 10:27:46.318016] --base-virtaddr=0x200000000000 [2023-08-29 10:27:46.318033] --match-allocations [2023-08-29 10:27:46.318051] --file-prefix=spdk_pid2646141 [2023-08-29 10:27:46.318070] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 10:27:46.403125] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 10:27:46.403151] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 10:27:46.549664] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 10:27:46.549735] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 10:27:46.595371] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 10:27:46.595423] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x55a52ba15b40
[2023-08-29 10:27:46.638973] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x55a52bf91b20, a new poll group = 0x55a52bf91ef0
[2023-08-29 10:27:46.639041] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 10:27:46.639063] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 10:27:46.639117] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 10:27:46.639135] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 10:27:46.639478] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 10:27:46.641944] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x55a52c09f380, ctrlr is 0x55a52bf91b20, group is 0x55a52bf91ef0
[2023-08-29 10:27:46.641977] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 10:27:46.650042] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 10:27:46.650071] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 10:27:46.650083] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 10:27:47.597703] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 15907
[2023-08-29 10:27:48.600050] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16855
[2023-08-29 10:27:49.602398] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16829
[2023-08-29 10:27:50.604746] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16858
[2023-08-29 10:27:51.607093] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16901
[2023-08-29 10:27:52.609441] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16859
[2023-08-29 10:27:53.611790] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16824
[2023-08-29 10:27:54.614138] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16813
[2023-08-29 10:27:55.616486] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16836
[2023-08-29 10:27:56.618833] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16820
[2023-08-29 10:27:57.621181] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16807
[2023-08-29 10:27:58.623529] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16851
[2023-08-29 10:27:59.625876] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16866
[2023-08-29 10:28:00.628224] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16705
[2023-08-29 10:28:01.630571] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16830
[2023-08-29 10:28:02.632919] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16856
[2023-08-29 10:28:03.635267] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16827
[2023-08-29 10:28:04.637614] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16849
[2023-08-29 10:28:05.639962] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16885
[2023-08-29 10:28:06.642311] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16836
ops is: 16790, average latency is 59.559261us
latency histogram: min:55.000000us 10.000000%:58.026667us 50.000000%:59.001905us 90.000000%:60.708571us 95.000000%:61.683810us 99.000000%:65.828571us 99.900000%:72.167619us max:7498.000000us
```

### 写相同对象，nvme bdev:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8002 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 10:32:24.586095] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 10:32:24.586209] [ DPDK EAL parameters: [2023-08-29 10:32:24.586238] fbbench [2023-08-29 10:32:24.586263] --no-shconf [2023-08-29 10:32:24.586285] -c 0x8 [2023-08-29 10:32:24.586306] --huge-unlink [2023-08-29 10:32:24.586326] --log-level=lib.eal:6 [2023-08-29 10:32:24.586348] --log-level=lib.cryptodev:5 [2023-08-29 10:32:24.586370] --log-level=user1:6 [2023-08-29 10:32:24.586392] --iova-mode=pa [2023-08-29 10:32:24.586414] --base-virtaddr=0x200000000000 [2023-08-29 10:32:24.586436] --match-allocations [2023-08-29 10:32:24.586457] --file-prefix=spdk_pid2647684 [2023-08-29 10:32:24.586480] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 10:32:24.670943] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 10:32:24.670967] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 10:32:24.816940] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 10:32:24.817010] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 10:32:24.862648] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 10:32:24.862699] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x55b719379b40
[2023-08-29 10:32:24.902666] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x55b7198f5b20, a new poll group = 0x55b7198f5ef0
[2023-08-29 10:32:24.902727] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8002) in core 3
[2023-08-29 10:32:24.902746] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8002...
[2023-08-29 10:32:24.902795] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 10:32:24.902812] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 10:32:24.903138] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 10:32:24.905817] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8002 done, conn is 0x55b719a03380, ctrlr is 0x55b7198f5b20, group is 0x55b7198f5ef0
[2023-08-29 10:32:24.905854] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8002
[2023-08-29 10:32:24.915289] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 10:32:24.915323] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 10:32:24.915337] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 10:32:25.864981] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 18773
[2023-08-29 10:32:26.867332] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19967
[2023-08-29 10:32:27.869682] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19889
[2023-08-29 10:32:28.872032] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19941
[2023-08-29 10:32:29.874382] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19975
[2023-08-29 10:32:30.876732] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19863
[2023-08-29 10:32:31.879083] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19990
[2023-08-29 10:32:32.881433] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19853
[2023-08-29 10:32:33.883788] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19940
[2023-08-29 10:32:34.886139] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19944
[2023-08-29 10:32:35.888490] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19862
[2023-08-29 10:32:36.890840] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19822
[2023-08-29 10:32:37.893190] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19930
[2023-08-29 10:32:38.895540] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19808
[2023-08-29 10:32:39.897891] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19884
[2023-08-29 10:32:40.900241] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19835
[2023-08-29 10:32:41.902591] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19882
[2023-08-29 10:32:42.904942] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19851
[2023-08-29 10:32:43.907292] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19840
[2023-08-29 10:32:44.909642] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 19877
ops is: 19836, average latency is 50.413390us
latency histogram: min:46.000000us 10.000000%:48.761905us 50.000000%:49.737143us 90.000000%:51.443810us 95.000000%:52.419048us 99.000000%:57.295238us 99.900000%:98.499048us max:8845.000000us
[2023-08-29 10:32:44.909782] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```


### 写不同对象，nvme bdev:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 10:16:46.486257] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 10:16:46.486399] [ DPDK EAL parameters: [2023-08-29 10:16:46.486430] fbbench [2023-08-29 10:16:46.486455] --no-shconf [2023-08-29 10:16:46.486476] -c 0x8 [2023-08-29 10:16:46.486500] --huge-unlink [2023-08-29 10:16:46.486521] --log-level=lib.eal:6 [2023-08-29 10:16:46.486545] --log-level=lib.cryptodev:5 [2023-08-29 10:16:46.486567] --log-level=user1:6 [2023-08-29 10:16:46.486589] --iova-mode=pa [2023-08-29 10:16:46.486611] --base-virtaddr=0x200000000000 [2023-08-29 10:16:46.486633] --match-allocations [2023-08-29 10:16:46.486655] --file-prefix=spdk_pid2638321 [2023-08-29 10:16:46.486678] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 10:16:46.573581] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 10:16:46.573607] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 10:16:46.719816] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 10:16:46.719886] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 10:16:46.765526] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 10:16:46.765581] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x55ba8c5a9b40
[2023-08-29 10:16:46.807667] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x55ba8cb25b20, a new poll group = 0x55ba8cb25ef0
[2023-08-29 10:16:46.807732] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 10:16:46.807753] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 10:16:46.807802] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 10:16:46.807819] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 10:16:46.808144] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 10:16:46.810838] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x55ba8cc33380, ctrlr is 0x55ba8cb25b20, group is 0x55ba8cb25ef0
[2023-08-29 10:16:46.810876] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 10:16:46.825249] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 10:16:46.825278] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 10:16:46.825288] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 10:16:47.767841] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 15639
[2023-08-29 10:16:48.770172] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16547
[2023-08-29 10:16:49.772503] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16738
[2023-08-29 10:16:50.774833] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16614
[2023-08-29 10:16:51.777164] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16588
[2023-08-29 10:16:52.779495] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16508
[2023-08-29 10:16:53.781826] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16402
[2023-08-29 10:16:54.784156] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16024
[2023-08-29 10:16:55.786487] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16139
[2023-08-29 10:16:56.788818] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16247
[2023-08-29 10:16:57.791149] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16437
[2023-08-29 10:16:58.793486] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16265
[2023-08-29 10:16:59.795817] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16169
[2023-08-29 10:17:00.798148] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16380
[2023-08-29 10:17:01.800479] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16397
[2023-08-29 10:17:02.802811] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16419
[2023-08-29 10:17:03.805141] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16405
[2023-08-29 10:17:04.807473] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16418
[2023-08-29 10:17:05.809804] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16627
[2023-08-29 10:17:06.812135] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16656
ops is: 16380, average latency is 61.050061us
latency histogram: min:48.000000us 10.000000%:55.100952us 50.000000%:57.051429us 90.000000%:60.952381us 95.000000%:63.390476us 99.000000%:82.407619us 99.900000%:721.676190us max:13263.000000us
[2023-08-29 10:17:06.812283] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```

### 写不同对象，aio bdev:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 10:20:41.261241] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 10:20:41.261348] [ DPDK EAL parameters: [2023-08-29 10:20:41.261373] fbbench [2023-08-29 10:20:41.261392] --no-shconf [2023-08-29 10:20:41.261411] -c 0x8 [2023-08-29 10:20:41.261427] --huge-unlink [2023-08-29 10:20:41.261443] --log-level=lib.eal:6 [2023-08-29 10:20:41.261460] --log-level=lib.cryptodev:5 [2023-08-29 10:20:41.261477] --log-level=user1:6 [2023-08-29 10:20:41.261494] --iova-mode=pa [2023-08-29 10:20:41.261520] --base-virtaddr=0x200000000000 [2023-08-29 10:20:41.261547] --match-allocations [2023-08-29 10:20:41.261574] --file-prefix=spdk_pid2639353 [2023-08-29 10:20:41.261596] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 10:20:41.329727] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 10:20:41.329753] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 10:20:41.476886] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 10:20:41.476958] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 10:20:41.522550] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 10:20:41.522602] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x562583721b40
[2023-08-29 10:20:41.563563] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x562583c9db20, a new poll group = 0x562583c9def0
[2023-08-29 10:20:41.563627] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 10:20:41.563647] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 10:20:41.563699] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 10:20:41.563716] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 10:20:41.564039] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 10:20:41.566709] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x562583dab380, ctrlr is 0x562583c9db20, group is 0x562583c9def0
[2023-08-29 10:20:41.566746] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 10:20:41.581437] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 10:20:41.581475] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 10:20:41.581491] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 10:20:42.524873] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 13622
[2023-08-29 10:20:43.527213] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14373
[2023-08-29 10:20:44.529551] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14429
[2023-08-29 10:20:45.531890] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14254
[2023-08-29 10:20:46.534229] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14289
[2023-08-29 10:20:47.536569] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14068
[2023-08-29 10:20:48.538908] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14341
[2023-08-29 10:20:49.541248] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 13999
[2023-08-29 10:20:50.543588] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14317
[2023-08-29 10:20:51.545927] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14081
[2023-08-29 10:20:52.548266] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14073
[2023-08-29 10:20:53.550607] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14102
[2023-08-29 10:20:54.552946] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14192
[2023-08-29 10:20:55.555285] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14263
[2023-08-29 10:20:56.557624] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14302
[2023-08-29 10:20:57.559964] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14245
[2023-08-29 10:20:58.562303] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14048
[2023-08-29 10:20:59.564643] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14258
[2023-08-29 10:21:00.566984] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 13872
[2023-08-29 10:21:01.569323] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14357
ops is: 14174, average latency is 70.551714us
latency histogram: min:57.000000us 10.000000%:63.390476us 50.000000%:65.340952us 90.000000%:70.704762us 95.000000%:74.118095us 99.000000%:89.234286us 99.900000%:2200.137143us max:13916.000000us
[2023-08-29 10:21:01.569460] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```

### 一副本、二副本数据总结: 
- 在所有情况下，nvme bdev都比aio bdev快12%-15%左右，用户态驱动在测试中优于内核驱动, 但用户态驱动不利于观测磁盘IO情况，相关工具目前还较欠缺;  
- 写相同对象比写不同对象更快，猜测可能是因为免掉了状态机apply时所需的新创建blob的耗时;  
- 在一副本、写相同对象的情况下，延迟达到28us，此处基本可以看到磁盘延迟的平均延迟为28-11=17us，目前基于当前P4510单线程的fio测试，4k随机写平均延迟为11.15，似乎我们的延迟没有包含状态机apply的延迟？需要查证。
- 尾延迟情况比较严重: 基于P4510的单线程测试显示，max为2145us，而我们的单线程测试，max通常都到了10000us以上，甚至20000us，尾延迟远略于平均延迟


## 单PG三副本测试:
### 写相同对象， aio bdev:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 4 -m 0x8 -T write
[2023-08-29 11:01:31.616764] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 11:01:31.616883] [ DPDK EAL parameters: [2023-08-29 11:01:31.616913] fbbench [2023-08-29 11:01:31.616938] --no-shconf [2023-08-29 11:01:31.616960] -c 0x8 [2023-08-29 11:01:31.616982] --huge-unlink [2023-08-29 11:01:31.617003] --log-level=lib.eal:6 [2023-08-29 11:01:31.617024] --log-level=lib.cryptodev:5 [2023-08-29 11:01:31.617045] --log-level=user1:6 [2023-08-29 11:01:31.617070] --iova-mode=pa [2023-08-29 11:01:31.617094] --base-virtaddr=0x200000000000 [2023-08-29 11:01:31.617115] --match-allocations [2023-08-29 11:01:31.617137] --file-prefix=spdk_pid2657775 [2023-08-29 11:01:31.617160] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 11:01:31.702271] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 11:01:31.702297] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 11:01:31.848538] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 11:01:31.848606] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 11:01:31.894218] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 11:01:31.894268] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x5628fb0a0b40
[2023-08-29 11:01:31.963116] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x5628fb61cb20, a new poll group = 0x5628fb61cef0
[2023-08-29 11:01:31.963195] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 11:01:31.963225] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 11:01:31.963296] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 11:01:31.963321] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 11:01:31.963730] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 11:01:31.966724] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x5628fb72a380, ctrlr is 0x5628fb61cb20, group is 0x5628fb61cef0
[2023-08-29 11:01:31.966764] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 11:01:31.982481] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 11:01:31.982523] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 11:01:31.982540] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 11:01:32.896564] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9982
[2023-08-29 11:01:33.898927] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9911
[2023-08-29 11:01:34.901289] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9907
[2023-08-29 11:01:35.903652] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9931
ops is: 9932, average latency is 100.684656us
latency histogram: min:57.000000us 10.000000%:92.647619us 50.000000%:100.449524us 90.000000%:102.887619us 95.000000%:104.838095us 99.000000%:130.681905us 99.900000%:157.988571us max:15285.000000us
[2023-08-29 11:01:35.903791] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```
### 写不同对象， aio bdev:
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 11:11:37.992410] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 11:11:37.992536] [ DPDK EAL parameters: [2023-08-29 11:11:37.992562] fbbench [2023-08-29 11:11:37.992583] --no-shconf [2023-08-29 11:11:37.992602] -c 0x8 [2023-08-29 11:11:37.992621] --huge-unlink [2023-08-29 11:11:37.992640] --log-level=lib.eal:6 [2023-08-29 11:11:37.992658] --log-level=lib.cryptodev:5 [2023-08-29 11:11:37.992678] --log-level=user1:6 [2023-08-29 11:11:37.992698] --iova-mode=pa [2023-08-29 11:11:37.992717] --base-virtaddr=0x200000000000 [2023-08-29 11:11:37.992737] --match-allocations [2023-08-29 11:11:37.992755] --file-prefix=spdk_pid2664940 [2023-08-29 11:11:37.992775] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 11:11:38.083031] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 11:11:38.083063] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 11:11:38.229339] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 11:11:38.229410] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 11:11:38.275099] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 11:11:38.275153] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x556e884cdb40
[2023-08-29 11:11:38.318533] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x556e88a49b20, a new poll group = 0x556e88a49ef0
[2023-08-29 11:11:38.318600] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 11:11:38.318621] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 11:11:38.318674] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 11:11:38.318693] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 11:11:38.319047] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 11:11:38.321939] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x556e88b57380, ctrlr is 0x556e88a49b20, group is 0x556e88a49ef0
[2023-08-29 11:11:38.321972] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 11:11:38.337897] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 11:11:38.337931] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 11:11:38.337943] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 11:11:39.277443] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14316
[2023-08-29 11:11:40.279803] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 15200
[2023-08-29 11:11:41.282162] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 15158
[2023-08-29 11:11:42.284521] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 15082
[2023-08-29 11:11:43.286880] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14997
[2023-08-29 11:11:44.289240] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14932
[2023-08-29 11:11:45.291601] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14875
[2023-08-29 11:11:46.293962] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14772
[2023-08-29 11:11:47.296322] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14772
[2023-08-29 11:11:48.298681] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14772
[2023-08-29 11:11:49.301040] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14745
[2023-08-29 11:11:50.303400] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14758
[2023-08-29 11:11:51.305759] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14768
[2023-08-29 11:11:52.308118] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14754
[2023-08-29 11:11:53.310477] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14743
[2023-08-29 11:11:54.312839] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14593
[2023-08-29 11:11:55.315199] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14720
[2023-08-29 11:11:56.317559] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14734
[2023-08-29 11:11:57.319917] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14664
[2023-08-29 11:11:58.322277] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 14754
ops is: 14805, average latency is 67.544748us
latency histogram: min:57.000000us 10.000000%:63.878095us 50.000000%:66.316190us 90.000000%:71.192381us 95.000000%:74.118095us 99.000000%:83.870476us 99.900000%:247.710476us max:15280.000000us
[2023-08-29 11:11:58.322433] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```


### 写相同对象, nvme bdev
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 11:19:57.348156] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 11:19:57.348294] [ DPDK EAL parameters: [2023-08-29 11:19:57.348323] fbbench [2023-08-29 11:19:57.348345] --no-shconf [2023-08-29 11:19:57.348367] -c 0x8 [2023-08-29 11:19:57.348389] --huge-unlink [2023-08-29 11:19:57.348411] --log-level=lib.eal:6 [2023-08-29 11:19:57.348432] --log-level=lib.cryptodev:5 [2023-08-29 11:19:57.348453] --log-level=user1:6 [2023-08-29 11:19:57.348475] --iova-mode=pa [2023-08-29 11:19:57.348497] --base-virtaddr=0x200000000000 [2023-08-29 11:19:57.348519] --match-allocations [2023-08-29 11:19:57.348540] --file-prefix=spdk_pid2666166 [2023-08-29 11:19:57.348562] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 11:19:57.439058] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 11:19:57.439086] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 11:19:57.585275] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 11:19:57.585349] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 11:19:57.630925] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 11:19:57.630976] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x5612550c3b40
[2023-08-29 11:19:57.669876] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x56125563fb20, a new poll group = 0x56125563fef0
[2023-08-29 11:19:57.669941] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 11:19:57.669962] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 11:19:57.670014] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 11:19:57.670031] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 11:19:57.670276] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 11:19:57.673013] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x56125574d380, ctrlr is 0x56125563fb20, group is 0x56125563fef0
[2023-08-29 11:19:57.673049] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 11:19:57.685727] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 11:19:57.685761] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 11:19:57.685775] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 11:19:58.633266] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 10556
[2023-08-29 11:19:59.635622] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9923
[2023-08-29 11:20:00.637980] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9911
[2023-08-29 11:20:01.640336] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9915
[2023-08-29 11:20:02.642692] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9914
[2023-08-29 11:20:03.645048] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9894
[2023-08-29 11:20:04.647405] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9898
[2023-08-29 11:20:05.649761] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9931
[2023-08-29 11:20:06.652117] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9888
[2023-08-29 11:20:07.654473] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9925
[2023-08-29 11:20:08.656829] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9903
[2023-08-29 11:20:09.659185] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9886
[2023-08-29 11:20:10.661541] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9923
[2023-08-29 11:20:11.663897] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9901
[2023-08-29 11:20:12.666254] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9897
[2023-08-29 11:20:13.668610] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9889
[2023-08-29 11:20:14.670967] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9886
[2023-08-29 11:20:15.673323] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9910
[2023-08-29 11:20:16.675679] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9893
[2023-08-29 11:20:17.678036] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 9892
ops is: 9936, average latency is 100.644122us
latency histogram: min:50.000000us 10.000000%:98.499048us 50.000000%:100.449524us 90.000000%:102.400000us 95.000000%:104.838095us 99.000000%:129.706667us 99.900000%:171.641905us max:11793.000000us
[2023-08-29 11:20:17.678199] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```

### 写不同对象, nvme bdev
```
root@ceph144:~/fb/fastblock/build/tools# ./fbbench -o 172.31.77.144 -t 8001 -S 4096 -k 20 -m 0x8 -T write
[2023-08-29 11:21:30.058872] Starting SPDK v22.05 git sha1 395c61a42 / DPDK 21.08.0 initialization...
[2023-08-29 11:21:30.058977] [ DPDK EAL parameters: [2023-08-29 11:21:30.058999] fbbench [2023-08-29 11:21:30.059017] --no-shconf [2023-08-29 11:21:30.059032] -c 0x8 [2023-08-29 11:21:30.059045] --huge-unlink [2023-08-29 11:21:30.059062] --log-level=lib.eal:6 [2023-08-29 11:21:30.059078] --log-level=lib.cryptodev:5 [2023-08-29 11:21:30.059095] --log-level=user1:6 [2023-08-29 11:21:30.059111] --iova-mode=pa [2023-08-29 11:21:30.059127] --base-virtaddr=0x200000000000 [2023-08-29 11:21:30.059144] --match-allocations [2023-08-29 11:21:30.059160] --file-prefix=spdk_pid2666283 [2023-08-29 11:21:30.059177] ]
EAL: No free 2048 kB hugepages reported on node 1
EAL: No available 1048576 kB hugepages reported
TELEMETRY: No legacy callbacks, legacy socket not created
[2023-08-29 11:21:30.147844] app.c: 607:spdk_app_start: *NOTICE*:
[2023-08-29 11:21:30.147875] app.c: 608:spdk_app_start: *NOTICE*: Total cores available: 1
[2023-08-29 11:21:30.293947] reactor.c: 947:reactor_run: *NOTICE*: Reactor started on core 3
[2023-08-29 11:21:30.294017] accel_engine.c: 969:sw_accel_engine_init: *NOTICE*: Accel framework software engine initialized.
[2023-08-29 11:21:30.339647] /root/fb/fastblock/tools/fbbench.cc:  92:fbbench_started: *NOTICE*: ------block start, cpu count : 1
[2023-08-29 11:21:30.339700] /root/fb/fastblock/src/msg/transport_client.h: 422:transport_client: *NOTICE*: construct transport_client, this is 0x55fda6d3eb40
[2023-08-29 11:21:30.385457] /root/fb/fastblock/src/msg/transport_client.h: 469:start: *NOTICE*: Construct a new ctrlr = 0x55fda72bab20, a new poll group = 0x55fda72baef0
[2023-08-29 11:21:30.385535] /root/fb/fastblock/tools/fbbench.h:  93:create_connect: *NOTICE*: create connect to node 0 (address 172.31.77.144, port 8001) in core 3
[2023-08-29 11:21:30.385561] /root/fb/fastblock/src/msg/transport_client.h: 171:connect: *NOTICE*: Connecting to 172.31.77.144:8001...
[2023-08-29 11:21:30.385617] client.c: 454:spdk_client_ctrlr_alloc_io_qpair_async: *NOTICE*: spdk_client_ctrlr_alloc_io_qpair : io_queue_size 128
[2023-08-29 11:21:30.385639] rdma_c.c:2304:client_rdma_ctrlr_create_io_qpair: *NOTICE*: client_rdma_ctrlr_create_io_qpair io_queue_size: 128 io_queue_requests: 4096
[2023-08-29 11:21:30.385915] rdma_c.c:2046:client_rdma_ctrlr_create_qpair: *NOTICE*: client_rdma_ctrlr_create_qpair num_entries: 128
[2023-08-29 11:21:30.388913] /root/fb/fastblock/src/msg/transport_client.h: 191:connect: *NOTICE*: Conneting to 172.31.77.144:8001 done, conn is 0x55fda73c8380, ctrlr is 0x55fda72bab20, group is 0x55fda72baef0
[2023-08-29 11:21:30.388948] /root/fb/fastblock/src/msg/transport_client.h: 488:emplace_connection: *NOTICE*: Connecting to 172.31.77.144:8001
[2023-08-29 11:21:30.401975] rdma_c.c: 574:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries before: 128
[2023-08-29 11:21:30.402011] rdma_c.c: 578:client_rdma_qpair_process_cm_event: *NOTICE*: client_rdma_qpair_process_cm_event num_entries: 128 128
[2023-08-29 11:21:30.402027] rdma_c.c:1226:client_rdma_register_reqs: *NOTICE*: client_rdma_register_reqs: 128
[2023-08-29 11:21:31.341988] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16217
[2023-08-29 11:21:32.344349] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 17115
[2023-08-29 11:21:33.346710] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 17165
[2023-08-29 11:21:34.349067] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 17123
[2023-08-29 11:21:35.351424] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 17081
[2023-08-29 11:21:36.353780] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 17026
[2023-08-29 11:21:37.356136] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16867
[2023-08-29 11:21:38.358492] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16832
[2023-08-29 11:21:39.360849] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16843
[2023-08-29 11:21:40.363206] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16824
[2023-08-29 11:21:41.365562] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16798
[2023-08-29 11:21:42.367918] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16847
[2023-08-29 11:21:43.370275] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16853
[2023-08-29 11:21:44.372632] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16756
[2023-08-29 11:21:45.374988] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16723
[2023-08-29 11:21:46.377344] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16510
[2023-08-29 11:21:47.379701] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16755
[2023-08-29 11:21:48.382058] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16817
[2023-08-29 11:21:49.384414] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16785
[2023-08-29 11:21:50.386771] /root/fb/fastblock/tools/fbbench.cc:  66:print_stats: *NOTICE*: last second processed: 16759
ops is: 16834, average latency is 59.403588us
latency histogram: min:50.000000us 10.000000%:56.076190us 50.000000%:58.270476us 90.000000%:62.171429us 95.000000%:63.878095us 99.000000%:70.217143us 99.900000%:133.607619us max:13665.000000us
[2023-08-29 11:21:50.386929] /root/fb/fastblock/src/msg/transport_client.h: 434:~transport_client: *NOTICE*: destruct transport_client
```
### 监控视角的压测图表
![Alt text](fbbench_prometheus.png)
在这一测试中，我们采用的是aio bdev，从此图中可以看出， 在一分钟(14:34开始)的压测过程中，三块磁盘的写入次数在17000左右，其中包含了恒定的1000左右的apply io，在压测结束之后，还进行了15分钟的apply操作，原因是目前版本的raft的recover存在bug，在某个raft节点的log因为写log慢了而无法完全与leader的log匹配时，raft没办法恢复，所以退化为两副本运行，所以本次测试没有开启全速的apply，导致apply index和commit index之间差了很多。  



### 三副本数据总结: 
- 在所有情况下，nvme bdev与aio bdev相比，在写相同对象时几乎没区别，但在写不同对象时有10%左右差距，暂时没有好的解释;
- 写相同对象比写不同对象更慢，这一点与一副本和二副本矛盾，这里存疑；
- 三副本相比8.17测试数据，有巨大提升，三副本在写不同对象的情况下首次跨入100us，最优情况达到了59.4us，显示出本项目有长期调优的基础.

