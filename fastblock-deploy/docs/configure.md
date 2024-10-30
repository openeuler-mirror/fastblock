## 配置文件说明
### 1. Inventory文件
Ansible 使用 Inventory文件定义自动化的受管节点，并可将变量分配给单个主机。`fastblock-deploy/hosts` 为 fastblock-deploy 使用的Inventory文件。
`fastblock-deploy/hosts` 文件中各参数含义如下，可根据实际业务场景定义该文件：
```
[monitors]
monitor主机1别名 ansible_host=monitor主机1地址
monitor主机2别名 ansible_host=monitor主机2地址
monitor主机3别名 ansible_host=monitor主机3地址
...

[osds]
osd主机1别名 ansible_host=osd主机1地址 disks=osd主机1磁盘(使用逗号分隔) nic=osd主机1的rdma设备
osd主机2别名 ansible_host=osd主机2地址 disks=osd主机2磁盘(使用逗号分隔) nic=osd主机2的rdma设备
...

[clients]
client主机1别名 ansible_host=client主机1地址
...

```

例如：
```
[monitors]
monitor1 ansible_host=192.168.3.181
monitor2 ansible_host=192.168.3.182
monitor3 ansible_host=192.168.3.183

[osds]
osd1 ansible_host=192.168.3.191 disks=/dev/nvme0n1,/dev/nvme0n2 nic=rxe1
osd2 ansible_host=192.168.3.192 disks=/dev/nvme0n1 nic=rxe1

[clients]
client1 ansible_host=192.168.3.180
```

### 2. fastblock部署配置
`fastblock-deploy/roles/fastblock-defaults/defaults/main.yml` 中保存fastblock部署过程中使用的默认配置项，如bdev_type。

### 3. fastblock-deploy.sh脚本
`fastblock-deploy/fastblock-deploy.sh` 脚本将完成ansible相关环境准备。
例如：
```bash
./fastblock-deploy.sh -m "192.168.3.181,192.168.3.182,192.168.3.183" -o "192.168.3.191,192.168.3.192" -c "192.168.3.180" -d "192.168.3.191:/dev/nvme0n1,/dev/nvme0n2 192.168.3.192:/dev/nvme0n1,/dev/nvme0n2" -n rxe1 -t aio
```
