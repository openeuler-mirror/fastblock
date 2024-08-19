在鲲鹏920 + openeuler系统上使用虚拟机对接fastblock的vhost

# 1 环境
cpu:  Kunpeng-920   128核
系统： openEuler 22.03 (LTS-SP2)

# 2 部署fastblock集群
## 2.1 部署monitor
monitor运行在node1上，monitor配置文件 fastblock.json如下:
```json
{
  "monitors": ["monitor"],
  "mon_host": ["ip"],
  "log_path": "/var/log/fastblock/monitor1.log",
  "log_level": "info",
  "election_master_key": "fastblock_monitor_election"
}
```

启动monitor进程:  
```
fastblock-mon -conf=fastblock.json -id=monitor &
```
注意：这里的id要和配置文件中的monitors字段值一致

## 2.2 部署osd
osd的配置文件是json格式的：
```json
{
    "monitors": ["monitor"],
    "mon_host": ["ip"],
    "log_path": "/var/log/fastblock/monitor1.log",
    "log_level": "info",
    "election_master_key": "fastblock_monitor_election",
    "mon_rpc_address": "ip",
    "mon_rpc_port": 3333,
    "rdma_device_name": "rocep125s0f0",
    "msg_server_listen_backlog" : 1024,
    "msg_server_poll_cq_batch_size": 32,
    "msg_server_metadata_memory_pool_capacity": 8192,
    "msg_server_metadata_memory_pool_element_size": 1024,
    "msg_server_data_memory_pool_capacity": 8192,
    "msg_server_data_memory_pool_element_size": 8192,
    "msg_server_per_post_recv_num": 512,
    "msg_server_rpc_timeout_us": 1000000,
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

配置3个osd
```
for osdid in 1 2 3
do
    uuid=`uuidgen`
    fastblock-client -op=fakeapplyid -uuid=${uuid}
    fastblock-osd -s 2048 -S 2 -C fastblock.json --mkfs --id ${osdid} --uuid ${uuid} --force 
    nohup fastblock-osd -s 2048 -C fastblock.json  --id ${osdid}  >> /var/log/fastblock/osd${osdid}.log  2>&1 &
done
```

## 2.3 创建pool和image
当3个osd都是up/in状态后，再创建pool
```
root@node1 /home/liuhongquan $ fastblock-client -op=getosdmap
OSDID        ADDRESS                PORT          STATUS-UP    STATUS-IN
1            175.5.24.252           9254          up           in
2            175.5.24.252           9641          up           in
3            175.5.24.252           9908          up           in
```

创建pool：
```
fastblock-client -op=createpool -poolname=fb -pgcount=6 -pgsize=3 -failure_domain="osd"
fastblock-client -op=createimage -imagesize=$((100*1024*1024*1024))  -imagename=fbimage -poolname=fb
```
# 3 启动vhost
vhost的配置文件vhost.json：
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
启动vhost:
```
nohup fastblock/build/src/bdev/fastblock-vhost -m '[8]' -L libblk -C vhost.json >> /var/log/fastblock/bdev.log  2>&1 &
```

创建bdev设备
```
fastblock/build/deps_build/spdk-prefix/src/spdk/scripts/rpc.py -s /var/tmp/bdev_vhost_192944.sock  bdev_fastblock_create -P 1 -p fb -i fbimage -k 4096 -I 100G -m "175.5.24.252:3333" -b fbdev
```
其中：-s /var/tmp/bdev_vhost_192944.sock 是/var/tmp目录下bdev_vhost_${vhost进程id}.sock

创建设备controller
```
fastblock/build/deps_build/spdk-prefix/src/spdk/scripts/rpc.py -s /var/tmp/bdev_vhost_192944.sock vhost_create_blk_controller --cpumask 0x100 vhost.1 fbdev
```
注意：如果--cpumask的值写错，会返回错误（"Invalid argument"），并且在vhost日志中会报错并且给出正确的cpumask。

创建完成后，会在当前目录下生成一个“vhost.1”文件，在启动qemu时会用到

# 4 启动qemu
## 4.1 安装qemu相关软件

```
yum install virt-manager qemu qemu-* libvirt -y
```

针对不同的架构，linux系统引导的方式有所差异:x86支持UEFI（Unified Extensible Firmware Interface）和BIOS方式启动，AArch64仅支持UEFI方式启动。统一的可扩展固件接口UEFI是一种全新类型的接口标准，用于开机自检、引导操作系统的启动。EDK II是一套实现了UEFI标准的开源代码，在虚拟化场景中，通常利用EDK II工具集，通过UEFI的方式启动虚拟机。
在鲲鹏920 + openeuler系统上，安装edk2:
```
yum install edk2-aarch64 edk2-devel.aarch64 -y
```
UEFI固件位置：/usr/share/edk2/aarch64/

## 4.2 启动qemu

从https://cloud.centos.org/centos/8-stream/aarch64/images/ 下载aarch64架构的CentOS云镜像：CentOS-Stream-GenericCloud-8-20240520.0.aarch64.qcow2

启动libvirtd服务：
```
systemctl start libvirtd
```

修改CentOS云镜像密码：
```
virt-customize -a CentOS-Stream-GenericCloud-8-20240520.0.aarch64.qcow2 --root-password password:123  
```

启动qemu:
```
taskset -c 36,37,38,39 qemu-system-aarch64 \
        -M virt \
        -cpu cortex-a57   \
        -smp 4 \
        -m 8G  \
        -object memory-backend-file,id=mem0,size=8G,mem-path=/dev/hugepages,share=on -numa node,memdev=mem0 \
        -drive file=/home/liuhongquan/CentOS-Stream-GenericCloud-8-20240520.0.aarch64.qcow2,id=hd0,if=none  \
        -device virtio-scsi-pci,id=scsi0  \
        -device scsi-hd,drive=hd0,bus=scsi0.0 \
        -bios /usr/share/edk2/aarch64/QEMU_EFI.fd \
        -device usb-ehci,id=usb \
        -device usb-tablet,bus=usb.0 \
        -chardev socket,id=spdk_vhost_blk0,path=/home/liuhongquan/vhost.1 \
        -device vhost-user-blk-pci,chardev=spdk_vhost_blk0,num-queues=2 \
        -device virtio-net-pci,netdev=mynet0 \
        -netdev user,id=mynet0,hostfwd=tcp::10022-:22 \
        -vnc 0.0.0.0:1  \
        -nographic
```

其中：
    -cpu 为开启的虚拟机的cpu类型（qemu-system-aarch64 -cpu help查看支持的类型）
    -drive file 指定虚拟机镜像
    -bios  指定UEFI固件
    -chardev socket,id=spdk_vhost_blk0,path=/home/liuhongquan/vhost.1  中vhost.1就是上面创建设备controller生成的
