iSCSI（Internet SCSI）是一种在IP网络上运行SCSI协议的网络存储技术。它通过TCP/IP技术实现存储设备端与客户端之间的连接。具体
而言，存储设备端通过iSCSI Target提供磁盘服务，而客户端则通过iSCSI Initiator连接并使用这些存储设备。

fastblock的iscsi_tgt应用可用配置iscsi target，把fastblock的bdev设备作为存储设备为iscsi target提供磁盘服务，客户端通过
iSCSI Initiator连接并使用这些磁盘。

iscsi有接个关键的概念：
- Portal: 是iSCSI Target的一个网络接口，用于接收来自iSCSI Initiator的连接请求。Portal通常由IP地址和端口号组成，
标准的iSCSI端口是3260。一个iSCSI Target可以有多个Portal，以便为不同的网络配置提供冗余和负载均衡。
- Portal Group： 一个包含多个Portal的集合，以便为iSCSI Target提供冗余。通过Portal Group，可以将多个Portal视为
一个单元，以实现负载均衡和故障转移。如果一个Portal不可用，iSCSI Initiator可以通过同一Portal Group中的其他Portal
继续访问存储资源。
- Portal Group Tag： 一个唯一的标识符，用来标识一个Portal Group。
- Initiator Group：一个iSCSI Initiator集合。每个Initiator Group可以配置不同的访问权限和策略。
- Initiator Group Tag：一个唯一的标识符，用来标识一个Initiator Group。

使用方法：
# 1 部署fastblock集群
部署fastblock方法参考qemu_vhost_test.md文件，这里不再重复。

# 2 创建pool
```
fastblock-client -op=createpool -poolname=fb -pgcount=6 -pgsize=3 -failure_domain="osd"
fastblock-client -op=createimage -imagesize=$((100*1024*1024*1024))  -imagename=fbimage -poolname=fb
```

# 3 启动iscsi_tgt应用
iscsi_tgt的配置文件iscsi_tgt.json：
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
fastblock-iscsi-tgt -C iscsi_tgt.json -S 1   
```
参数说明：
-C：  配置文件
-S：  绑定的cpu核数量

# 4 使用rpc.py创建bdev设备
创建bdev设备
```
spdk/scripts/rpc.py -s /var/tmp/fastblock_iscsi_tgt_3217499.sock  bdev_fastblock_create -P  1 -p fb -i fbimage -k 4096 -I 100G -m "175.5.24.252:3333" -b fbdev
```
参数说明：
  -s /var/tmp/fastblock_iscsi_tgt_3217499.sock： 是/var/tmp目录下fastblock_iscsi_tgt_${fastblock-iscsi-tgt进程id}.sock
  -P ： pool id
  -p ： pool名字
  -i ： image名字
  -k ： image的block大小
  -I ： image的大小
  -m ： monitor地址
  -b ： bdev名字

# 4 创建Portal Group
为iscsi创建Portal Group
```
spdk/scripts/rpc.py -s /var/tmp/fastblock_iscsi_tgt_3217499.sock iscsi_create_portal_group 1 175.5.24.252:3260
```
参数说明：
  -s /var/tmp/fastblock_iscsi_tgt_3217499.sock： 是/var/tmp目录下fastblock_iscsi_tgt_${fastblock-iscsi-tgt进程id}.sock
  参数1： Portal Group Tag，用来标识一个Portal Group的唯一标识符
  参数2： Portal列表，可以包含多个Portal（双引号括起来，portal之间用空格分隔）。Portal由IP地址和端口号组成

# 5 创建initiator group
为iscsi创建initiator group
```
spdk/scripts/rpc.py -s /var/tmp/fastblock_iscsi_tgt_3217499.sock iscsi_create_initiator_group 2 ANY 175.5.24.252/32
```
参数说明：
  -s /var/tmp/fastblock_iscsi_tgt_3217499.sock： 是/var/tmp目录下fastblock_iscsi_tgt_${fastblock-iscsi-tgt进程id}.sock
  参数1： initiator group Tag，用来标识一个initiator group的唯一标识符  
  参数2： initiator列表。可以是ANY，表示可以任意initiator；可以是一个或多个initiator（双引号括起来，initiator之间用空格分隔）。
  参数3： initiator netmasks列表（双引号括起来，netmasks之间用空格分隔）。

# 6 创建target node
为iscsi创建target node
```
spdk/scripts/rpc.py -s /var/tmp/fastblock_iscsi_tgt_3217499.sock iscsi_create_target_node disk1 "Data Disk1" "fbdev:0" 1:2 64 -d
```
参数说明
  参数1： Target node名字
  参数2： Target node别名
  参数3： bdev_name_id_pairs，bdev名字:LUN ID，可以包含多对（双引号括起来，之间用空格分隔）
  参数4： pg_ig_mappings，Target node使用的Portal Group Tag和Initiator Group Tag，使用“:”分隔，可以包含多对（双引号括起来，之间用空格分隔）
  参数5： queue_depth，目标队列深度。
  -d : 关闭CHAP认证

# 7 安装open-iscsi 
在fedora上： yum install -y iscsi-initiator-utils
在ubuntu上： apt-get install -y open-iscsi

# 8 发现target
```
iscsiadm -m discovery -t sendtargets -p 172.31.4.142
```

# 9 连接target
```
iscsiadm -m node --login
```
此时，iSCSI目标应显示为 SCSI 磁盘，通过lsblk可以看到。

# 10 断开连接target
```
iscsiadm -m node --logout
```