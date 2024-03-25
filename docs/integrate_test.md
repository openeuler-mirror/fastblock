本文主要介绍进行monitor和osd整体联调的步骤。  
# monitor
monitor需要自行编译，代码在`git@gitee.com:custorage/monitor.git`, 编译方式为:  
```
export GOPROXY=https://proxy.golang.com.cn,direct
cd monitor
make
```
然后需要创建monitor所需的目录:   
```
mkdir -p /etc/fastblock
mkdir -p /var/log/fastblock
```
编辑/etc/fastblock/monitor.json形如:  
```
{
    "monitors": ["monitor1"],
    "mon_host": ["173.20.4.1"],
    "log_path": "/var/log/fastblock/monitor1.log",
    "log_level": "info",
    "election_master_key": "fastblock_monitor_election"
}
```
默认情况下，monitor会绑定3333端口作为元数据rpc的端口，另有3332作为Prometheus端口(相关功能暂未完整开发)  
配置好之后，启动monitor:
```
./fastblock-mon -conf=/etc/fastblock/monitor.json -id=monitor1 
```

# osd
## 1.需要进行osd的编译:  
```
./install-deps.sh
./build.sh
cd build
make
```
## 2.配置所需的rdma环境(如有rdma设备则这一步可跳过)
当前版本的fastblock osd仅支持RDMA作为传输层，为了适配一些CI环境，使用linux kernel的softroce模拟RDMA环境:  
```
# 这一步需要5.x以上的内核，ubuntu 22.04测试通过:
# 具体请参考https://access.redhat.com/documentation/zh-cn/red_hat_enterprise_linux/8/html/configuring_infiniband_and_rdma_networks/configuring-soft-roce_configuring-roce
modprobe rdma_rxe
rdma link add rxe1 type rxe netdev eth0
```

## 3.配置启动osd所需的huge page： 
内存请量力而行
```
echo 1024 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
```

## 4.为要启动的osd申请一个id:
```
cd monitor
./fbclient -op=fakeapplyid -uuid=`uuidgen`
```
## 5.使用上述uuid启动osd:
```
# 其中ip为上述eth0的地址
./build/src/osd/osd -I $id -U $uuid -t $port -o $ip
```
## 6.创建一个pool:
```
cd monitor
./fbclient -op=createpool -pgcount=1 -pgsize=1 -poolname=asdf
```

# 进行小demo验证
查看demo中，能否实现客户端到osd端的rpc通信:
```
./build/src/osd/demo/demo -o $ip
```



