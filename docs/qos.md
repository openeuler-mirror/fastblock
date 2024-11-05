本文介绍如何配置fastblock块设备的qos配置，因为fastblock导出的块设备本质上一个spdk bdev，所以可以利用spdk bdev的qos机制来设置qos来限制块设备的iops和带宽。从导出的接口来看，fastblock的块设备主要是通过fastblock-vhost和fastblock-nvmf-tgt两种方式导出，这两个进程都会导出一个socket，可通过这个socket进行qos参数的配置，下面以fastblock-nvmf-tgt为例进行介绍。

使用方法：
# 1 部署fastblock集群
部署fastblock方法参考[vhost对接qemu测试](./qemu_vhost_test.md "vhost对接")，这里不再重复。

# 2 启动fastblock-nvmf-tgt及linux initiator
配置、启动fastblock-nvmf-tgt的方法以及启动linux nvmf initiator参考[nvmf对接测试](./nvmf_tgt.md "nvmf对接")，这里不再重复。

# 3 配置fastblock bdev qos参数以及fio验证qos参数是否生效
使用下面命令行进行qos参数配置:  
```
spdk/scripts/rpc.py -s /var/tmp/fastblock_nvmf_tgt1406627.sock  bdev_set_qos_limit --rw-ios-per-sec 200000 fbdev
```
再使用fio验证qos是否生效:  
```
taskset -c 12,13,14,15 fio -direct=1 -iodepth=32 -thread -rw=randwrite -bs=4096 -numjobs=4 -runtime=360000 -group_reporting  -name=test -filename=/dev/nvme12n1 -ioengine=libaio  -time_based
```
使用taskset避免fio跟fastblock-nvmf-tgt跑在同一个核上。  

另外，通过bdev_set_qos_limit还可以进行读写带宽的qos参数配置，通过帮助信息即可, 此处不再赘述。  
```
# spdk/scripts/rpc.py  bdev_set_qos_limit --help
usage: rpc.py [options] bdev_set_qos_limit [-h] [--rw-ios-per-sec RW_IOS_PER_SEC] [--rw-mbytes-per-sec RW_MBYTES_PER_SEC] [--r-mbytes-per-sec R_MBYTES_PER_SEC]
                                           [--w-mbytes-per-sec W_MBYTES_PER_SEC]
                                           name

positional arguments:
  name                  Blockdev name to set QoS. Example: Malloc0

optional arguments:
  -h, --help            show this help message and exit
  --rw-ios-per-sec RW_IOS_PER_SEC
                        R/W IOs per second limit (>=1000, example: 20000). 0 means unlimited.
  --rw-mbytes-per-sec RW_MBYTES_PER_SEC
                        R/W megabytes per second limit (>=1, example: 100). 0 means unlimited.
  --r-mbytes-per-sec R_MBYTES_PER_SEC
                        Read megabytes per second limit (>=1, example: 100). 0 means unlimited.
  --w-mbytes-per-sec W_MBYTES_PER_SEC
                        Write megabytes per second limit (>=1, example: 100). 0 means unlimited.
```
